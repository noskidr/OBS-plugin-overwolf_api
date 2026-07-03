/*
GamePulse for OBS — protocol implementation.
Uses obs_data for JSON parsing (no extra dependency).
*/

#include "gp-protocol.h"
#include "gp-taxonomy.h"

#include <ctime>

#include <util/base.h>

namespace gamepulse {

namespace {

int64_t now_wall_ms()
{
	return static_cast<int64_t>(time(nullptr)) * 1000;
}

std::string get_string(obs_data_t *d, const char *key)
{
	const char *v = obs_data_get_string(d, key);
	return v ? v : "";
}

/* game object: {"id": 21640, "name": "VALORANT"} — id may arrive as number
   or string; normalize to string. */
void read_game(obs_data_t *msg, std::string &game_id, std::string &game_name)
{
	obs_data_t *game = obs_data_get_obj(msg, "game");
	if (!game)
		return;
	obs_data_item_t *id_item = obs_data_item_byname(game, "id");
	if (id_item) {
		enum obs_data_number_type num_type = obs_data_item_numtype(id_item);
		if (obs_data_item_gettype(id_item) == OBS_DATA_NUMBER) {
			long long v = (num_type == OBS_DATA_NUM_DOUBLE) ? (long long)obs_data_item_get_double(id_item)
									: obs_data_item_get_int(id_item);
			game_id = std::to_string(v);
		} else {
			const char *s = obs_data_item_get_string(id_item);
			if (s)
				game_id = s;
		}
		obs_data_item_release(&id_item);
	}
	game_name = get_string(game, "name");
	obs_data_release(game);
}

std::string json_escape_min(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (char raw : s) {
		unsigned char c = static_cast<unsigned char>(raw);
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	return out;
}

} // namespace

ParseResult Protocol::parse(const std::string &json_text)
{
	ParseResult out;

	obs_data_t *msg = obs_data_create_from_json(json_text.c_str());
	if (!msg) {
		out.error = "invalid JSON";
		return out;
	}

	std::string t = get_string(msg, "t");

	if (t == "hello") {
		out.type = MsgType::Hello;
		out.client_name = get_string(msg, "client");
		out.client_version = get_string(msg, "version");
		out.token = get_string(msg, "token");
	} else if (t == "game") {
		out.type = MsgType::Game;
		out.game_state = get_string(msg, "state");
		read_game(msg, out.game_id, out.game_name);
		if (out.game_state == "closed")
			reset_game_state(out.game_id);
	} else if (t == "event") {
		out.type = MsgType::Event;
		normalize_event(msg, out);
	} else if (t == "info") {
		out.type = MsgType::Info;
		normalize_info(msg, out);
	} else if (t == "batch") {
		out.type = MsgType::Batch;
		obs_data_array_t *items = obs_data_get_array(msg, "items");
		if (items) {
			size_t n = obs_data_array_count(items);
			for (size_t i = 0; i < n; i++) {
				obs_data_t *item = obs_data_array_item(items, i);
				if (!item)
					continue;
				std::string it = get_string(item, "t");
				if (it == "event")
					normalize_event(item, out);
				else if (it == "info")
					normalize_info(item, out);
				obs_data_release(item);
			}
			obs_data_array_release(items);
		}
	} else if (t == "ping") {
		out.type = MsgType::Ping;
	} else {
		out.error = "unknown message type '" + t + "'";
	}

	obs_data_release(msg);
	return out;
}

void Protocol::normalize_event(obs_data_t *item, ParseResult &out)
{
	GpEvent ev;
	ev.source = EventSource::Gep;
	read_game(item, ev.game_id, ev.game_name);
	ev.game_name = Taxonomy::game_name(ev.game_id, ev.game_name);
	ev.name = get_string(item, "name");
	if (ev.name.empty())
		return;

	int64_t ts = obs_data_get_int(item, "ts");
	ev.wall_ms = ts > 0 ? ts : now_wall_ms();

	EventMeta meta = Taxonomy::lookup(ev.game_id, ev.name);
	ev.label = meta.label;
	ev.importance = meta.importance;

	/* companion-provided enrichment wins over taxonomy defaults */
	std::string label = get_string(item, "label");
	if (!label.empty())
		ev.label = label;
	ev.detail = get_string(item, "detail");
	if (obs_data_has_user_value(item, "importance")) {
		int imp = (int)obs_data_get_int(item, "importance");
		if (imp >= IMP_DEBUG && imp <= IMP_EPIC)
			ev.importance = imp;
	}

	obs_data_t *data = obs_data_get_obj(item, "data");
	ev.data = data; /* transfer ref (may be null) */

	out.events.push_back(std::move(ev));
}

void Protocol::normalize_info(obs_data_t *item, ParseResult &out)
{
	std::string game_id, game_name;
	read_game(item, game_id, game_name);
	std::string key = get_string(item, "key");
	std::string value = get_string(item, "value");
	if (key.empty())
		return;

	std::string state_key = game_id + "|" + key;
	std::string prev;
	auto it = info_state_.find(state_key);
	if (it != info_state_.end())
		prev = it->second;
	info_state_[state_key] = value;

	/* Synthesize round boundaries from Valorant-style round_phase
	   transitions: shopping -> combat -> end (-> game_end). A new
	   "shopping" phase means a round is starting. */
	if (key == "round_phase" && value != prev) {
		const char *synth = nullptr;
		if (value == "shopping")
			synth = "round_start";
		else if (value == "end" || value == "game_end")
			synth = "round_end";

		if (synth) {
			GpEvent ev;
			ev.source = EventSource::Gep;
			ev.game_id = game_id;
			ev.game_name = Taxonomy::game_name(game_id, game_name);
			ev.name = synth;
			int64_t ts = obs_data_get_int(item, "ts");
			ev.wall_ms = ts > 0 ? ts : now_wall_ms();
			EventMeta meta = Taxonomy::lookup(game_id, ev.name);
			ev.label = meta.label;
			ev.importance = IMP_DEBUG; /* boundaries are plumbing, not highlights */
			out.events.push_back(std::move(ev));
		}
	}
}

std::string Protocol::info_value(const std::string &game_id, const std::string &key) const
{
	auto it = info_state_.find(game_id + "|" + key);
	return it == info_state_.end() ? "" : it->second;
}

void Protocol::reset_game_state(const std::string &game_id)
{
	std::string prefix = game_id + "|";
	for (auto it = info_state_.begin(); it != info_state_.end();) {
		if (it->first.compare(0, prefix.size(), prefix) == 0)
			it = info_state_.erase(it);
		else
			++it;
	}
}

std::string Protocol::make_welcome(const char *plugin_version, const char *obs_version)
{
	return std::string("{\"t\":\"welcome\",\"plugin\":\"obs-gamepulse\",\"version\":\"") +
	       json_escape_min(plugin_version ? plugin_version : "?") + "\",\"obs\":\"" +
	       json_escape_min(obs_version ? obs_version : "?") + "\"}";
}

std::string Protocol::make_status(bool streaming, bool recording, bool replay, int64_t stream_ms, int64_t record_ms)
{
	char buf[192];
	snprintf(buf, sizeof(buf),
		 "{\"t\":\"status\",\"streaming\":%s,\"recording\":%s,\"replay\":%s,"
		 "\"stream_ms\":%lld,\"record_ms\":%lld}",
		 streaming ? "true" : "false", recording ? "true" : "false", replay ? "true" : "false",
		 (long long)stream_ms, (long long)record_ms);
	return buf;
}

std::string Protocol::make_pong()
{
	return "{\"t\":\"pong\"}";
}

std::string Protocol::make_error(const std::string &message)
{
	return "{\"t\":\"error\",\"message\":\"" + json_escape_min(message) + "\"}";
}

} // namespace gamepulse
