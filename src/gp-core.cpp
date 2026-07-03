/*
GamePulse for OBS — core implementation.
*/

#include "gp-core.h"
#include "gp-taxonomy.h"
#include "gp-twitch.h"

#include <algorithm>
#include <cstring>

#include <obs-module.h>
#include <util/platform.h>
#include <util/base.h>

#include "plugin-support.h"

namespace gamepulse {

namespace {

int64_t mono_ms()
{
	return static_cast<int64_t>(os_gettime_ns() / 1000000ULL);
}

int64_t wall_ms_now()
{
	return static_cast<int64_t>(time(nullptr)) * 1000;
}

std::string sanitize_filename(const std::string &in)
{
	std::string out;
	out.reserve(in.size());
	for (char c : in) {
		unsigned char u = static_cast<unsigned char>(c);
		if (u < 0x20 || strchr("<>:\"/\\|?*", c))
			out += '-';
		else
			out += c;
	}
	/* trim trailing dots/spaces (Windows) */
	while (!out.empty() && (out.back() == '.' || out.back() == ' '))
		out.pop_back();
	return out.empty() ? "clip" : out;
}

void ensure_parent_dir(const std::string &path)
{
	std::string p = path;
	for (char &c : p)
		if (c == '\\')
			c = '/';
	size_t slash = p.find_last_of('/');
	if (slash != std::string::npos)
		os_mkdirs(p.substr(0, slash).c_str());
}

struct QueuedEvent {
	GpEvent ev;
};

} // namespace

GpCore &GpCore::instance()
{
	static GpCore core;
	return core;
}

/* ---------------- config ---------------- */

void GpCore::apply_config_defaults(obs_data_t *cfg)
{
	obs_data_set_default_int(cfg, "port", 4477);
	obs_data_set_default_string(cfg, "token", "");
	obs_data_set_default_bool(cfg, "server_enabled", true);
	obs_data_set_default_bool(cfg, "auto_export", true);
	obs_data_set_default_bool(cfg, "log_debug_events", false);
	obs_data_set_default_bool(cfg, "rename_clips", true);
	obs_data_set_default_bool(cfg, "chapter_on_manual_comment", true);
	obs_data_set_default_int(cfg, "overlay_duration_ms", 4200);
	obs_data_set_default_double(cfg, "caption_duration_sec", 2.0);

	/* game automation */
	obs_data_set_default_bool(cfg, "auto_replay_buffer", true);
	obs_data_set_default_bool(cfg, "auto_record", false);
	obs_data_set_default_bool(cfg, "split_on_match", false);
	obs_data_set_default_bool(cfg, "export_on_match_end", true);
	obs_data_set_default_string(cfg, "scene_game", "");
	obs_data_set_default_string(cfg, "scene_lobby", "");
	obs_data_set_default_string(cfg, "scene_privacy", "");
}

void GpCore::load_config()
{
	char *path = obs_module_config_path("config.json");
	obs_data_t *cfg = nullptr;
	if (path) {
		cfg = obs_data_create_from_json_file_safe(path, "bak");
		bfree(path);
	}
	if (!cfg)
		cfg = obs_data_create();
	apply_config_defaults(cfg);
	config_ = cfg;

	rules_.load(cfg);
}

void GpCore::save_config()
{
	if (!config_)
		return;

	rules_.save(config_);
	save_hotkeys(config_);
	if (twitch_)
		twitch_->save(config_);

	char *path = obs_module_config_path("config.json");
	if (!path)
		return;
	ensure_parent_dir(path);
	if (!obs_data_save_json_safe(config_, path, "tmp", "bak"))
		obs_log(LOG_WARNING, "failed to save config to %s", path);
	bfree(path);
}

std::string GpCore::sessions_dir() const
{
	char *base = obs_module_config_path("sessions");
	std::string dir = base ? base : "";
	if (base)
		bfree(base);
	return dir;
}

/* ---------------- lifecycle ---------------- */

void GpCore::startup()
{
	if (started_)
		return;
	started_ = true;

	load_config();

	journal_.set_base_dir(sessions_dir());

	twitch_.reset(new TwitchService);
	twitch_->load(config_);
	twitch_->on_chat_clip = [this](const std::string &user, const std::string & /*channel*/) {
		GpEvent ev;
		ev.source = EventSource::Chat;
		ev.name = "chat_clip";
		ev.label = "Chat Clip";
		ev.detail = "requested by " + user;
		ev.importance = IMP_NOTABLE;
		ev.wall_ms = wall_ms_now();
		{
			std::lock_guard<std::mutex> lock(status_mutex_);
			ev.game_id = active_game_id_;
			ev.game_name = active_game_name_;
		}
		/* viewer clip requests bypass the rules gates */
		ev.actions_taken = ACTION_CLIP | ACTION_MARKER;
		submit_event(std::move(ev));
	};
	twitch_->start_workers();

	server_.set_on_message([this](int id, const std::string &text) { ws_message(id, text); });
	server_.set_on_connect([this](int id, const std::string &) { ws_connected(id); });
	server_.set_on_disconnect([this](int id) { ws_disconnected(id); });

	if (obs_data_get_bool(config_, "server_enabled")) {
		uint16_t port = (uint16_t)obs_data_get_int(config_, "port");
		server_.start(port, obs_data_get_string(config_, "token"));
	}

	obs_frontend_add_event_callback(frontend_event_cb, this);

	obs_log(LOG_INFO, "core started (port %d)", (int)obs_data_get_int(config_, "port"));
}

void GpCore::post_load()
{
	register_hotkeys();
	load_hotkeys(config_);
}

void GpCore::shutdown()
{
	if (!started_)
		return;
	started_ = false;

	obs_frontend_remove_event_callback(frontend_event_cb, this);

	server_.stop();
	if (twitch_) {
		twitch_->save(config_);
		twitch_->shutdown();
	}

	if (journal_.session_open())
		journal_.end_session(obs_data_get_bool(config_, "auto_export"));

	save_config();

	on_ui_update = nullptr;

	obs_data_release(config_);
	config_ = nullptr;

	obs_log(LOG_INFO, "core shut down");
}

/* ---------------- pipeline ---------------- */

void GpCore::submit_event(GpEvent &&ev)
{
	QueuedEvent *q = new QueuedEvent{std::move(ev)};
	obs_queue_task(
		OBS_TASK_UI,
		[](void *param) {
			QueuedEvent *qe = static_cast<QueuedEvent *>(param);
			GpCore::instance().process_event(qe->ev);
			delete qe;
		},
		q, false);
}

void GpCore::submit_manual(const char *name, const std::string &detail, uint32_t forced_actions)
{
	GpEvent ev;
	ev.source = EventSource::Manual;
	ev.name = name ? name : "manual_bookmark";
	EventMeta meta = Taxonomy::lookup("", ev.name);
	ev.label = meta.label;
	ev.importance = IMP_NOTABLE;
	ev.detail = detail;
	ev.wall_ms = wall_ms_now();
	{
		std::lock_guard<std::mutex> lock(status_mutex_);
		ev.game_id = active_game_id_;
		ev.game_name = active_game_name_;
	}
	/* stash forced actions in actions_taken until process time */
	ev.actions_taken = forced_actions;
	submit_event(std::move(ev));
}

void GpCore::process_event(GpEvent &ev)
{
	if (!started_)
		return;

	int64_t now = mono_ms();
	ev.stream_ms = stream_clock_.elapsed(now);
	ev.record_ms = record_clock_.elapsed(now);

	uint32_t forced = ev.actions_taken;
	ev.actions_taken = 0;

	uint32_t actions;
	if (forced != 0) {
		actions = forced | ACTION_OVERLAY;
	} else {
		actions = rules_.evaluate(ev, now);
	}

	/* match_start automation must run BEFORE actions so an auto-started
	   recording / split file is in place when the chapter fires */
	if (ev.source == EventSource::Gep && ev.name == "match_start")
		run_match_automation(ev);

	execute_actions(ev, actions);

	/* journal (skip debug-level plumbing unless configured) */
	bool log_debug = obs_data_get_bool(config_, "log_debug_events");
	if (ev.importance > IMP_DEBUG || log_debug)
		journal_.append(ev);

	/* match_end automation runs AFTER the journal has the summary event */
	if (ev.source == EventSource::Gep && ev.name == "match_end")
		run_match_automation(ev);

	if (on_ui_update)
		on_ui_update(&ev);

	/* derived events (multikill/ace) ride after the base event */
	if (ev.source == EventSource::Gep) {
		std::vector<GpEvent> derived = rules_.derive(ev);
		for (GpEvent &d : derived) {
			d.stream_ms = stream_clock_.elapsed(now);
			d.record_ms = record_clock_.elapsed(now);
			uint32_t da = rules_.evaluate(d, now);
			execute_actions(d, da);
			journal_.append(d);
			if (on_ui_update)
				on_ui_update(&d);
		}
	}
}

void GpCore::execute_actions(GpEvent &ev, uint32_t actions)
{
	if (actions & ACTION_CHAPTER)
		do_chapter(ev);
	if (actions & ACTION_CLIP)
		do_clip(ev);
	if (actions & ACTION_MARKER)
		do_marker(ev);
	if (actions & ACTION_CAPTION)
		do_caption(ev);
	if (actions & ACTION_OVERLAY)
		do_overlay(ev);

	bump_overlay_stats(ev, ev.actions_taken);
}

/* ---------------- game / match automation (UI thread) ---------------- */

void GpCore::switch_scene(const char *config_key)
{
	const char *name = obs_data_get_string(config_, config_key);
	if (!name || !*name)
		return;
	obs_source_t *scene = obs_get_source_by_name(name);
	if (!scene) {
		obs_log(LOG_WARNING, "scene '%s' (%s) not found", name, config_key);
		return;
	}
	obs_frontend_set_current_scene(scene);
	obs_source_release(scene);
	obs_log(LOG_INFO, "switched to scene '%s' (%s)", name, config_key);
}

void GpCore::on_game_state(bool detected)
{
	if (detected) {
		if (obs_data_get_bool(config_, "auto_replay_buffer") && !obs_frontend_replay_buffer_active()) {
			obs_log(LOG_INFO, "game detected — starting replay buffer");
			obs_frontend_replay_buffer_start();
		}
		switch_scene("scene_game");
	} else {
		/* Gate only on our ownership flag (+ active), NOT the current
		   auto_record setting — else toggling auto_record off mid-match would
		   orphan the recording we started. */
		if (record_started_by_us_ && obs_frontend_recording_active()) {
			record_started_by_us_ = false;
			obs_frontend_recording_stop();
		}
		switch_scene("scene_lobby");
		/* stale match context shouldn't linger on the overlay */
		{
			std::lock_guard<std::mutex> lock(overlay_feed_.mutex);
			overlay_feed_.map.clear();
			overlay_feed_.agent.clear();
			overlay_feed_.score.clear();
			overlay_feed_.round.clear();
			overlay_feed_.version++;
		}
	}
}

void GpCore::apply_context(const std::map<std::string, std::string> &ctx)
{
	bool changed = false;
	{
		std::lock_guard<std::mutex> lock(overlay_feed_.mutex);
		auto set = [&](const char *key, std::string &slot) {
			auto it = ctx.find(key);
			if (it != ctx.end() && it->second != slot) {
				slot = it->second;
				changed = true;
			}
		};
		set("map", overlay_feed_.map);
		set("agent", overlay_feed_.agent);
		set("score", overlay_feed_.score);
		set("round", overlay_feed_.round);
		if (changed)
			overlay_feed_.version++;
	}

	/* anti-stream-snipe scene automation */
	auto scene = ctx.find("scene");
	if (scene != ctx.end()) {
		if (scene->second == "agent_select")
			switch_scene("scene_privacy");
		else if (scene->second == "map")
			switch_scene("scene_game");
		else if (scene->second == "menu")
			switch_scene("scene_lobby");
	}

	if (changed && on_ui_update)
		on_ui_update(nullptr);
}

void GpCore::run_match_automation(const GpEvent &ev)
{
	if (ev.name == "match_start") {
		bool recording = obs_frontend_recording_active();
		if (recording && obs_data_get_bool(config_, "split_on_match")) {
			if (obs_frontend_recording_split_file())
				obs_log(LOG_INFO, "split recording file at match start");
			else
				obs_log(LOG_WARNING, "recording split rejected (needs Hybrid MP4 or fMP4)");
		} else if (!recording && obs_data_get_bool(config_, "auto_record")) {
			obs_log(LOG_INFO, "match started — starting recording");
			/* record_started_by_us_ is set in the RECORDING_STARTED event
			   once the start actually succeeds (a failed start fires no
			   event, so we never claim ownership of a recording that isn't
			   running). */
			record_start_requested_ = true;
			obs_frontend_recording_start();
		}
	} else if (ev.name == "match_end") {
		if (obs_data_get_bool(config_, "export_on_match_end") && journal_.session_open())
			journal_.export_files();
		if (record_started_by_us_ && obs_frontend_recording_active()) {
			record_started_by_us_ = false;
			obs_log(LOG_INFO, "match ended — stopping recording");
			obs_frontend_recording_stop();
		}
	}
}

void GpCore::do_chapter(GpEvent &ev)
{
	if (!obs_frontend_recording_active())
		return;

	std::string name = ev.label;
	if (!ev.detail.empty())
		name += " - " + ev.detail;

	if (obs_frontend_recording_add_chapter(name.c_str())) {
		ev.actions_taken |= ACTION_CHAPTER;
	} else if (!chapter_warned_) {
		chapter_warned_ = true;
		obs_log(LOG_WARNING, "chapter marker rejected — chapters require the Hybrid MP4 recording format "
				     "(Settings → Output → Recording Format)");
	}
}

void GpCore::do_clip(GpEvent &ev)
{
	if (!obs_frontend_replay_buffer_active()) {
		obs_log(LOG_INFO, "clip skipped for '%s' — replay buffer is not running", ev.label.c_str());
		return;
	}

	uint64_t now_ns = os_gettime_ns();
	/* prune stale pendings */
	while (!pending_clips_.empty() && pending_clips_.front().deadline_ns < now_ns)
		pending_clips_.pop_front();

	PendingClip pc;
	pc.label = ev.label + (ev.detail.empty() ? "" : " " + ev.detail);
	pc.game = ev.game_name;
	pc.deadline_ns = now_ns + 20ULL * 1000000000ULL;
	pending_clips_.push_back(pc);

	obs_frontend_replay_buffer_save();
	ev.actions_taken |= ACTION_CLIP;
}

void GpCore::do_marker(GpEvent &ev)
{
	if (!twitch_ || !twitch_->authed())
		return;
	if (!obs_frontend_streaming_active())
		return;

	std::string desc = "[GP] " + ev.label;
	if (!ev.detail.empty())
		desc += " - " + ev.detail;
	if (ev.stream_ms >= 0)
		desc += " @" + format_clock(ev.stream_ms);
	if (desc.size() > 140) {
		/* Twitch caps the description at 140 chars; trim back to a UTF-8
		   boundary so we never send a truncated multibyte sequence. */
		size_t cut = 140;
		while (cut > 0 && (static_cast<unsigned char>(desc[cut]) & 0xC0) == 0x80)
			cut--;
		desc.resize(cut);
	}

	twitch_->queue_marker(desc);
	ev.actions_taken |= ACTION_MARKER;
}

void GpCore::do_caption(GpEvent &ev)
{
	if (!obs_frontend_streaming_active())
		return;
	obs_output_t *out = obs_frontend_get_streaming_output();
	if (!out)
		return;
	std::string text = ev.label;
	if (!ev.detail.empty())
		text += " - " + ev.detail;
	obs_output_output_caption_text2(out, text.c_str(), obs_data_get_double(config_, "caption_duration_sec"));
	obs_output_release(out);
	ev.actions_taken |= ACTION_CAPTION;
}

void GpCore::do_overlay(GpEvent &ev)
{
	int64_t dur = obs_data_get_int(config_, "overlay_duration_ms");
	std::lock_guard<std::mutex> lock(overlay_feed_.mutex);
	OverlayItem item;
	item.label = ev.label;
	item.detail = ev.detail;
	item.importance = ev.importance;
	item.expires_ns = os_gettime_ns() + (uint64_t)dur * 1000000ULL;
	overlay_feed_.items.push_back(std::move(item));
	while (overlay_feed_.items.size() > 6)
		overlay_feed_.items.pop_front();
	overlay_feed_.version++;
	ev.actions_taken |= ACTION_OVERLAY;
}

void GpCore::bump_overlay_stats(const GpEvent &ev, uint32_t actions)
{
	std::lock_guard<std::mutex> lock(overlay_feed_.mutex);
	if (ev.name == "kill" || ev.name == "elimination")
		overlay_feed_.kills++;
	else if (ev.name == "death")
		overlay_feed_.deaths++;
	else if (ev.name == "assist")
		overlay_feed_.assists++;
	if (actions & ACTION_CLIP)
		overlay_feed_.clips++;
	if (actions & ACTION_CHAPTER)
		overlay_feed_.chapters++;
	if (actions & ACTION_MARKER)
		overlay_feed_.markers++;
	if (!ev.game_name.empty())
		overlay_feed_.game_name = ev.game_name;
	overlay_feed_.version++;
}

/* ---------------- frontend events ---------------- */

void GpCore::frontend_event_cb(enum obs_frontend_event event, void *ptr)
{
	static_cast<GpCore *>(ptr)->on_frontend_event(event);
}

void GpCore::on_frontend_event(enum obs_frontend_event event)
{
	int64_t now = mono_ms();

	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		stream_clock_.start(now);
		journal_.begin_session("stream started");
		broadcast_status();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		stream_clock_.stop();
		if (!obs_frontend_recording_active() && journal_.session_open())
			journal_.end_session(obs_data_get_bool(config_, "auto_export"));
		broadcast_status();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		record_clock_.start(now);
		chapter_warned_ = false;
		/* Only claim ownership if WE requested this start; a user-initiated
		   recording must never be auto-stopped by us. */
		record_started_by_us_ = record_start_requested_;
		record_start_requested_ = false;
		journal_.begin_session("recording started");
		broadcast_status();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
		record_clock_.pause(now);
		break;
	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED:
		record_clock_.unpause(now);
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		record_clock_.stop();
		/* Any stop (ours or the user's) ends our ownership, so a later
		   match_end / game-close can't stop a recording the user started next. */
		record_started_by_us_ = false;
		record_start_requested_ = false;
		if (!obs_frontend_streaming_active() && journal_.session_open())
			journal_.end_session(obs_data_get_bool(config_, "auto_export"));
		broadcast_status();
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
		broadcast_status();
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
		on_replay_saved();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
		save_config();
		break;
	default:
		break;
	}

