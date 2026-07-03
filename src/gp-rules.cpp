/*
GamePulse for OBS — rules engine implementation.
*/

#include "gp-rules.h"
#include "gp-taxonomy.h"

#include <algorithm>

#include <util/base.h>

namespace gamepulse {

const uint32_t RulesEngine::ALL_ACTIONS[5] = {ACTION_CHAPTER, ACTION_CLIP, ACTION_MARKER, ACTION_OVERLAY,
					      ACTION_CAPTION};

namespace {

const char *action_key(uint32_t bit)
{
	switch (bit) {
	case ACTION_CHAPTER:
		return "chapter";
	case ACTION_CLIP:
		return "clip";
	case ACTION_MARKER:
		return "marker";
	case ACTION_OVERLAY:
		return "overlay";
	case ACTION_CAPTION:
		return "caption";
	default:
		return "unknown";
	}
}

bool is_kill_event(const std::string &name)
{
	return name == "kill" || name == "elimination" || name == "final_blow" || name == "knockout";
}

bool is_round_boundary(const std::string &name)
{
	/* round_start/round_end may be synthesized by the protocol layer from
	   round_phase info transitions for games without native round events */
	return name == "round_start" || name == "roundStart" || name == "round_end" || name == "roundEnd" ||
	       name == "match_start" || name == "matchStart" || name == "match_end" || name == "matchEnd" ||
	       name == "death";
}

} // namespace

RulesEngine::RulesEngine()
{
	/* Sensible out-of-the-box behavior:
	   - chapters for notable+ events while recording
	   - auto clips only for epic events (replay buffer must be on)
	   - Twitch markers for notable+ (only fires once authenticated)
	   - overlay shows minor+
	   - captions off (niche) */
	actions_[ACTION_CHAPTER] = {true, IMP_NOTABLE, 10000};
	actions_[ACTION_CLIP] = {true, IMP_EPIC, 30000};
	actions_[ACTION_MARKER] = {true, IMP_NOTABLE, 15000};
	actions_[ACTION_OVERLAY] = {true, IMP_MINOR, 750};
	actions_[ACTION_CAPTION] = {false, IMP_EPIC, 5000};
}

void RulesEngine::load(obs_data_t *root)
{
	obs_data_t *rules = obs_data_get_obj(root, "rules");
	if (!rules)
		return;

	obs_data_t *acts = obs_data_get_obj(rules, "actions");
	if (acts) {
		for (uint32_t bit : ALL_ACTIONS) {
			obs_data_t *a = obs_data_get_obj(acts, action_key(bit));
			if (!a)
				continue;
			ActionConfig cfg = actions_[bit];
			/* presence-checked reads: keep defaults when absent */
			if (obs_data_has_user_value(a, "enabled"))
				cfg.enabled = obs_data_get_bool(a, "enabled");
			if (obs_data_has_user_value(a, "min_importance"))
				cfg.min_importance = (int)obs_data_get_int(a, "min_importance");
			if (obs_data_has_user_value(a, "cooldown_ms"))
				cfg.cooldown_ms = (int)obs_data_get_int(a, "cooldown_ms");
			actions_[bit] = cfg;
			obs_data_release(a);
		}
		obs_data_release(acts);
	}

	overrides_.clear();
	obs_data_array_t *ovs = obs_data_get_array(rules, "overrides");
	if (ovs) {
		size_t n = obs_data_array_count(ovs);
		for (size_t i = 0; i < n; i++) {
			obs_data_t *o = obs_data_array_item(ovs, i);
			const char *key = obs_data_get_string(o, "key");
			if (key && *key) {
				EventOverride ov;
				ov.force_on = (uint32_t)obs_data_get_int(o, "on");
				ov.force_off = (uint32_t)obs_data_get_int(o, "off");
				overrides_[key] = ov;
			}
			obs_data_release(o);
		}
		obs_data_array_release(ovs);
	}

	obs_data_t *derive = obs_data_get_obj(rules, "derive");
	if (derive) {
		if (obs_data_has_user_value(derive, "multikill"))
			multikill_enabled_ = obs_data_get_bool(derive, "multikill");
		if (obs_data_has_user_value(derive, "window_ms"))
			multikill_window_ms_ = (int)obs_data_get_int(derive, "window_ms");
		if (obs_data_has_user_value(derive, "ace"))
			ace_enabled_ = obs_data_get_bool(derive, "ace");
		obs_data_release(derive);
	}

	obs_data_release(rules);
}

void RulesEngine::save(obs_data_t *root) const
{
	obs_data_t *rules = obs_data_create();

	obs_data_t *acts = obs_data_create();
	for (uint32_t bit : ALL_ACTIONS) {
		auto it = actions_.find(bit);
		if (it == actions_.end())
			continue;
		obs_data_t *a = obs_data_create();
		obs_data_set_bool(a, "enabled", it->second.enabled);
		obs_data_set_int(a, "min_importance", it->second.min_importance);
		obs_data_set_int(a, "cooldown_ms", it->second.cooldown_ms);
		obs_data_set_obj(acts, action_key(bit), a);
		obs_data_release(a);
	}
	obs_data_set_obj(rules, "actions", acts);
	obs_data_release(acts);

	obs_data_array_t *ovs = obs_data_array_create();
	for (const auto &kv : overrides_) {
		obs_data_t *o = obs_data_create();
		obs_data_set_string(o, "key", kv.first.c_str());
		obs_data_set_int(o, "on", kv.second.force_on);
		obs_data_set_int(o, "off", kv.second.force_off);
		obs_data_array_push_back(ovs, o);
		obs_data_release(o);
	}
	obs_data_set_array(rules, "overrides", ovs);
	obs_data_array_release(ovs);

	obs_data_t *derive = obs_data_create();
	obs_data_set_bool(derive, "multikill", multikill_enabled_);
	obs_data_set_int(derive, "window_ms", multikill_window_ms_);
	obs_data_set_bool(derive, "ace", ace_enabled_);
	obs_data_set_obj(rules, "derive", derive);
	obs_data_release(derive);

	obs_data_set_obj(root, "rules", rules);
	obs_data_release(rules);
}

ActionConfig RulesEngine::action_config(uint32_t action_bit) const
{
	auto it = actions_.find(action_bit);
	return it == actions_.end() ? ActionConfig{} : it->second;
}

void RulesEngine::set_action_config(uint32_t action_bit, const ActionConfig &cfg)
{
	actions_[action_bit] = cfg;
}

void RulesEngine::set_override(const std::string &key, const EventOverride &ov)
{
	if (ov.force_on == 0 && ov.force_off == 0)
		overrides_.erase(key);
	else
		overrides_[key] = ov;
}

void RulesEngine::clear_override(const std::string &key)
{
	overrides_.erase(key);
}

void RulesEngine::set_derive(bool multikill, int window_ms, bool ace)
{
	multikill_enabled_ = multikill;
	multikill_window_ms_ = std::max(1000, window_ms);
	ace_enabled_ = ace;
}

uint32_t RulesEngine::evaluate(const GpEvent &ev, int64_t now_ms)
{
	uint32_t result = 0;

	const EventOverride *ov = nullptr;
	{
		auto it = overrides_.find(ev.game_id + ":" + ev.name);
		if (it == overrides_.end())
			it = overrides_.find("*:" + ev.name);
		if (it != overrides_.end())
			ov = &it->second;
	}

	for (uint32_t bit : ALL_ACTIONS) {
		const ActionConfig cfg = action_config(bit);
		bool fire;
		if (ov && (ov->force_on & bit))
			fire = true;
		else if (ov && (ov->force_off & bit))
			fire = false;
		else
			fire = cfg.enabled && ev.importance >= cfg.min_importance;

		if (!fire)
			continue;

		/* Manual requests skip cooldowns — the human clicked on purpose. */
		if (ev.source == EventSource::Gep && cfg.cooldown_ms > 0) {
			auto last = last_fire_ms_.find(bit);
			if (last != last_fire_ms_.end() && now_ms - last->second < cfg.cooldown_ms)
				continue;
		}

		last_fire_ms_[bit] = now_ms;
		result |= bit;
	}

	return result;
}

std::vector<GpEvent> RulesEngine::derive(const GpEvent &ev)
{
	std::vector<GpEvent> out;

	if (ev.source != EventSource::Gep)
		return out;

	if (ev.game_id != current_game_id_) {
		current_game_id_ = ev.game_id;
		reset_derive_state();
	}

	if (is_round_boundary(ev.name)) {
		/* death also breaks a kill chain; round/match boundaries reset the round counter */
		kill_times_ms_.clear();
		max_multikill_announced_ = 1;
		if (ev.name != "death")
			round_kills_ = 0;
		return out;
	}

	if (!is_kill_event(ev.name))
		return out;

	int64_t t = ev.wall_ms;
	round_kills_++;

	if (multikill_enabled_) {
		kill_times_ms_.push_back(t);
		int64_t cutoff = t - multikill_window_ms_;
		kill_times_ms_.erase(std::remove_if(kill_times_ms_.begin(), kill_times_ms_.end(),
						    [&](int64_t k) { return k < cutoff; }),
				     kill_times_ms_.end());

		int chain = (int)kill_times_ms_.size();
		if (chain <= 1)
			max_multikill_announced_ = 1; /* previous chain expired; allow a fresh one */
		if (chain >= 2 && chain > max_multikill_announced_ && chain <= 6) {
			max_multikill_announced_ = chain;
			GpEvent d = ev;
			d.name = "multikill_" + std::to_string(chain);
			EventMeta meta = Taxonomy::lookup(d.game_id, d.name);
			d.label = meta.label;
			d.importance = meta.importance;
			d.detail = std::to_string(chain) + " kills in " + std::to_string(multikill_window_ms_ / 1000) +
				   "s";
			out.push_back(std::move(d));
		}
	}

	if (ace_enabled_ && round_kills_ == 5) {
		GpEvent d = ev;
		d.name = "ace";
		EventMeta meta = Taxonomy::lookup(d.game_id, d.name);
		d.label = meta.label;
		d.importance = meta.importance;
		d.detail = "5 kills this round";
		out.push_back(std::move(d));
	}

	return out;
}

void RulesEngine::reset_derive_state()
{
	kill_times_ms_.clear();
	round_kills_ = 0;
	max_multikill_announced_ = 1;
}

} // namespace gamepulse
