/*
GamePulse for OBS — Twitch integration implementation.
*/

#include "gp-twitch.h"

#include <cstring>
#include <sstream>

#include <curl/curl.h>

#include <util/base.h>
#include <util/platform.h>

#include "plugin-support.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET tw_socket_t;
#define TW_INVALID_SOCKET INVALID_SOCKET
#define tw_closesocket closesocket
#define tw_shutdown_both(s) ::shutdown(s, SD_BOTH)
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int tw_socket_t;
#define TW_INVALID_SOCKET (-1)
#define tw_closesocket close
#define tw_shutdown_both(s) ::shutdown(s, SHUT_RDWR)
#endif

namespace gamepulse {

namespace {

const char *TWITCH_ID_BASE = "https://id.twitch.tv";
const char *TWITCH_HELIX_BASE = "https://api.twitch.tv/helix";
const char *MARKER_SCOPES = "channel:manage:broadcast";

int64_t mono_ms()
{
	return static_cast<int64_t>(os_gettime_ns() / 1000000ULL);
}

size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	std::string *out = static_cast<std::string *>(userdata);
	out->append(ptr, size * nmemb);
	return size * nmemb;
}

std::string url_encode(const std::string &s)
{
	static const char hex[] = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
		    c == '_' || c == '.' || c == '~') {
			out += static_cast<char>(c);
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

std::string json_string_field(const std::string &body, const char *field)
{
	obs_data_t *d = obs_data_create_from_json(body.c_str());
	if (!d)
		return "";
	const char *v = obs_data_get_string(d, field);
	std::string out = v ? v : "";
	obs_data_release(d);
	return out;
}

} // namespace

TwitchService::TwitchService() = default;

TwitchService::~TwitchService()
{
	shutdown();
}

/* ---------------- config ---------------- */

void TwitchService::load(obs_data_t *root)
{
	obs_data_t *tw = obs_data_get_obj(root, "twitch");
	if (!tw)
		return;
	std::lock_guard<std::mutex> lock(state_mutex_);
	client_id_ = obs_data_get_string(tw, "client_id");
	access_token_ = obs_data_get_string(tw, "access_token");
	refresh_token_ = obs_data_get_string(tw, "refresh_token");
	user_id_ = obs_data_get_string(tw, "user_id");
	login_ = obs_data_get_string(tw, "login");
	expires_at_ms_ = 0; /* force refresh/validate on first use */

	{
		std::lock_guard<std::mutex> clock(chat_mutex_);
		chat_cfg_.enabled = obs_data_get_bool(tw, "chat_enabled");
		const char *cmd = obs_data_get_string(tw, "chat_command");
		if (cmd && *cmd)
			chat_cfg_.command = cmd;
		const char *perm = obs_data_get_string(tw, "chat_permission");
		if (perm && *perm)
			chat_cfg_.permission = perm;
		int cd = (int)obs_data_get_int(tw, "chat_cooldown_s");
		if (cd > 0)
			chat_cfg_.cooldown_s = cd;
		const char *chan = obs_data_get_string(tw, "chat_channel");
		chat_cfg_.channel_override = chan ? chan : "";
	}
	obs_data_release(tw);
}

void TwitchService::save(obs_data_t *root) const
{
	obs_data_t *tw = obs_data_create();
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		obs_data_set_string(tw, "client_id", client_id_.c_str());
		obs_data_set_string(tw, "access_token", access_token_.c_str());
		obs_data_set_string(tw, "refresh_token", refresh_token_.c_str());
		obs_data_set_string(tw, "user_id", user_id_.c_str());
		obs_data_set_string(tw, "login", login_.c_str());
	}
	{
		std::lock_guard<std::mutex> lock(chat_mutex_);
		obs_data_set_bool(tw, "chat_enabled", chat_cfg_.enabled);
		obs_data_set_string(tw, "chat_command", chat_cfg_.command.c_str());
		obs_data_set_string(tw, "chat_permission", chat_cfg_.permission.c_str());
		obs_data_set_int(tw, "chat_cooldown_s", chat_cfg_.cooldown_s);
		obs_data_set_string(tw, "chat_channel", chat_cfg_.channel_override.c_str());
	}
	obs_data_set_obj(root, "twitch", tw);
	obs_data_release(tw);
}

