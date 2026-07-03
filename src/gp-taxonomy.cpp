/*
GamePulse for OBS — event taxonomy tables.

Game ids are Overwolf game class ids (https://overwolf.github.io/api/games/ids).
Event keys are Overwolf GEP event names plus GamePulse-derived events
(multikill_N, ace, manual_*). Unknown entries degrade to prettified names.
*/

#include "gp-taxonomy.h"

#include <map>
#include <unordered_map>

namespace gamepulse {

namespace {

const std::unordered_map<std::string, const char *> GAME_NAMES = {
	{"21640", "VALORANT"},
	{"5426", "League of Legends"},
	{"22730", "Counter-Strike 2"},
	{"7764", "CS:GO"},
	{"21566", "Apex Legends"},
	{"21216", "Fortnite"},
	{"10798", "Rocket League"},
	{"7314", "Dota 2"},
	{"10844", "Overwatch 2"},
	{"10826", "Rainbow Six Siege"},
	{"10906", "PUBG"},
	{"21570", "Teamfight Tactics"},
	{"24890", "Marvel Rivals"},
	{"21634", "Escape from Tarkov"},
	{"22328", "Warzone"},
};

struct Meta {
	const char *label;
	int importance;
};

/* Per-game specific labels/importance. Key: event machine name. */
using MetaMap = std::map<std::string, Meta>;

const MetaMap COMMON_EVENTS = {
	{"kill", {"Kill", IMP_NOTABLE}},
	{"death", {"Death", IMP_MINOR}},
	{"assist", {"Assist", IMP_MINOR}},
	{"headshot", {"Headshot", IMP_NOTABLE}},
	{"knockdown", {"Knockdown", IMP_NOTABLE}},
	{"knockout", {"Knockout", IMP_NOTABLE}},
	{"revive", {"Revive", IMP_MINOR}},
	{"level", {"Level Up", IMP_MINOR}},
	/* match_start is NOTABLE so each match gets its own chapter/marker —
	   the detail carries the map ("Match Start - Haven"), which is exactly
	   the YouTube-chapter block editors want. */
	{"match_start", {"Match Start", IMP_NOTABLE}},
	{"matchStart", {"Match Start", IMP_NOTABLE}},
	{"first_blood", {"First Blood", IMP_NOTABLE}},
	{"round_won", {"Round Won", IMP_MINOR}},
	{"round_lost", {"Round Lost", IMP_DEBUG}},
	{"clutch_1v1", {"Clutch 1v1", IMP_NOTABLE}},
	{"clutch_1v2", {"CLUTCH 1v2", IMP_EPIC}},
	{"clutch_1v3", {"CLUTCH 1v3", IMP_EPIC}},
	{"clutch_1v4", {"CLUTCH 1v4", IMP_EPIC}},
	{"clutch_1v5", {"CLUTCH 1v5", IMP_EPIC}},
	{"match_end", {"Match End", IMP_NOTABLE}},
	{"matchEnd", {"Match End", IMP_NOTABLE}},
	{"round_start", {"Round Start", IMP_MINOR}},
	{"round_end", {"Round End", IMP_MINOR}},
	{"victory", {"VICTORY", IMP_EPIC}},
	{"defeat", {"Defeat", IMP_MINOR}},
	{"death_match_over", {"Match Over", IMP_NOTABLE}},
	/* GamePulse derived events */
	{"multikill_2", {"Double Kill", IMP_EPIC}},
	{"multikill_3", {"Triple Kill", IMP_EPIC}},
	{"multikill_4", {"Quadra Kill", IMP_EPIC}},
	{"multikill_5", {"Penta Kill", IMP_EPIC}},
	{"multikill_6", {"Hexa Kill", IMP_EPIC}},
	{"ace", {"ACE", IMP_EPIC}},
	{"kill_streak", {"Kill Streak", IMP_EPIC}},
	/* Manual / chat events */
	{"manual_bookmark", {"Bookmark", IMP_NOTABLE}},
	{"manual_comment", {"Comment", IMP_NOTABLE}},
	{"manual_clip", {"Clip", IMP_NOTABLE}},
	{"chat_clip", {"Chat Clip", IMP_NOTABLE}},
};

const std::unordered_map<std::string, MetaMap> GAME_EVENTS = {
	{"21640", /* VALORANT */
	 {
		 {"spike_planted", {"Spike Planted", IMP_NOTABLE}},
		 {"spike_defused", {"Spike Defused", IMP_EPIC}},
		 {"spike_detonated", {"Spike Detonated", IMP_NOTABLE}},
		 {"ace", {"ACE", IMP_EPIC}},
		 {"multikill_5", {"ACE (5K)", IMP_EPIC}},
	 }},
	{"5426", /* League of Legends */
	 {
		 {"multikill_2", {"Double Kill", IMP_EPIC}},
		 {"multikill_3", {"Triple Kill", IMP_EPIC}},
		 {"multikill_4", {"Quadra Kill", IMP_EPIC}},
		 {"multikill_5", {"PENTAKILL", IMP_EPIC}},
		 {"first_blood", {"First Blood", IMP_EPIC}},
		 {"tower_destroyed", {"Tower Destroyed", IMP_NOTABLE}},
		 {"baron_kill", {"Baron Kill", IMP_EPIC}},
		 {"dragon_kill", {"Dragon Kill", IMP_NOTABLE}},
	 }},
	{"22730", /* CS2 */
	 {
		 {"bomb_planted", {"Bomb Planted", IMP_NOTABLE}},
		 {"bomb_defused", {"Bomb Defused", IMP_EPIC}},
		 {"bomb_exploded", {"Bomb Exploded", IMP_NOTABLE}},
		 {"round_mvp", {"Round MVP", IMP_EPIC}},
		 {"multikill_5", {"ACE (5K)", IMP_EPIC}},
	 }},
	{"21566", /* Apex Legends */
	 {
		 {"kill", {"Kill", IMP_NOTABLE}},
		 {"knockdown", {"Knock", IMP_NOTABLE}},
		 {"respawn", {"Respawn", IMP_MINOR}},
		 {"healed_from_ko", {"Revived", IMP_MINOR}},
		 {"victory", {"CHAMPION!", IMP_EPIC}},
	 }},
	{"21216", /* Fortnite */
	 {
		 {"knockout", {"Knock", IMP_NOTABLE}},
		 {"victory", {"VICTORY ROYALE", IMP_EPIC}},
	 }},
	{"10798", /* Rocket League */
	 {
		 {"goal", {"GOAL!", IMP_EPIC}},
		 {"save", {"Save", IMP_NOTABLE}},
		 {"epic_save", {"Epic Save", IMP_EPIC}},
		 {"shot", {"Shot on Goal", IMP_MINOR}},
		 {"demolish", {"Demolition", IMP_NOTABLE}},
		 {"hat_trick", {"Hat Trick", IMP_EPIC}},
		 {"teamGoal", {"Team Goal", IMP_NOTABLE}},
		 {"opposingTeamGoal", {"Goal Conceded", IMP_MINOR}},
	 }},
	{"7314", /* Dota 2 */
	 {
		 {"kill", {"Kill", IMP_NOTABLE}},
		 {"first_blood", {"First Blood", IMP_EPIC}},
	 }},
	{"10844", /* Overwatch 2 */
	 {
		 {"elimination", {"Elimination", IMP_NOTABLE}},
		 {"final_blow", {"Final Blow", IMP_NOTABLE}},
		 {"healing_done", {"Healing", IMP_DEBUG}},
	 }},
	{"10826", /* Rainbow Six Siege */
	 {
		 {"kill", {"Kill", IMP_NOTABLE}},
		 {"headshot", {"Headshot", IMP_EPIC}},
	 }},
	{"24890", /* Marvel Rivals */
	 {
		 {"kill", {"Kill", IMP_NOTABLE}},
		 {"final_hit", {"Final Hit", IMP_NOTABLE}},
	 }},
};

} // namespace

std::string Taxonomy::game_name(const std::string &game_id, const std::string &fallback)
{
	auto it = GAME_NAMES.find(game_id);
	if (it != GAME_NAMES.end())
		return it->second;
	return fallback.empty() ? (game_id.empty() ? "Unknown Game" : "Game " + game_id) : fallback;
}

std::string Taxonomy::prettify(const std::string &event_name)
{
	std::string out;
	out.reserve(event_name.size() + 4);
	bool cap_next = true;
	for (size_t i = 0; i < event_name.size(); i++) {
		char c = event_name[i];
		if (c == '_' || c == '-') {
			out += ' ';
			cap_next = true;
			continue;
		}
		/* camelCase -> spaced */
		if (i > 0 && c >= 'A' && c <= 'Z' && event_name[i - 1] >= 'a' && event_name[i - 1] <= 'z') {
			out += ' ';
			out += c;
			cap_next = false;
			continue;
		}
		if (cap_next && c >= 'a' && c <= 'z') {
			out += static_cast<char>(c - 'a' + 'A');
			cap_next = false;
		} else {
			out += c;
			cap_next = false;
		}
	}
	return out;
}

EventMeta Taxonomy::lookup(const std::string &game_id, const std::string &event_name)
{
	EventMeta meta;

	auto game_it = GAME_EVENTS.find(game_id);
	if (game_it != GAME_EVENTS.end()) {
		auto ev_it = game_it->second.find(event_name);
		if (ev_it != game_it->second.end()) {
			meta.label = ev_it->second.label;
			meta.importance = ev_it->second.importance;
			meta.known = true;
			return meta;
		}
	}

	auto common_it = COMMON_EVENTS.find(event_name);
	if (common_it != COMMON_EVENTS.end()) {
		meta.label = common_it->second.label;
		meta.importance = common_it->second.importance;
		meta.known = true;
		return meta;
	}

	meta.label = prettify(event_name);
	meta.importance = IMP_MINOR;
	meta.known = false;
	return meta;
}

} // namespace gamepulse