	if (on_ui_update)
		on_ui_update(nullptr);
}

void GpCore::on_replay_saved()
{
	char *last = obs_frontend_get_last_replay();
	if (!last)
		return;
	std::string path = last;
	bfree(last);

	uint64_t now_ns = os_gettime_ns();
	while (!pending_clips_.empty() && pending_clips_.front().deadline_ns < now_ns)
		pending_clips_.pop_front();

	if (!pending_clips_.empty() && obs_data_get_bool(config_, "rename_clips")) {
		PendingClip pc = pending_clips_.front();
		pending_clips_.pop_front();

		std::string p = path;
		for (char &c : p)
			if (c == '\\')
				c = '/';
		size_t slash = p.find_last_of('/');
		size_t dot = p.find_last_of('.');
		std::string dir = (slash == std::string::npos) ? "" : p.substr(0, slash + 1);
		std::string ext = (dot == std::string::npos || dot < slash) ? "" : p.substr(dot);

		char stamp[32];
		time_t t = time(nullptr);
		struct tm tmv;
#ifdef _WIN32
		localtime_s(&tmv, &t);
#else
		localtime_r(&t, &tmv);
#endif
		strftime(stamp, sizeof(stamp), "%H-%M-%S", &tmv);

		std::string base = sanitize_filename((pc.game.empty() ? "" : pc.game + " ") + pc.label);
		std::string target = dir + base + " " + stamp + ext;
		int n = 2;
		while (os_file_exists(target.c_str()) && n < 10) {
			target = dir + base + " " + stamp + "-" + std::to_string(n) + ext;
			n++;
		}

		if (os_rename(path.c_str(), target.c_str()) == 0) {
			obs_log(LOG_INFO, "clip saved: %s", target.c_str());
		} else {
			obs_log(LOG_WARNING, "failed to rename clip %s", path.c_str());
		}
	} else {
		/* External replay save (user's own OBS hotkey) — journal it so the
		   moment shows up in exports. Direct append avoids re-triggering
		   the clip action. */
		GpEvent ev;
		ev.source = EventSource::Manual;
		ev.name = "manual_clip";
		ev.label = "Replay Saved";
		ev.importance = IMP_NOTABLE;
		ev.wall_ms = wall_ms_now();
		int64_t now = mono_ms();
		ev.stream_ms = stream_clock_.elapsed(now);
		ev.record_ms = record_clock_.elapsed(now);
		{
			std::lock_guard<std::mutex> lock(status_mutex_);
			ev.game_id = active_game_id_;
			ev.game_name = active_game_name_;
		}
		ev.actions_taken = ACTION_CLIP;
		journal_.append(ev);
		if (on_ui_update)
			on_ui_update(&ev);
	}
}