/* ---------------- lifecycle ---------------- */

void TwitchService::start_workers()
{
	if (workers_running_)
		return;
	curl_global_init(CURL_GLOBAL_DEFAULT);
	workers_running_ = true;
	marker_thread_ = std::thread([this]() { marker_worker(); });
	apply_chat_state();
}

void TwitchService::shutdown()
{
	cancel_device_auth();

	if (workers_running_) {
		workers_running_ = false;
		queue_cv_.notify_all();
		if (marker_thread_.joinable())
			marker_thread_.join();
	}

	irc_should_run_ = false;
	intptr_t s = irc_socket_.exchange((intptr_t)TW_INVALID_SOCKET);
	if (s != (intptr_t)TW_INVALID_SOCKET) {
		tw_shutdown_both((tw_socket_t)s);
		tw_closesocket((tw_socket_t)s);
	}
	if (irc_thread_.joinable())
		irc_thread_.join();

	curl_global_cleanup();
}

/* ---------------- state accessors ---------------- */

bool TwitchService::authed() const
{
	std::lock_guard<std::mutex> lock(state_mutex_);
	return !access_token_.empty() && !user_id_.empty();
}

std::string TwitchService::login() const
{
	std::lock_guard<std::mutex> lock(state_mutex_);
	return login_;
}

std::string TwitchService::client_id() const
{
	std::lock_guard<std::mutex> lock(state_mutex_);
	return client_id_;
}

void TwitchService::set_client_id(const std::string &id)
{
	std::lock_guard<std::mutex> lock(state_mutex_);
	client_id_ = id;
}

void TwitchService::logout()
{
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		access_token_.clear();
		refresh_token_.clear();
		user_id_.clear();
		login_.clear();
		expires_at_ms_ = 0;
	}
	if (on_state_changed)
		on_state_changed();
	apply_chat_state();
}

/* ---------------- HTTP ---------------- */

TwitchService::Http TwitchService::http_post_form(const std::string &url, const std::string &form_body,
						  const std::vector<std::string> &headers)
{
	Http res;
	CURL *curl = curl_easy_init();
	if (!curl)
		return res;

	struct curl_slist *hdrs = nullptr;
	for (const std::string &h : headers)
		hdrs = curl_slist_append(hdrs, h.c_str());

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form_body.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	if (hdrs)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

	CURLcode rc = curl_easy_perform(curl);
	if (rc == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.code);
	else
		obs_log(LOG_WARNING, "twitch http error: %s", curl_easy_strerror(rc));

	if (hdrs)
		curl_slist_free_all(hdrs);
	curl_easy_cleanup(curl);
	return res;
}

TwitchService::Http TwitchService::http_post_json(const std::string &url, const std::string &json_body,
						  const std::vector<std::string> &headers)
{
	std::vector<std::string> h = headers;
	h.push_back("Content-Type: application/json");
	return http_post_form(url, json_body, h);
}

TwitchService::Http TwitchService::http_get(const std::string &url, const std::vector<std::string> &headers)
{
	Http res;
	CURL *curl = curl_easy_init();
	if (!curl)
		return res;

	struct curl_slist *hdrs = nullptr;
	for (const std::string &h : headers)
		hdrs = curl_slist_append(hdrs, h.c_str());

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	if (hdrs)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

	CURLcode rc = curl_easy_perform(curl);
	if (rc == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.code);
	else
		obs_log(LOG_WARNING, "twitch http error: %s", curl_easy_strerror(rc));

	if (hdrs)
		curl_slist_free_all(hdrs);
	curl_easy_cleanup(curl);
	return res;
}

/* ---------------- tokens ---------------- */

bool TwitchService::refresh_tokens_locked(std::string refresh_token)
{
	/* state_mutex_ must NOT be held (does HTTP) */
	std::string cid = client_id();
	if (cid.empty() || refresh_token.empty())
		return false;

	std::string form =
		"grant_type=refresh_token&refresh_token=" + url_encode(refresh_token) + "&client_id=" + url_encode(cid);
	Http res = http_post_form(std::string(TWITCH_ID_BASE) + "/oauth2/token", form);
	if (res.code != 200) {
		obs_log(LOG_WARNING, "twitch token refresh failed (%ld): %s", res.code, res.body.c_str());
		return false;
	}

	obs_data_t *d = obs_data_create_from_json(res.body.c_str());
	if (!d)
		return false;
	std::string access = obs_data_get_string(d, "access_token");
	std::string refresh = obs_data_get_string(d, "refresh_token");
	int64_t expires_in = obs_data_get_int(d, "expires_in");
	obs_data_release(d);

	if (access.empty())
		return false;

	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		access_token_ = access;
		if (!refresh.empty())
			refresh_token_ = refresh; /* rotation: always keep the newest */
		expires_at_ms_ = mono_ms() + expires_in * 1000;
	}
	if (on_state_changed)
		on_state_changed();
	return true;
}

