/*
GamePulse for OBS — embedded localhost WebSocket server implementation.
RFC 6455 server side: HTTP upgrade handshake, frame parse (masked client
frames, fragmentation, ping/pong/close), unmasked server frames.
*/

#include "gp-ws-server.h"
#include "gp-sha1.h"

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET gp_socket_t;
#define GP_INVALID_SOCKET INVALID_SOCKET
#define gp_closesocket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int gp_socket_t;
#define GP_INVALID_SOCKET (-1)
#define gp_closesocket close
#endif

#include <util/base.h>

namespace gamepulse {

namespace {

const char *WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const size_t MAX_PAYLOAD = 4 * 1024 * 1024; /* 4 MB hard cap per message */

std::string base64_encode(const uint8_t *data, size_t len)
{
	static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve(((len + 2) / 3) * 4);
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = static_cast<uint32_t>(data[i]) << 16;
		if (i + 1 < len)
			v |= static_cast<uint32_t>(data[i + 1]) << 8;
		if (i + 2 < len)
			v |= static_cast<uint32_t>(data[i + 2]);
		out.push_back(tbl[(v >> 18) & 63]);
		out.push_back(tbl[(v >> 12) & 63]);
		out.push_back(i + 1 < len ? tbl[(v >> 6) & 63] : '=');
		out.push_back(i + 2 < len ? tbl[v & 63] : '=');
	}
	return out;
}

std::string to_lower(std::string s)
{
	for (char &c : s)
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	return s;
}

std::string trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos)
		return "";
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

bool send_all(gp_socket_t sock, const uint8_t *data, size_t len)
{
	size_t sent = 0;
	while (sent < len) {
#ifdef _WIN32
		int n = ::send(sock, reinterpret_cast<const char *>(data + sent), static_cast<int>(len - sent), 0);
#else
		ssize_t n = ::send(sock, data + sent, len - sent, 0);
#endif
		if (n <= 0)
			return false;
		sent += static_cast<size_t>(n);
	}
	return true;
}

/* Build a server->client frame (no masking) */
std::vector<uint8_t> build_frame(uint8_t opcode, const std::string &payload)
{
	std::vector<uint8_t> f;
	size_t len = payload.size();
	f.push_back(static_cast<uint8_t>(0x80 | (opcode & 0x0F)));
	if (len < 126) {
		f.push_back(static_cast<uint8_t>(len));
	} else if (len <= 0xFFFF) {
		f.push_back(126);
		f.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
		f.push_back(static_cast<uint8_t>(len & 0xFF));
	} else {
		f.push_back(127);
		for (int i = 7; i >= 0; i--)
			f.push_back(static_cast<uint8_t>((static_cast<uint64_t>(len) >> (8 * i)) & 0xFF));
	}
	f.insert(f.end(), payload.begin(), payload.end());
	return f;
}

} // namespace

struct ClientConn {
	int id = 0;
	gp_socket_t sock = GP_INVALID_SOCKET;
	std::thread thread;
	std::mutex write_mutex;
	std::atomic<bool> open{false};
};

struct WsServer::Impl {
	gp_socket_t listener = GP_INVALID_SOCKET;
	std::thread accept_thread;
	std::atomic<bool> running{false};
	uint16_t port = 0;
	std::string token;

	std::mutex clients_mutex;
	std::map<int, std::shared_ptr<ClientConn>> clients;
	int next_id = 1;

	MessageHandler on_message;
	ConnectHandler on_connect;
	DisconnectHandler on_disconnect;
	std::mutex handler_mutex;

#ifdef _WIN32
	bool wsa_ready = false;
#endif

	bool recv_byte(gp_socket_t s, uint8_t *b)
	{
#ifdef _WIN32
		int n = ::recv(s, reinterpret_cast<char *>(b), 1, 0);
#else
		ssize_t n = ::recv(s, b, 1, 0);
#endif
		return n == 1;
	}

