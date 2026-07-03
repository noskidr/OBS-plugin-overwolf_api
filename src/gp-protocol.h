/*
GamePulse for OBS — WebSocket JSON protocol parsing + event normalization.

Companion -> plugin messages (one JSON object per text frame):
  {"t":"hello","client":"gamepulse-companion","version":"0.1.0","token":"..."}
  {"t":"game","state":"detected"|"running"|"closed","game":{"id":21640,"name":"VALORANT"}}
  {"t":"event","game":{"id":21640,"name":"VALORANT"},"name":"kill",
   "label":"Kill","detail":"Vandal -> Reyna (HS)","importance":2,
   "ts":1730000000000,"data":{...}}                       // label/detail/importance optional
  {"t":"info","game":{...},"feature":"match_info","category":"match_info",
   "key":"round_phase","value":"combat","ts":...}
  {"t":"batch","items":[ <event|info|game objects> ]}
  {"t":"ping"}

Plugin -> companion:
  {"t":"welcome","plugin":"obs-gamepulse","version":"...","obs":"31.1.1"}
  {"t":"status","streaming":b,"recording":b,"replay":b,"stream_ms":n,"record_ms":n}
  {"t":"pong"}
  {"t":"error","message":"..."}

The protocol layer also synthesizes events games do not emit natively:
  Valorant has no round_start/round_end events — they are derived here from
  match_info/round_phase info-update transitions (shopping -> combat -> end).
*/

#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "gp-types.h"

namespace gamepulse {

enum class MsgType {
	Invalid,
	Hello,
	Game,
	Event,
	Info,
	Batch,
	Ping,
};

struct ParseResult {
	MsgType type = MsgType::Invalid;
	std::string error;

	/* Hello */
	std::string client_name;
	std::string client_version;
	std::string token;

	/* Game */
	std::string game_state; /* detected | running | closed */
	std::string game_id;
	std::string game_name;

	/* Event/Info payload(s): normalized events ready for the pipeline
	   (batch expands to many; info usually yields none unless synthesized). */
	std::vector<GpEvent> events;

	/* Context updates carried by "context" infos from the companion
	   (keys: map, agent, score, round, scene, clutch). Consumed by the
	   overlay/dock and OBS scene automation. */
	std::map<std::string, std::string> context;
};

class Protocol {
public:
	/* Parse one incoming text frame. Returns a ParseResult; for Event/Info/
	   Batch the .events vector holds normalized events (may be empty for
	   pure state infos). Never throws. */
	ParseResult parse(const std::string &json_text);

	/* Latest raw info values per (game_id, feature/key) — for overlay/state
	   consumers (e.g. current score). Cleared on game closed. */
	std::string info_value(const std::string &game_id, const std::string &key) const;

	void reset_game_state(const std::string &game_id);

	/* Serialization helpers for plugin -> companion frames. */
	static std::string make_welcome(const char *plugin_version, const char *obs_version);
	static std::string make_status(bool streaming, bool recording, bool replay, int64_t stream_ms,
				       int64_t record_ms);
	static std::string make_pong();
	static std::string make_error(const std::string &message);

private:
	void normalize_event(obs_data_t *item, ParseResult &out);
	void normalize_info(obs_data_t *item, ParseResult &out);

	/* per-game tracked info state, keyed "<game_id>|<key>" */
	std::map<std::string, std::string> info_state_;
};

} // namespace gamepulse
