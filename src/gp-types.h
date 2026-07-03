/*
GamePulse for OBS — core types shared across the plugin.
*/

#pragma once

#include <cstdint>
#include <string>

#include <obs-data.h>

namespace gamepulse {

/* Actions the rules engine can trigger for an event. Bitmask. */
enum ActionBits : uint32_t {
	ACTION_NONE = 0,
	ACTION_LOG = 1u << 0,     /* dock event log + journal (always implied for real events) */
	ACTION_CHAPTER = 1u << 1, /* recording chapter marker */
	ACTION_CLIP = 1u << 2,    /* save replay buffer */
	ACTION_MARKER = 1u << 3,  /* Twitch stream marker */
	ACTION_OVERLAY = 1u << 4, /* show on overlay source */
	ACTION_CAPTION = 1u << 5, /* CEA-608 caption on stream output */
};

/* Importance scale used for filtering. */
enum Importance : int {
	IMP_DEBUG = 0,   /* info updates, roster churn */
	IMP_MINOR = 1,   /* routine events: death, assist, round start */
	IMP_NOTABLE = 2, /* kill, headshot, objective */
	IMP_EPIC = 3,    /* multikill, ace, clutch, victory */
};

/* Where an event came from. */
enum class EventSource {
	Gep,    /* companion / Overwolf GEP */
	Manual, /* dock button or OBS hotkey */
	Chat,   /* viewer chat command (!clip) */
};

/* A normalized game event flowing through the pipeline. */
struct GpEvent {
	int64_t wall_ms = 0;    /* epoch milliseconds */
	int64_t stream_ms = -1; /* ms since stream start, -1 if not streaming */
	int64_t record_ms = -1; /* ms since recording start (pause-adjusted), -1 if not recording */
	std::string game_id;    /* numeric id as string, e.g. "21640"; empty for manual */
	std::string game_name;  /* "VALORANT" */
	std::string name;       /* machine key: "kill", "spike_planted", "multikill_3", "manual_bookmark" */
	std::string label;      /* display: "Kill", "Triple Kill" */
	std::string detail;     /* optional human detail: victim, weapon, note text */
	int importance = IMP_MINOR;
	EventSource source = EventSource::Gep;
	uint32_t actions_taken = ACTION_NONE; /* filled by the action executor */

	/* Raw payload (may be null). Owned; released in destructor. */
	obs_data_t *data = nullptr;

	GpEvent() = default;
	GpEvent(const GpEvent &other) { copy_from(other); }
	GpEvent &operator=(const GpEvent &other)
	{
		if (this != &other) {
			obs_data_release(data);
			copy_from(other);
		}
		return *this;
	}
	GpEvent(GpEvent &&other) noexcept { move_from(std::move(other)); }
	GpEvent &operator=(GpEvent &&other) noexcept
	{
		if (this != &other) {
			obs_data_release(data);
			move_from(std::move(other));
		}
		return *this;
	}
	~GpEvent() { obs_data_release(data); }

private:
	void copy_from(const GpEvent &o)
	{
		wall_ms = o.wall_ms;
		stream_ms = o.stream_ms;
		record_ms = o.record_ms;
		game_id = o.game_id;
		game_name = o.game_name;
		name = o.name;
		label = o.label;
		detail = o.detail;
		importance = o.importance;
		source = o.source;
		actions_taken = o.actions_taken;
		data = o.data;
		if (data)
			obs_data_addref(data);
	}
	void move_from(GpEvent &&o)
	{
		wall_ms = o.wall_ms;
		stream_ms = o.stream_ms;
		record_ms = o.record_ms;
		game_id = std::move(o.game_id);
		game_name = std::move(o.game_name);
		name = std::move(o.name);
		label = std::move(o.label);
		detail = std::move(o.detail);
		importance = o.importance;
		source = o.source;
		actions_taken = o.actions_taken;
		data = o.data;
		o.data = nullptr;
	}
};

/* mm:ss or h:mm:ss for display / chapter names */
inline std::string format_clock(int64_t ms)
{
	if (ms < 0)
		return "--:--";
	int64_t total_s = ms / 1000;
	int64_t h = total_s / 3600;
	int64_t m = (total_s % 3600) / 60;
	int64_t s = total_s % 60;
	char buf[32];
	if (h > 0)
		snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)s);
	else
		snprintf(buf, sizeof(buf), "%02lld:%02lld", (long long)m, (long long)s);
	return buf;
}

/* YouTube chapter timestamps are h:mm:ss / m:ss with no leading zero-hour */
inline std::string format_youtube(int64_t ms)
{
	if (ms < 0)
		ms = 0;
	int64_t total_s = ms / 1000;
	int64_t h = total_s / 3600;
	int64_t m = (total_s % 3600) / 60;
	int64_t s = total_s % 60;
	char buf[32];
	if (h > 0)
		snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)s);
	else
		snprintf(buf, sizeof(buf), "%lld:%02lld", (long long)m, (long long)s);
	return buf;
}

} // namespace gamepulse