bool TwitchService::ensure_fresh_token()
{
	std::string refresh;
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		if (access_token_.empty())
			return false;
		/* refresh when unknown expiry or < 10 minutes left */
		if (expires_at_ms_ != 0 && mono_ms() < expires_at_ms_ - 10 * 60 * 1000)
			return true;
		refresh = refresh_token_;
	}
	if (refresh.empty()) {
		/* No refresh token — validate the access token instead */
		std::string token;
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			token = access_token_;
		}
		Http res =
			http_get(std::string(TWITCH_ID_BASE) + "/oauth2/validate", {"Authorization: OAuth " + token});
		if (res.code == 200) {
			obs_data_t *d = obs_data_create_from_json(res.body.c_str());
			int64_t expires_in = d ? obs_data_get_int(d, "expires_in") : 0;
			if (d)
				obs_data_release(d);
			std::lock_guard<std::mutex> lock(state_mutex_);
			expires_at_ms_ = mono_ms() + expires_in * 1000;
			return true;
		}
		return false;
	}
	return refresh_tokens_locked(refresh);
}

bool TwitchService::fetch_identity()
{
	std::string token, cid;
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		token = access_token_;
		cid = client_id_;
	}
	if (token.empty() || cid.empty())
		return false;

	Http res = http_get(std::string(TWITCH_HELIX_BASE) + "/users",
			    {"Authorization: Bearer " + token, "Client-Id: " + cid});
	if (res.code != 200)
		return false;

	obs_data_t *d = obs_data_create_from_json(res.body.c_str());
	if (!d)
		return false;
	obs_data_array_t *arr = obs_data_get_array(d, "data");
	bool ok = false;
	if (arr && obs_data_array_count(arr) > 0) {
		obs_data_t *u = obs_data_array_item(arr, 0);
		const char *id = obs_data_get_string(u, "id");
		const char *lg = obs_data_get_string(u, "login");
		if (id && *id) {
			std::lock_guard<std::mutex> lock(state_mutex_);
			user_id_ = id;
			login_ = lg ? lg : "";
			ok = true;
		}
		obs_data_release(u);
	}
	if (arr)
		obs_data_array_release(arr);
	obs_data_release(d);
	return ok;
}

/* ---------------- device flow ---------------- */