	bool recv_exact(gp_socket_t s, uint8_t *buf, size_t len)
	{
		size_t got = 0;
		while (got < len) {
#ifdef _WIN32
			int n = ::recv(s, reinterpret_cast<char *>(buf + got), static_cast<int>(len - got), 0);
#else
			ssize_t n = ::recv(s, buf + got, len - got, 0);
#endif
			if (n <= 0)
				return false;
			got += static_cast<size_t>(n);
		}
		return true;
	}

	bool send_frame(const std::shared_ptr<ClientConn> &c, uint8_t opcode, const std::string &payload)
	{
		if (!c->open)
			return false;
		std::vector<uint8_t> f = build_frame(opcode, payload);
		std::lock_guard<std::mutex> lock(c->write_mutex);
		return send_all(c->sock, f.data(), f.size());
	}

	/* Read HTTP request until CRLFCRLF (with a sane cap), perform WS handshake.
	   Returns false to drop the connection. */
	bool do_handshake(const std::shared_ptr<ClientConn> &c)
	{
		std::string req;
		uint8_t b;
		while (req.size() < 16 * 1024) {
			if (!recv_byte(c->sock, &b))
				return false;
			req.push_back(static_cast<char>(b));
			if (req.size() >= 4 && req.compare(req.size() - 4, 4, "\r\n\r\n") == 0)
				break;
		}
		if (req.size() >= 16 * 1024)
			return false;

		std::istringstream stream(req);
		std::string request_line;
		std::getline(stream, request_line);
		request_line = trim(request_line);

		/* GET <path> HTTP/1.1 */
		std::string path;
		{
			size_t sp1 = request_line.find(' ');
			size_t sp2 = request_line.rfind(' ');
			if (sp1 == std::string::npos || sp2 == std::string::npos || sp2 <= sp1)
				return false;
			if (request_line.substr(0, sp1) != "GET")
				return false;
			path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
		}

		std::map<std::string, std::string> headers;
		std::string line;
		while (std::getline(stream, line)) {
			line = trim(line);
			if (line.empty())
				break;
			size_t colon = line.find(':');
			if (colon == std::string::npos)
				continue;
			headers[to_lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
		}

		auto header = [&](const char *name) -> std::string {
			auto it = headers.find(name);
			return it == headers.end() ? "" : it->second;
		};

		std::string key = header("sec-websocket-key");
		bool is_upgrade = to_lower(header("upgrade")).find("websocket") != std::string::npos;
		if (key.empty() || !is_upgrade) {
			static const char *bad = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
			send_all(c->sock, reinterpret_cast<const uint8_t *>(bad), strlen(bad));
			return false;
		}

		/* Token check: ?token=X in path or Authorization: Bearer X */
		if (!token.empty()) {
			bool ok = false;
			size_t q = path.find("token=");
			if (q != std::string::npos) {
				std::string t = path.substr(q + 6);
				size_t amp = t.find('&');
				if (amp != std::string::npos)
					t = t.substr(0, amp);
				ok = (t == token);
			}
			if (!ok) {
				std::string auth = header("authorization");
				const std::string prefix = "Bearer ";
				if (auth.rfind(prefix, 0) == 0)
					ok = (auth.substr(prefix.size()) == token);
			}
			if (!ok) {
				static const char *denied = "HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n";
				send_all(c->sock, reinterpret_cast<const uint8_t *>(denied), strlen(denied));
				blog(LOG_WARNING, "[gamepulse] WS client rejected: bad token");
				return false;
			}
		}

		uint8_t digest[20];
		Sha1::hash(key + WS_GUID, digest);
		std::string accept = base64_encode(digest, 20);

		std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
				   "Upgrade: websocket\r\n"
				   "Connection: Upgrade\r\n"
				   "Sec-WebSocket-Accept: " +
				   accept + "\r\n\r\n";
		return send_all(c->sock, reinterpret_cast<const uint8_t *>(resp.data()), resp.size());
	}

	void client_loop(std::shared_ptr<ClientConn> c)
	{
		if (!do_handshake(c)) {
			drop_client(c, false);
			return;
		}

		c->open = true;
		{
			ConnectHandler handler;
			{
				std::lock_guard<std::mutex> lock(handler_mutex);
				handler = on_connect;
			}
			if (handler)
				handler(c->id, "127.0.0.1");
		}

		std::string message; /* accumulates across fragments */
		uint8_t frag_opcode = 0;

		for (;;) {
			uint8_t hdr[2];
			if (!recv_exact(c->sock, hdr, 2))
				break;
			bool fin = (hdr[0] & 0x80) != 0;
			uint8_t opcode = hdr[0] & 0x0F;
			bool masked = (hdr[1] & 0x80) != 0;
			uint64_t len = hdr[1] & 0x7F;

			if (len == 126) {
				uint8_t ext[2];
				if (!recv_exact(c->sock, ext, 2))
					break;
				len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
			} else if (len == 127) {
				uint8_t ext[8];
				if (!recv_exact(c->sock, ext, 8))
					break;
				len = 0;
				for (int i = 0; i < 8; i++)
					len = (len << 8) | ext[i];
			}

			if (len > MAX_PAYLOAD || message.size() + len > MAX_PAYLOAD)
				break; /* protocol abuse; drop */

			uint8_t mask[4] = {0, 0, 0, 0};
			if (masked) {
				if (!recv_exact(c->sock, mask, 4))
					break;
			}

			std::vector<uint8_t> payload(static_cast<size_t>(len));
			if (len > 0 && !recv_exact(c->sock, payload.data(), payload.size()))
				break;
			if (masked) {
				for (size_t i = 0; i < payload.size(); i++)
					payload[i] ^= mask[i & 3];
			}

			if (opcode == 0x8) { /* close */
				send_frame(c, 0x8, std::string(payload.begin(), payload.end()));
				break;
			} else if (opcode == 0x9) { /* ping -> pong */
				send_frame(c, 0xA, std::string(payload.begin(), payload.end()));
				continue;
			} else if (opcode == 0xA) { /* pong */
				continue;
			} else if (opcode == 0x1 || opcode == 0x2 || opcode == 0x0) {
				if (opcode != 0x0) {
					frag_opcode = opcode;
					message.clear();
				}
				message.append(payload.begin(), payload.end());
				if (!fin)
					continue;
				if (frag_opcode == 0x1) { /* text only; binary ignored */
					MessageHandler handler;
					{
						std::lock_guard<std::mutex> lock(handler_mutex);
						handler = on_message;
					}
					if (handler)
						handler(c->id, message);
				}
				message.clear();
				frag_opcode = 0;
			}
			/* other opcodes ignored */
		}

		drop_client(c, true);
	}

	void drop_client(const std::shared_ptr<ClientConn> &c, bool was_open)
	{
		bool notify = c->open.exchange(false) || was_open;
		if (c->sock != GP_INVALID_SOCKET) {
			gp_closesocket(c->sock);
			c->sock = GP_INVALID_SOCKET;
		}
		{
			std::lock_guard<std::mutex> lock(clients_mutex);
			clients.erase(c->id);
		}
		if (notify) {
			DisconnectHandler handler;
			{
				std::lock_guard<std::mutex> lock(handler_mutex);
				handler = on_disconnect;
			}
			if (handler)
				handler(c->id);
		}
	}

	void accept_loop()
	{
		while (running) {
			sockaddr_in addr;
#ifdef _WIN32
			int addr_len = sizeof(addr);
#else
			socklen_t addr_len = sizeof(addr);
#endif
			gp_socket_t s = ::accept(listener, reinterpret_cast<sockaddr *>(&addr), &addr_len);
			if (s == GP_INVALID_SOCKET) {
				if (!running)
					break;
				continue;
			}

			int flag = 1;
			setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&flag), sizeof(flag));

			auto c = std::make_shared<ClientConn>();
			c->sock = s;
			{
				std::lock_guard<std::mutex> lock(clients_mutex);
				c->id = next_id++;
				clients[c->id] = c;
			}
			c->thread = std::thread([this, c]() { client_loop(c); });
			c->thread.detach();
		}
	}
};

