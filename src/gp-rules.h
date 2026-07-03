/*
GamePulse for OBS — rules engine.

Decides which actions fire for each incoming event:
  action fires  <=>  per-event override says so, or
                     (action enabled globally AND event importance >= action threshold)
then per-action cooldowns are applied (manual events bypass cooldowns).

Also derives higher-order events from the raw stream:
  multikill_N — N kills within a sliding window
  ace         — 5 kills within one round (round_start resets)

Not thread-safe by itself; call only from the plugin's main-thread executor.
*/

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <obs-data.h>

#include "gp-types.h"

namespace gamepulse {

struct ActionConfig {
	bool enabled = false;
	int min_importance = IMP_NOTABLE;
	int cooldown_ms = 15000;
};

struct EventOverride {
	uint32_t force_on = 0;  /* actions forced on for this event */
	uint32_t force_off = 0; /* actions forced off */
};

class RulesEngine {
public:
	RulesEngine();

	/* ---- config ---- */
	void load(obs_data_t *root); /* reads "rules" object if present */
	void save(obs_data_t *root) const;

	ActionConfig action_config(uint32_t action_bit) const;
	void set_action_config(uint32_t action_bit, const ActionConfig &cfg);

	/* Override key: "<game_id>:<event_name>" (game_id may be "*"). */
	const std::map<std::string, EventOverride> &overrides() const { return overrides_; }
	void set_override(const std::string &key, const EventOverride &ov);
	void clear_override(const std::string &key);

	bool multikill_enabled() const { return multikill_enabled_; }
	int multikill_window_ms() const { return multikill_window_ms_; }
	bool ace_enabled() const { return ace_enabled_; }
	void set_derive(bool multikill, int window_ms, bool ace);

	/* ---- runtime ---- */

	/* Which actions should fire for this event (cooldowns applied and
	   recorded). now_ms is a monotonic-ish clock in ms. */
	uint32_t evaluate(const GpEvent &ev, int64_t now_ms);

	/* Feed raw events; returns derived events (multikill/ace) to inject.
	   Derived events inherit game/timestamps from the trigger event. */
	std::vector<GpEvent> derive(const GpEvent &ev);

	/* Forget kill windows / round state (game change, match end). */
	void reset_derive_state();

private:
	static const uint32_t ALL_ACTIONS[5];

	std::map<uint32_t, ActionConfig> actions_;
	std::map<std::string, EventOverride> overrides_;
	std::map<uint32_t, int64_t> last_fire_ms_;

	bool multikill_enabled_ = true;
	int multikill_window_ms_ = 8000;
	bool ace_enabled_ = true;

	/* derive state */
	std::vector<int64_t> kill_times_ms_;
	int round_kills_ = 0;
	int max_multikill_announced_ = 1;
	std::string current_game_id_;
};

} // namespace gamepulse