void GpCore::broadcast_status()
{
	int64_t now = mono_ms();
	std::string status = Protocol::make_status(obs_frontend_streaming_active(), obs_frontend_recording_active(),
						   obs_frontend_replay_buffer_active(), stream_clock_.elapsed(now),
						   record_clock_.elapsed(now));
	server_.broadcast_text(status);
}

/* ---------------- WS handlers (server threads) ---------------- */

void GpCore::ws_message(int client_id, const std::string &text)
{
	/* Parse on the socket thread (Protocol is only touched from here and
	   the UI thread during shutdown; contention is negligible, but keep
	   parsing off the UI thread for large batches). NOTE: Protocol keeps
	   per-game info state — serialize access with a mutex if more than one
	   client ever streams events concurrently. */
	ParseResult res;
	{
		static std::mutex parse_mutex;
		std::lock_guard<std::mutex> lock(parse_mutex);
		res = protocol_.parse(text);
	}

	switch (res.type) {
	case MsgType::Hello:
		obs_log(LOG_INFO, "companion connected: %s %s", res.client_name.c_str(), res.client_version.c_str());
		break;
	case MsgType::Ping:
		server_.send_text(client_id, Protocol::make_pong());
		break;
	case MsgType::Game: {
		std::lock_guard<std::mutex> lock(status_mutex_);
		if (res.game_state == "closed") {
			if (res.game_id == active_game_id_) {
				active_game_id_.clear();
				active_game_name_.clear();
			}
		} else {
			active_game_id_ = res.game_id;
			active_game_name_ = Taxonomy::game_name(res.game_id, res.game_name);
		}
		break;
	}
	case MsgType::Event:
	case MsgType::Info:
	case MsgType::Batch:
		for (GpEvent &ev : res.events)
			submit_event(std::move(ev));
		break;
	default:
		if (!res.error.empty())
			server_.send_text(client_id, Protocol::make_error(res.error));
		break;
	}

	if (res.type == MsgType::Game) {
		/* game automation + dock notify on the UI thread */
		struct GameTask {
			bool detected;
		};
		GameTask *task = new GameTask{res.game_state != "closed"};
		obs_queue_task(
			OBS_TASK_UI,
			[](void *param) {
				GameTask *t = static_cast<GameTask *>(param);
				GpCore &core = GpCore::instance();
				core.on_game_state(t->detected);
				if (core.on_ui_update)
					core.on_ui_update(nullptr);
				core.broadcast_status();
				delete t;
			},
			task, false);
	}

	if (!res.context.empty()) {
		/* context (map/agent/score/round/scene) → overlay + scene automation */
		auto *ctx = new std::map<std::string, std::string>(std::move(res.context));
		obs_queue_task(
			OBS_TASK_UI,
			[](void *param) {
				auto *c = static_cast<std::map<std::string, std::string> *>(param);
				GpCore::instance().apply_context(*c);
				delete c;
			},
			ctx, false);
	}
}