WsServer::WsServer() : impl(new Impl) {}

WsServer::~WsServer()
{
	stop();
}

bool WsServer::start(uint16_t port, const std::string &token)
{
	if (impl->running)
		return true;

#ifdef _WIN32
	if (!impl->wsa_ready) {
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			blog(LOG_ERROR, "[gamepulse] WSAStartup failed");
			return false;
		}
		impl->wsa_ready = true;
	}
#endif

	impl->token = token;

	gp_socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == GP_INVALID_SOCKET) {
		blog(LOG_ERROR, "[gamepulse] socket() failed");
		return false;
	}

	int reuse = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));

	sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* localhost only */
	addr.sin_port = htons(port);

	if (::bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
		blog(LOG_ERROR, "[gamepulse] bind(127.0.0.1:%u) failed — port in use?", (unsigned)port);
		gp_closesocket(s);
		return false;
	}
	if (::listen(s, 4) != 0) {
		blog(LOG_ERROR, "[gamepulse] listen() failed");
		gp_closesocket(s);
		return false;
	}

	impl->listener = s;
	impl->port = port;
	impl->running = true;
	impl->accept_thread = std::thread([this]() { impl->accept_loop(); });

	blog(LOG_INFO, "[gamepulse] WebSocket server listening on ws://127.0.0.1:%u%s", (unsigned)port,
	     token.empty() ? "" : " (token required)");
	return true;
}

