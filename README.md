# GamePulse for OBS

**Turn live game events into OBS actions.** GamePulse is a native OBS Studio
plugin plus an [Overwolf](https://overwolf.github.io/) companion app that
bridges real‑time in‑game events (kills, headshots, spike defuses, aces, …)
into your stream and recordings — automatically dropping **chapter markers**,
saving **replay clips**, posting **Twitch stream markers**, driving an
on‑stream **event overlay**, and exporting an **editor‑ready highlight list**
when you're done.

> **v1 targets Valorant** (Overwolf game id `21640`). The plugin itself is
> game‑agnostic — any game the companion forwards events for works — but the
> shipped companion normalizer and event taxonomy are tuned for Valorant.

Repository: <https://github.com/noskidr/OBS-plugin-overworlf_api>

---

## Why

Streamers and their editors constantly re‑watch VODs hunting for the moment a
fight popped off. Twitch's only marker tool is a manual `/marker`; OBS chapter
markers are a manual hotkey; clip tools like Medal don't touch your OBS stream.
GamePulse closes that gap: the game itself tells OBS when something happened, so
the highlights mark **themselves** — live, at the moment they occur, with the
weapon/victim/headshot detail baked into the label.

## What it does

| Feature | How it works |
|---|---|
| **Recording chapter markers** | On notable events, calls `obs_frontend_recording_add_chapter()` (needs Hybrid MP4). Jump straight to each kill/ace when editing. |
| **Auto replay clips** | On epic events (ace, clutch, multikill) triggers the OBS replay buffer and renames the saved file `VALORANT Ace 21-04-33.mp4`. |
| **Twitch stream markers** | Posts to Helix `POST /streams/markers` at the live position, e.g. `[GP] ACE - 5 kills this round @12:41`. Device‑Code‑Grant login, no server needed. |
| **On‑stream overlay** | A native OBS source ("GamePulse Overlay") renders a live event feed + K/D/A + clip counter. Add it like any source. |
| **Viewer `!clip` command** | Reads Twitch chat anonymously; a permission‑gated, cooldown‑limited `!clip` saves a replay. Mods‑only by default. |
| **Control + event‑log dock** | A dockable panel: manual Bookmark / Comment / Clip / Export buttons, live event log, server + Twitch settings. |
| **Highlight exports** | Every session writes `youtube-chapters.txt`, `events.csv`, `markers.edl` (DaVinci Resolve), and `session.json`. |
| **Derived events** | Multikills and aces are computed from the raw kill stream by the plugin's rules engine, so they're consistent regardless of source. |

## Architecture

```
 ┌───────────────────────────┐        localhost WebSocket         ┌────────────────────────────┐
 │  GamePulse Companion       │   ws://127.0.0.1:4477  (JSON)      │  OBS Studio                 │
 │  (Overwolf ow-electron)    │ ────────────────────────────────► │  GamePulse plugin (C++/Qt)  │
 │                            │                                    │                             │
 │  Overwolf GEP (game 21640) │   {"t":"event","name":"kill",...}  │  WS server → rules engine → │
 │   → Valorant normalizer    │                                    │   chapters / clips /        │
 │   → WS forwarder           │ ◄──────────────────────────────── │   Twitch markers / overlay /│
 │  (+ tray UI, simulator)    │   {"t":"status",...} {"welcome"}   │   dock / journal + exports  │
 └───────────────────────────┘                                    └────────────────────────────┘
```

The companion uses Overwolf's **official** Game Events Provider (the same engine
behind insights.gg / Outplayed), so there's **no memory reading or anti‑cheat
risk on your side** — Overwolf's signed, Riot‑approved engine does that, and you
only receive structured events. See [companion/README.md](companion/README.md).

The two halves are decoupled by a small JSON‑over‑WebSocket protocol
([PROTOCOL.md](PROTOCOL.md)), so you can drive the plugin from anything — the
included companion, the built‑in match **simulator**, or your own script.

---

## Install (users)

### 1. The OBS plugin

Grab `obs-gamepulse-<version>-windows-x64.zip` from
[Releases](https://github.com/noskidr/OBS-plugin-overworlf_api/releases) (or
build it — see below) and copy into your OBS install (or `%APPDATA%\obs-studio`):

```
obs-plugins\64bit\obs-gamepulse.dll
data\obs-plugins\obs-gamepulse\locale\en-US.ini
```

Launch OBS → you'll see a **GamePulse** dock (View → Docks if hidden) and a
**GamePulse Overlay** source type. Confirm it loaded via *Help → Log Files →
View Current Log* (search `obs-gamepulse`).

Requires **OBS Studio 30.2+** (chapter‑marker API); tested on **32.1.2**.

### 2. The companion