void GpCore::ws_connected(int client_id)
{
	server_.send_text(client_id, Protocol::make_welcome(PLUGIN_VERSION, obs_get_version_string()));
	int64_t now = mono_ms();
	server_.send_text(client_id,
			  Protocol::make_status(obs_frontend_streaming_active(), obs_frontend_recording_active(),
						obs_frontend_replay_buffer_active(), stream_clock_.elapsed(now),
						record_clock_.elapsed(now)));
	obs_queue_task(
		OBS_TASK_UI,
		[](void *param) {
			GpCore *core = static_cast<GpCore *>(param);
			if (core->on_ui_update)
				core->on_ui_update(nullptr);
		},
		this, false);
}

void GpCore::ws_disconnected(int)
{
	obs_queue_task(
		OBS_TASK_UI,
		[](void *param) {
			GpCore *core = static_cast<GpCore *>(param);
			if (core->on_ui_update)
				core->on_ui_update(nullptr);
		},
		this, false);
}

/* ---------------- misc ---------------- */

CoreStatus GpCore::status() const
{
	CoreStatus s;
	s.server_running = server_.running();
	s.clients = server_.client_count();
	{
		std::lock_guard<std::mutex> lock(status_mutex_);
		s.game_name = active_game_name_;
	}
	s.streaming = obs_frontend_streaming_active();
	s.recording = obs_frontend_recording_active();
	s.replay = obs_frontend_replay_buffer_active();
	if (twitch_) {
		s.twitch_authed = twitch_->authed();
		s.twitch_login = twitch_->login();
		s.chat_listener = twitch_->chat_running();
	}
	{
		std::lock_guard<std::mutex> lock(overlay_feed_.mutex);
		std::string line;
		if (!overlay_feed_.agent.empty())
			line += overlay_feed_.agent;
		if (!overlay_feed_.map.empty())
			line += (line.empty() ? "" : " on ") + overlay_feed_.map;
		if (!overlay_feed_.score.empty())
			line += (line.empty() ? "" : " \xC2\xB7 ") + overlay_feed_.score;
		if (!overlay_feed_.round.empty())
			line += (line.empty() ? "" : " \xC2\xB7 ") + std::string("R") + overlay_feed_.round;
		s.context_line = line;
	}
	return s;
}