void WsServer::stop()
{
	if (!impl->running)
		return;
	impl->running = false;

	if (impl->listener != GP_INVALID_SOCKET) {
		gp_closesocket(impl->listener);
		impl->listener = GP_INVALID_SOCKET;
	}
	if (impl->accept_thread.joinable())
		impl->accept_thread.join();

	/* Close all client sockets; their detached threads will fall out of
	   recv and clean themselves up. Snapshot first to avoid holding the
	   lock while sockets close. */
	std::vector<std::shared_ptr<ClientConn>> snapshot;
	{
		std::lock_guard<std::mutex> lock(impl->clients_mutex);
		for (auto &kv : impl->clients)
			snapshot.push_back(kv.second);
	}
	for (auto &c : snapshot) {
		c->open = false;
		if (c->sock != GP_INVALID_SOCKET)
			gp_closesocket(c->sock);
	}

	blog(LOG_INFO, "[gamepulse] WebSocket server stopped");
}

bool WsServer::running() const
{
	return impl->running;
}

uint16_t WsServer::port() const
{
	return impl->port;
}

void WsServer::send_text(int client_id, const std::string &text)
{
	std::shared_ptr<ClientConn> c;
	{
		std::lock_guard<std::mutex> lock(impl->clients_mutex);
		auto it = impl->clients.find(client_id);
		if (it == impl->clients.end())
			return;
		c = it->second;
	}
	impl->send_frame(c, 0x1, text);
}

void WsServer::broadcast_text(const std::string &text)
{
	std::vector<std::shared_ptr<ClientConn>> snapshot;
	{
		std::lock_guard<std::mutex> lock(impl->clients_mutex);
		for (auto &kv : impl->clients)
			snapshot.push_back(kv.second);
	}
	for (auto &c : snapshot)
		impl->send_frame(c, 0x1, text);
}

void WsServer::close_client(int client_id)
{
	std::shared_ptr<ClientConn> c;
	{
		std::lock_guard<std::mutex> lock(impl->clients_mutex);
		auto it = impl->clients.find(client_id);
		if (it == impl->clients.end())
			return;
		c = it->second;
	}
	impl->send_frame(c, 0x8, "");
	c->open = false;
	if (c->sock != GP_INVALID_SOCKET)
		gp_closesocket(c->sock);
}

int WsServer::client_count() const
{
	std::lock_guard<std::mutex> lock(impl->clients_mutex);
	return static_cast<int>(impl->clients.size());
}

void WsServer::set_on_message(MessageHandler h)
{
	std::lock_guard<std::mutex> lock(impl->handler_mutex);
	impl->on_message = std::move(h);
}

void WsServer::set_on_connect(ConnectHandler h)
{
	std::lock_guard<std::mutex> lock(impl->handler_mutex);
	impl->on_connect = std::move(h);
}

void WsServer::set_on_disconnect(DisconnectHandler h)
{
	std::lock_guard<std::mutex> lock(impl->handler_mutex);
	impl->on_disconnect = std::move(h);
}

} // namespace gamepulse