void TwitchService::begin_device_auth(std::function<void(std::string, std::string)> on_code,
				      std::function<void(bool, std::string)> on_done)
{
	if (device_active_) {
		if (on_done)
			on_done(false, "authorization already in progress");
		return;
	}
	std::string cid = client_id();
	if (cid.empty()) {
		if (on_done)
			on_done(false, "no Twitch Client ID configured");
		return;
	}

	device_cancel_ = false;
	device_active_ = true;
	if (device_thread_.joinable())
		device_thread_.join();

	device_thread_ = std::thread([this, cid, on_code, on_done]() {
		std::string form = "client_id=" + url_encode(cid) + "&scopes=" + url_encode(MARKER_SCOPES);
		Http res = http_post_form(std::string(TWITCH_ID_BASE) + "/oauth2/device", form);
		if (res.code != 200) {
			device_active_ = false;
			if (on_done)
				on_done(false, "device request failed (" + std::to_string(res.code) + "): " + res.body);
			return;
		}

		obs_data_t *d = obs_data_create_from_json(res.body.c_str());
		if (!d) {
			device_active_ = false;
			if (on_done)
				on_done(false, "bad device response");
			return;
		}
		std::string device_code = obs_data_get_string(d, "device_code");
		std::string user_code = obs_data_get_string(d, "user_code");
		std::string veri_uri = obs_data_get_string(d, "verification_uri");
		int64_t interval = obs_data_get_int(d, "interval");
		int64_t expires_in = obs_data_get_int(d, "expires_in");
		obs_data_release(d);

		if (interval <= 0)
			interval = 5;
		if (expires_in <= 0)
			expires_in = 1800;

		if (on_code)
			on_code(user_code, veri_uri);

		int64_t deadline = mono_ms() + expires_in * 1000;
		while (!device_cancel_ && mono_ms() < deadline) {
			for (int i = 0; i < interval * 10 && !device_cancel_; i++)
				os_sleep_ms(100);
			if (device_cancel_)
				break;

			std::string poll = "client_id=" + url_encode(cid) + "&scopes=" + url_encode(MARKER_SCOPES) +
					   "&device_code=" + url_encode(device_code) +
					   "&grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code";
			Http tok = http_post_form(std::string(TWITCH_ID_BASE) + "/oauth2/token", poll);

			if (tok.code == 200) {
				obs_data_t *td = obs_data_create_from_json(tok.body.c_str());
				if (!td)
					break;
				std::string access = obs_data_get_string(td, "access_token");
				std::string refresh = obs_data_get_string(td, "refresh_token");
				int64_t tok_expires = obs_data_get_int(td, "expires_in");
				obs_data_release(td);

				{
					std::lock_guard<std::mutex> lock(state_mutex_);
					access_token_ = access;
					refresh_token_ = refresh;
					expires_at_ms_ = mono_ms() + tok_expires * 1000;
				}

				bool id_ok = fetch_identity();
				device_active_ = false;
				if (on_state_changed)
					on_state_changed();
				apply_chat_state();
				if (on_done)
					on_done(id_ok, id_ok ? login() : "authenticated but identity lookup failed");
				return;
			}

			std::string message = json_string_field(tok.body, "message");
			if (message == "authorization_pending")
				continue;
			if (message == "slow_down") {
				interval += 5;
				continue;
			}

			device_active_ = false;
			if (on_done)
				on_done(false, message.empty()
						       ? ("token poll failed (" + std::to_string(tok.code) + ")")
						       : message);
			return;
		}

		device_active_ = false;
		if (on_done)
			on_done(false, device_cancel_ ? "cancelled" : "code expired — try again");
	});
}

void TwitchService::cancel_device_auth()
{
	device_cancel_ = true;
	device_active_ = false;
	if (device_thread_.joinable())
		device_thread_.join();
}

/* ---------------- markers ---------------- */

void TwitchService::queue_marker(const std::string &description)
{
	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		if (marker_queue_.size() > 30)
			marker_queue_.pop_front(); /* keep it bounded */
		marker_queue_.push_back(description);
	}
	queue_cv_.notify_one();
}

void TwitchService::marker_worker()
{
	while (workers_running_) {
		std::string desc;
		{
			std::unique_lock<std::mutex> lock(queue_mutex_);
			queue_cv_.wait(lock, [this]() { return !workers_running_ || !marker_queue_.empty(); });
			if (!workers_running_)
				return;
			desc = marker_queue_.front();
			marker_queue_.pop_front();
		}

		if (!ensure_fresh_token()) {
			obs_log(LOG_WARNING, "twitch marker dropped — not authenticated");
			continue;
		}

		std::string token, cid, uid;
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			token = access_token_;
			cid = client_id_;
			uid = user_id_;
		}

		/* JSON-escape the description minimally (quotes/backslashes) */
		std::string esc;
		for (char c : desc) {
			if (c == '"' || c == '\\')
				esc += '\\';
			if (static_cast<unsigned char>(c) >= 0x20)
				esc += c;
		}

		std::string body = "{\"user_id\":\"" + uid + "\",\"description\":\"" + esc + "\"}";
		Http res = http_post_json(std::string(TWITCH_HELIX_BASE) + "/streams/markers", body,
					  {"Authorization: Bearer " + token, "Client-Id: " + cid});

		if (res.code == 401) {
			/* one refresh + retry */
			std::string refresh;
			{
				std::lock_guard<std::mutex> lock(state_mutex_);
				refresh = refresh_token_;
			}
			if (refresh_tokens_locked(refresh)) {
				{
					std::lock_guard<std::mutex> lock(state_mutex_);
					token = access_token_;
				}
				res = http_post_json(std::string(TWITCH_HELIX_BASE) + "/streams/markers", body,
						     {"Authorization: Bearer " + token, "Client-Id: " + cid});
			}
		}

		if (res.code == 200) {
			obs_log(LOG_INFO, "twitch marker created: %s", desc.c_str());
		} else if (res.code == 404) {
			obs_log(LOG_INFO, "twitch marker skipped (not live / VODs disabled)");
		} else {
			obs_log(LOG_WARNING, "twitch marker failed (%ld): %s", res.code, res.body.c_str());
		}
	}
}