bool GpCore::restart_server()
{
	server_.stop();
	if (!obs_data_get_bool(config_, "server_enabled"))
		return true;
	uint16_t port = (uint16_t)obs_data_get_int(config_, "port");
	return server_.start(port, obs_data_get_string(config_, "token"));
}

void GpCore::stop_server()
{
	server_.stop();
}

std::string GpCore::export_now()
{
	if (!journal_.session_open())
		journal_.begin_session("manual export");
	return journal_.export_files();
}

int64_t GpCore::stream_ms_now() const
{
	return stream_clock_.elapsed(mono_ms());
}

int64_t GpCore::record_ms_now() const
{
	return record_clock_.elapsed(mono_ms());
}

/* ---------------- hotkeys ---------------- */

static void hotkey_bookmark(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		GpCore::instance().submit_manual("manual_bookmark", "", ACTION_CHAPTER | ACTION_MARKER);
}

static void hotkey_clip(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		GpCore::instance().submit_manual("manual_clip", "", ACTION_CLIP);
}

static void hotkey_export(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed) {
		obs_queue_task(OBS_TASK_UI, [](void *) { GpCore::instance().export_now(); }, nullptr, false);
	}
}

void GpCore::register_hotkeys()
{
	hk_bookmark_ = obs_hotkey_register_frontend("gamepulse.bookmark", obs_module_text("Hotkey.Bookmark"),
						    hotkey_bookmark, this);
	hk_clip_ = obs_hotkey_register_frontend("gamepulse.clip", obs_module_text("Hotkey.Clip"), hotkey_clip, this);
	hk_export_ =
		obs_hotkey_register_frontend("gamepulse.export", obs_module_text("Hotkey.Export"), hotkey_export, this);
}

