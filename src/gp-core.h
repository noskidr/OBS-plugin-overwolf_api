/*
GamePulse for OBS — core singleton.

Owns every subsystem and runs the event pipeline on the OBS/Qt main thread:

  WS server (worker threads)  ─┐
  OBS hotkeys (hotkey thread) ─┤ submit_event() → obs_queue_task(OBS_TASK_UI)
  Twitch chat (IRC thread)    ─┤        │
  Dock buttons (UI thread)    ─┘        ▼
                              process_event()  [UI thread]
                                │ derive (multikill/ace)
                                │ rules → actions
                                ├─ chapter marker   (safe any thread, called here)
                                ├─ replay buffer save + rename bookkeeping
                                ├─ Twitch marker    (queued to Twitch worker)
                                ├─ caption          (streaming output)
                                ├─ overlay feed     (shared state, graphics reads)
                                ├─ journal          (+ exporters)
                                └─ dock update      (direct call, same thread)
*/

#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <obs.h>
#include <obs-frontend-api.h>

#include "gp-clocks.h"
#include "gp-journal.h"
#include "gp-protocol.h"
#include "gp-rules.h"
#include "gp-types.h"
#include "gp-ws-server.h"

namespace gamepulse {

class TwitchService;

/* One entry in the overlay's on-screen feed. Written by the pipeline (UI
   thread), read by the overlay source (graphics thread) under the mutex. */
struct OverlayItem {
	std::string label;
	std::string detail;
	int importance = IMP_MINOR;
	uint64_t expires_ns = 0; /* os_gettime_ns() deadline */
};

struct OverlayFeed {
	std::mutex mutex;
	std::deque<OverlayItem> items;
	/* session counters for the stats row */
	int kills = 0, deaths = 0, assists = 0, clips = 0, chapters = 0, markers = 0;
	std::string game_name;
	/* live match context from the companion ("Haven", "Jett", "8-4", "13") */
	std::string map, agent, score, round;
	uint64_t version = 0; /* bumped on every change so the source knows to repaint */
};

struct CoreStatus {
	bool server_running = false;
	int clients = 0;
	std::string game_name;    /* active game ("" if none) */
	std::string context_line; /* "Jett on Haven · 8-4 · R13" ("" if unknown) */
	bool streaming = false;
	bool recording = false;
	bool replay = false;
	bool twitch_authed = false;
	std::string twitch_login;
	bool chat_listener = false;
};

class GpCore {
public:
	static GpCore &instance();

	/* lifecycle (all on UI thread) */
	void startup();   /* obs_module_load */
	void post_load(); /* obs_module_post_load: dock + hotkeys */
	void shutdown();  /* obs_module_unload */

	/* config */
	obs_data_t *config() const { return config_; } /* borrowed */
	void save_config();
	std::string sessions_dir() const;

	/* pipeline entry — any thread */
	void submit_event(GpEvent &&ev);
	/* manual actions from dock/hotkeys; forced_actions==0 → rules decide */
	void submit_manual(const char *name, const std::string &detail, uint32_t forced_actions);

	/* subsystem access (UI thread only) */
	RulesEngine &rules() { return rules_; }
	Journal &journal() { return journal_; }
	TwitchService *twitch() { return twitch_.get(); }
	OverlayFeed &overlay_feed() { return overlay_feed_; }
	Protocol &protocol() { return protocol_; }

	CoreStatus status() const;

	/* server control (UI thread) */
	bool restart_server();
	void stop_server();

	/* dock hook: called on the UI thread after each processed event and on
	   status changes (nullptr allowed) */
	std::function<void(const GpEvent *ev)> on_ui_update;

	/* export now (UI thread); returns session dir or "" */
	std::string export_now();

	/* current clocks (ms since start, -1 inactive) — any thread */
	int64_t stream_ms_now() const;
	int64_t record_ms_now() const;

private:
	GpCore() = default;

	void load_config();
	void apply_config_defaults(obs_data_t *cfg);

	void process_event(GpEvent &ev); /* UI thread */
	void execute_actions(GpEvent &ev, uint32_t actions);
	void do_chapter(GpEvent &ev);
	void do_clip(GpEvent &ev);
	void do_marker(GpEvent &ev);
	void do_caption(GpEvent &ev);
	void do_overlay(GpEvent &ev);
	void bump_overlay_stats(const GpEvent &ev, uint32_t actions);

	static void frontend_event_cb(enum obs_frontend_event event, void *ptr);
	void on_frontend_event(enum obs_frontend_event event);
	void on_replay_saved();
	void broadcast_status();

	/* WS handlers (server threads) */
	void ws_message(int client_id, const std::string &text);
	void ws_connected(int client_id);
	void ws_disconnected(int client_id);

	/* automation (UI thread) */
	void apply_context(const std::map<std::string, std::string> &ctx);
	void on_game_state(bool detected);
	void switch_scene(const char *config_key);
	void run_match_automation(const GpEvent &ev);

	void register_hotkeys();
	void save_hotkeys(obs_data_t *cfg);
	void load_hotkeys(obs_data_t *cfg);

	/* state */
	obs_data_t *config_ = nullptr;
	WsServer server_;
	Protocol protocol_;
	RulesEngine rules_;
	Journal journal_;
	std::unique_ptr<TwitchService> twitch_;
	/* mutable: const status() composes the context line under its mutex */
	mutable OverlayFeed overlay_feed_;

	SessionClock stream_clock_;
	SessionClock record_clock_;

	std::string active_game_id_;
	std::string active_game_name_;

	struct PendingClip {
		std::string label;
		std::string game;
		uint64_t deadline_ns = 0;
	};
	std::deque<PendingClip> pending_clips_;

	obs_hotkey_id hk_bookmark_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hk_clip_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hk_export_ = OBS_INVALID_HOTKEY_ID;

	bool chapter_warned_ = false;
	bool started_ = false;
	bool record_started_by_us_ = false;   /* we own the current recording */
	bool record_start_requested_ = false; /* we asked to start; await STARTED */

	mutable std::mutex status_mutex_; /* guards active_game_* for status() */
};

} // namespace gamepulse