/* ---------------- chat (anonymous IRC) ---------------- */

void TwitchService::set_chat_config(const ChatConfig &cfg)
{
	std::lock_guard<std::mutex> lock(chat_mutex_);
	chat_cfg_ = cfg;
}

TwitchService::ChatConfig TwitchService::chat_config() const
{
	std::lock_guard<std::mutex> lock(chat_mutex_);
	return chat_cfg_;
}

bool TwitchService::chat_running() const
{
	return irc_connected_;
}

void TwitchService::apply_chat_state()
{
	ChatConfig cfg = chat_config();
	std::string channel = cfg.channel_override.empty() ? login() : cfg.channel_override;
	bool want = cfg.enabled && !channel.empty();

	if (want && !irc_should_run_) {
		irc_should_run_ = true;
		if (irc_thread_.joinable())
			irc_thread_.join();
		irc_thread_ = std::thread([this]() { irc_worker(); });
	} else if (!want && irc_should_run_) {
		irc_should_run_ = false;
		intptr_t s = irc_socket_.exchange((intptr_t)TW_INVALID_SOCKET);
		if (s != (intptr_t)TW_INVALID_SOCKET) {
			tw_shutdown_both((tw_socket_t)s);
			tw_closesocket((tw_socket_t)s);
		}
		if (irc_thread_.joinable())
			irc_thread_.join();
	}
}