void GpCore::save_hotkeys(obs_data_t *cfg)
{
	if (hk_bookmark_ != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *a = obs_hotkey_save(hk_bookmark_);
		obs_data_set_array(cfg, "hotkey_bookmark", a);
		obs_data_array_release(a);
	}
	if (hk_clip_ != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *a = obs_hotkey_save(hk_clip_);
		obs_data_set_array(cfg, "hotkey_clip", a);
		obs_data_array_release(a);
	}
	if (hk_export_ != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *a = obs_hotkey_save(hk_export_);
		obs_data_set_array(cfg, "hotkey_export", a);
		obs_data_array_release(a);
	}
}

void GpCore::load_hotkeys(obs_data_t *cfg)
{
	obs_data_array_t *a;
	if (hk_bookmark_ != OBS_INVALID_HOTKEY_ID && (a = obs_data_get_array(cfg, "hotkey_bookmark"))) {
		obs_hotkey_load(hk_bookmark_, a);
		obs_data_array_release(a);
	}
	if (hk_clip_ != OBS_INVALID_HOTKEY_ID && (a = obs_data_get_array(cfg, "hotkey_clip"))) {
		obs_hotkey_load(hk_clip_, a);
		obs_data_array_release(a);
	}
	if (hk_export_ != OBS_INVALID_HOTKEY_ID && (a = obs_data_get_array(cfg, "hotkey_export"))) {
		obs_hotkey_load(hk_export_, a);
		obs_data_array_release(a);
	}
}

} // namespace gamepulse