See [companion/README.md](companion/README.md). In short: install Node deps,
`npm start` (runs under `ow-electron`), set the same port as the dock, done.
No Valorant handy? Use the **simulator** (tray → *Start simulator*) to see the
whole thing work end‑to‑end.

### 3. Recommended OBS settings

- **Recording format → Hybrid MP4** (Settings → Output → Recording) so chapter
  markers work. GamePulse logs a warning if you try to chapter into a format
  that doesn't support it.
- **Enable the Replay Buffer** (Settings → Output → Replay Buffer) so auto‑clips
  can save. Set the buffer to ~30 s.
- Add a **GamePulse Overlay** source to your scene for the on‑stream feed.

---

## Build (developers)

Prerequisites: **CMake ≥ 3.28**, **Visual Studio 2022** (v17) with the C++
workload + **Windows 11 SDK 10.0.22621**. The template fetches OBS 31.1.1 +
Qt 6.8.3 + libcurl automatically into `.deps/`.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
```

Output: `build_x64\rundir\RelWithDebInfo\obs-gamepulse.dll`. Install it locally
with the layout above, or let CI package it — the repo ships the
obs‑plugintemplate GitHub Actions (build on push/PR for Windows/macOS/Linux).

The plugin is built from
[obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) with
`ENABLE_QT` + `ENABLE_FRONTEND_API` on and vendors nothing exotic — the
WebSocket server is a self‑contained RFC 6455 implementation (`gp-ws-server.*`,
`gp-sha1.h`), so there's no websocketpp/asio dependency.

### Source map

| File | Responsibility |
|---|---|
| `src/plugin-main.cpp` | module entry; registers source, dock, core |
| `src/gp-core.*` | the pipeline hub; owns every subsystem; runs on the UI thread |
| `src/gp-ws-server.*` | embedded localhost WebSocket server (RFC 6455) |
| `src/gp-protocol.*` | JSON message parsing → normalized events; round‑phase synthesis |
| `src/gp-rules.*` | event → actions gating + cooldowns; derives multikill/ace |
| `src/gp-taxonomy.*` | game‑id + event‑key → label/importance tables |
| `src/gp-journal.*` | session log + YouTube/CSV/EDL exporters |
| `src/gp-twitch.*` | Device‑Code OAuth, Helix markers, anonymous IRC `!clip` |
| `src/gp-overlay-source.cpp` | native QPainter‑rendered overlay source |
| `src/gp-dock.*` | Qt control + event‑log dock |
| `companion/` | Overwolf ow‑electron app (GEP → WS forwarder) + simulator |

---

## Twitch setup (optional)

Stream markers and `!clip` need a Twitch app **Client ID**:

1. <https://dev.twitch.tv/console> → *Register Your Application*. Set OAuth
   Redirect URL to `http://localhost`, **Client Type = Public**, any category.
2. Copy the **Client ID** into the GamePulse dock → Twitch → *Client ID*.
3. Click **Connect** → a browser opens with a code → authorize. Done; the token
   refreshes itself and persists.

Stream markers only land while you're **live with VODs enabled** (Twitch
requirement) — GamePulse logs a skip if you're not. `!clip` reads chat
anonymously (no extra scopes) and is **mods‑only** by default; change who can
trigger it in the dock.

## Exports

When a recording/stream ends (or on demand via the dock **Export** button or
hotkey), GamePulse writes to
`…\obs-studio\plugin_config\obs-gamepulse\sessions\<timestamp>\`:

- **`youtube-chapters.txt`** — paste into your YouTube description (first at
  `0:00`, ≥10 s apart, ≥3 entries — YouTube's rules).
- **`events.csv`** — every event with stream/record timecodes; open in any
  spreadsheet or import to Premiere.
- **`markers.edl`** — a DaVinci Resolve marker EDL (color‑coded by importance).
- **`session.json`** — the raw event log.

## Configuration

Everything is in the dock, persisted to
`…\obs-studio\plugin_config\obs-gamepulse\config.json`:

- **Event server** — port (default `4477`) and an optional shared **token**
  (require the companion to present it; blocks random web pages from injecting
  fake events into `ws://127.0.0.1`).
- **Rules** — per‑action enable + minimum importance + cooldown; per‑event
  overrides. Defaults: chapters for notable+ events, clips for epic events only,
  markers for notable+, overlay for everything.

## Security note

The event server binds **`127.0.0.1` only**. Because browsers permit
`ws://127.0.0.1` connections from any page, set a **token** in the dock if you're
concerned about a malicious website spamming markers/clips — the companion sends
it automatically.

## License

GPL‑2.0‑or‑later (matching OBS Studio). See [LICENSE](LICENSE).

## Credits

Built with [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).
Game events via [Overwolf GEP](https://overwolf.github.io/api/live-game-data/).
Not affiliated with OBS Project, Overwolf, Riot Games, or Twitch.