void TwitchService::irc_worker()
{
	while (irc_should_run_) {
		ChatConfig cfg = chat_config();
		std::string channel = cfg.channel_override.empty() ? login() : cfg.channel_override;
		if (channel.empty()) {
			for (int i = 0; i < 100 && irc_should_run_; i++)
				os_sleep_ms(100);
			continue;
		}
		for (char &c : channel)
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');

		/* resolve + connect irc.chat.twitch.tv:6667 (plain; read-only anon) */
		struct addrinfo hints;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		struct addrinfo *result = nullptr;
		if (getaddrinfo("irc.chat.twitch.tv", "6667", &hints, &result) != 0 || !result) {
			obs_log(LOG_WARNING, "twitch chat: DNS lookup failed, retrying in 15s");
			for (int i = 0; i < 150 && irc_should_run_; i++)
				os_sleep_ms(100);
			continue;
		}

		tw_socket_t sock = TW_INVALID_SOCKET;
		for (struct addrinfo *ai = result; ai; ai = ai->ai_next) {
			sock = (tw_socket_t)::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
			if (sock == TW_INVALID_SOCKET)
				continue;
			if (::connect(sock, ai->ai_addr, (int)ai->ai_addrlen) == 0)
				break;
			tw_closesocket(sock);
			sock = TW_INVALID_SOCKET;
		}
		freeaddrinfo(result);

		if (sock == TW_INVALID_SOCKET) {
			obs_log(LOG_WARNING, "twitch chat: connect failed, retrying in 15s");
			for (int i = 0; i < 150 && irc_should_run_; i++)
				os_sleep_ms(100);
			continue;
		}

		irc_socket_ = (intptr_t)sock;

		/* receive timeout so a silent connection is re-established
		   (Twitch pings roughly every 5 minutes) */
#ifdef _WIN32
		DWORD timeout = 420 * 1000;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
#else
		struct timeval tv;
		tv.tv_sec = 420;
		tv.tv_usec = 0;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

		auto send_line = [&](const std::string &line) {
			std::string data = line + "\r\n";
			::send(sock, data.c_str(), (int)data.size(), 0);
		};

		int nick_num = (int)(os_gettime_ns() % 80000ULL) + 10000;
		send_line("CAP REQ :twitch.tv/tags twitch.tv/commands");
		send_line("NICK justinfan" + std::to_string(nick_num));
		send_line("JOIN #" + channel);
		obs_log(LOG_INFO, "twitch chat: listening in #%s for '%s'", channel.c_str(), cfg.command.c_str());
		irc_connected_ = true;

		std::string buffer;
		char chunk[4096];
		while (irc_should_run_) {
			int n = ::recv(sock, chunk, sizeof(chunk), 0);
			if (n <= 0)
				break;
			buffer.append(chunk, n);

			size_t pos;
			while ((pos = buffer.find("\r\n")) != std::string::npos) {
				std::string line = buffer.substr(0, pos);
				buffer.erase(0, pos + 2);

				if (line.rfind("PING", 0) == 0) {
					send_line("PONG" + line.substr(4));
					continue;
				}

				/* @tags :nick!user@host PRIVMSG #chan :message */
				size_t privmsg = line.find(" PRIVMSG #");
				if (privmsg == std::string::npos)
					continue;

				std::string tags;
				size_t rest = 0;
				if (!line.empty() && line[0] == '@') {
					size_t sp = line.find(' ');
					if (sp == std::string::npos)
						continue;
					tags = line.substr(1, sp - 1);
					rest = sp + 1;
				}

				size_t colon = line.find(" :", privmsg);
				if (colon == std::string::npos)
					continue;
				std::string msg = line.substr(colon + 2);
				while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' '))
					msg.pop_back();

				/* command match (exact word or prefix followed by space) */
				const std::string &cmdname = cfg.command;
				if (msg.compare(0, cmdname.size(), cmdname) != 0)
					continue;
				if (msg.size() > cmdname.size() && msg[cmdname.size()] != ' ')
					continue;

				/* extract tag values */
				auto tag_value = [&](const char *key) -> std::string {
					std::string needle = std::string(key) + "=";
					size_t start;
					if (tags.compare(0, needle.size(), needle) == 0) {
						start = needle.size();
					} else {
						size_t p = tags.find(";" + needle);
						if (p == std::string::npos)
							return "";
						start = p + 1 + needle.size();
					}
					size_t end = tags.find(';', start);
					return tags.substr(start,
							   end == std::string::npos ? std::string::npos : end - start);
				};

				std::string badges = tag_value("badges");
				std::string display = tag_value("display-name");
				bool is_broadcaster = badges.find("broadcaster/") != std::string::npos;
				bool is_mod = tag_value("mod") == "1" || is_broadcaster;
				bool is_vip = badges.find("vip/") != std::string::npos || is_mod;
				bool is_sub = tag_value("subscriber") == "1" ||
					      badges.find("subscriber/") != std::string::npos || is_vip;

				bool allowed;
				if (cfg.permission == "broadcaster")
					allowed = is_broadcaster;
				else if (cfg.permission == "mod")
					allowed = is_mod;
				else if (cfg.permission == "vip")
					allowed = is_vip;
				else if (cfg.permission == "sub")
					allowed = is_sub;
				else
					allowed = true;

				if (!allowed)
					continue;

				{
					std::lock_guard<std::mutex> lock(chat_mutex_);
					int64_t now = mono_ms();
					if (now - last_chat_clip_ms_ < (int64_t)cfg.cooldown_s * 1000)
						continue;
					last_chat_clip_ms_ = now;
				}

				if (display.empty()) {
					/* fall back to nick from prefix */
					size_t excl = line.find('!', rest);
					size_t pref = line.find(':', rest);
					if (pref != std::string::npos && excl != std::string::npos && excl > pref)
						display = line.substr(pref + 1, excl - pref - 1);
					else
						display = "viewer";
				}

				obs_log(LOG_INFO, "twitch chat: %s triggered %s", display.c_str(), cmdname.c_str());
				if (on_chat_clip)
					on_chat_clip(display, channel);
			}
		}

		irc_connected_ = false;
		intptr_t cur = irc_socket_.exchange((intptr_t)TW_INVALID_SOCKET);
		if (cur != (intptr_t)TW_INVALID_SOCKET)
			tw_closesocket((tw_socket_t)cur);

		if (irc_should_run_) {
			obs_log(LOG_INFO, "twitch chat: disconnected, reconnecting in 10s");
			for (int i = 0; i < 100 && irc_should_run_; i++)
				os_sleep_ms(100);
		}
	}
	irc_connected_ = false;
}

} // namespace gamepulse
