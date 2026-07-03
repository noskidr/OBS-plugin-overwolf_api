# CLAUDE.md — GamePulse for OBS

Context for AI assistants (and humans) continuing this project. Read this first.

## What this is

**GamePulse** = a native **OBS Studio plugin** (C++/Qt, Windows) + an **Overwolf
ow‑electron companion app** (Node) that bridges live **Valorant** game events
into OBS: auto chapter markers, replay‑buffer clips, Twitch stream markers, an
on‑stream overlay source, a control/event‑log dock, viewer `!clip`, and
highlight exports (YouTube chapters / CSV / DaVinci EDL).

- **v0.2 (current)** added, still Valorant‑only: companion‑derived **first
  blood / 1vN clutch / round win‑loss / match summary** and rich kill detail
  (weapon→victim, HS, live score/round); **game automation** (auto replay
  buffer on launch, auto‑record or split per match, auto‑export on match end,
  agent‑select privacy‑scene switching); live **overlay context** (agent · map
  · score · round); a dock **Automation** panel + **Test Event** / **Open
  Sessions** buttons; companion **GEP health check** + **getInfo() priming** for
  mid‑match starts. See `CHANGELOG.md`.
- **Scope: Valorant only** (Overwolf game id `21640`). The plugin is
  game‑agnostic; only the companion's normalizer/taxonomy are Valorant‑specific.
- Repo: <https://github.com/noskidr/OBS-plugin-overwolf_api> (remote `origin`,
  branch `main`).
- The two halves talk over a localhost WebSocket + JSON protocol
  (see `PROTOCOL.md`). This is the key seam — you can test the plugin with the
  companion, the built‑in simulator, or `companion/test/harness.js`.

## Repo layout

```
src/                     OBS plugin (C++/Qt). See "Source map" in README.md.
  plugin-main.cpp        module entry (obs_module_load/post_load/unload)
  gp-core.*              pipeline hub; owns subsystems; runs on OBS UI thread
  gp-ws-server.*         embedded RFC6455 WebSocket server (no deps; gp-sha1.h)
  gp-protocol.*          JSON msg -> normalized events; round_phase synthesis
  gp-rules.*             event->action gating + cooldowns; derives multikill/ace
  gp-taxonomy.*          game-id / event-key -> label + importance
  gp-journal.*           session log + YouTube/CSV/EDL exporters
  gp-twitch.*            Device-Code OAuth, Helix markers, anonymous IRC !clip
  gp-overlay-source.cpp  native QPainter overlay source
  gp-dock.*              Qt control + event-log dock
  gp-types.h, gp-clocks.h
companion/               Overwolf ow-electron app (GEP -> WS forwarder) + sim
  src/main.js            app/tray/settings; wires source->normalizer->forwarder
  src/gep-service.js     Overwolf GEP wiring (game 21640)
  src/valorant-normalizer.js  raw GEP -> protocol events (kill_feed detail)
  src/ws-forwarder.js    dependency-free RFC6455 client, reconnect
  src/simulator.js       scripted mock Valorant match
  test/harness.js        headless pipeline test (plain node)
docs/research/           8 primary-source research reports (see its README)
docs/CONTINUATION.md     full environment + resume guide
.claude/skills/obs-plugins/  the OBS-plugin authoring skill (vendored)
CMakeLists.txt, CMakePresets.json, buildspec.json   obs-plugintemplate build
PROTOCOL.md              WebSocket message contract
```

## Build / test / install (Windows)

Toolchain (already set up on the original dev machine; see `docs/CONTINUATION.md`
to reproduce): CMake ≥ 3.28, **VS 2022 Build Tools** with C++ workload +
**Windows 11 SDK 10.0.22621**, and the obs‑plugintemplate deps (auto‑fetched to
`.deps/`).

```powershell
# Configure (first time; downloads OBS 31.1.1 + Qt 6.8.3 + libcurl to .deps/)
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --preset windows-x64

# Build
& '...\cmake.exe' --build --preset windows-x64
# -> build_x64\rundir\RelWithDebInfo\obs-gamepulse.dll (+ .pdb, locale)
```

`cmake` is not on PATH here — use the VS‑bundled one above (v3.30.5), **not**
the system `cmake` 3.21 (too old for the presets).

**Install into a (portable) OBS** — copy:
```
obs-plugins\64bit\obs-gamepulse.dll   (+ .pdb)
data\obs-plugins\obs-gamepulse\locale\en-US.ini
```

**End‑to‑end test without Valorant** (this is the standard verification):
```powershell
# 1. launch OBS (portable dev copy lives at G:\personal\insights_capture\obs-portable)
Start-Process 'G:\personal\insights_capture\obs-portable\bin\64bit\obs64.exe' -ArgumentList '--portable','--minimize-to-tray','--disable-updater'
# 2. feed a mock match through the real pipeline:
cd companion; node test/harness.js --port 4477 --rounds 1
# 3. verify: OBS log shows "companion connected" + a journal session gets
#    written to <obs>\config\obs-studio\plugin_config\obs-gamepulse\sessions\
```

## Verified facts (don't re‑investigate)

- The plugin **compiles clean with MSVC** and **loads on OBS 32.1.2** even though
  built against the template's OBS **31.1.1** headers (the 31→32 ABI concern is
  empirically resolved — it loads and the dock/overlay register).
- The RFC6455 handshake **interops** (C++ SHA‑1 server ↔ the Node client).
- A 5‑kill simulated stream correctly **derives Double Kill + ACE** and writes a
  well‑formed `session.json`. Actions correctly gate to `overlay` when
  recording/replay/streaming are off.
- **CI**: the Windows build + package **passes**. macOS/Ubuntu builds initially
  failed with exit 126 (shell scripts lost their +x bit via Windows robocopy) —
  **fixed** with `git update-index --chmod=+x`. clang‑format 19 + gersemi lint
  gates pass after formatting.
- OBS ships `Qt6Core/Gui/Network/Widgets` + `libcurl.dll`, so the plugin's
  runtime deps resolve. obs‑deps ships prebuilt `libcurl` (found via
  `find_package(CURL)`; `lib/cmake/CURL`).
- Overwolf GEP delivers `{gameId, feature, key, value(, category)}` tuples;
  `value` is often a JSON‑encoded (sometimes URI‑encoded) string. Valorant has
  **no** native `spike_planted`/`round_start`/`round_end` events — the plugin
  synthesizes round boundaries from `round_phase` info transitions.
- Twitch marker scope is `channel:manage:broadcast` (not `user:manage:...`);
  Device Code Grant public client (client_id only, no secret); markers 404 when
  not live / VODs disabled.

## Key decisions & rationale

- **Companion on ow‑electron, not classic Overwolf** — standalone (no Overwolf
  client install), Valorant is PROD on ow‑electron GEP, forwarder is ~200 lines.
- **Self‑contained WebSocket server** (raw sockets + vendored SHA‑1) instead of
  websocketpp/asio — localhost‑only, 1–2 clients, avoids a dep. obs‑deps *does*
  ship websocketpp if we ever want it.
- **Rules engine derives multikill/ace in the plugin**, not the companion, so
  any event source (incl. the simulator) gets consistent derived events.
- **Overlay source renders via QPainter → GS_BGRA texture** on the graphics
  thread (the obs‑text pattern), repainting only when the feed changes.

## Conventions

- All libobs/Qt mutation happens on the **OBS UI thread**; other threads hop via
  `obs_queue_task(OBS_TASK_UI, …)` (see `GpCore::submit_event`). WS reader
  threads, Twitch HTTP/IRC threads, and the graphics thread are the only other
  threads.
- C++ formatted with **clang‑format 19** (`.clang-format`); CMake with
  **gersemi** (`.gersemirc`). Run before committing — CI checks changed files.
- Commit trailers: `Co-Authored-By: Claude Fable 5 …` + `Claude-Session: …`.
- `obs_data_*` / `obs_frontend_get_*` returns are ref‑counted / `bfree`‑owned —
  release exactly once (this was a review focus; see below).

## Current status / what's left

- Core product: **complete and verified** end‑to‑end (tasks 1‑11 done).
- **Adversarial code review** (dynamic workflow) ran; findings are being applied.
  Already fixed: WS shutdown use‑after‑free (client‑thread join), socket
  double‑close race (atomic fd + idempotent close), marker‑worker lost‑wakeup
  hang, query‑token percent‑decode, chat‑permission fail‑closed, JS forwarder
  reconnect‑timer + stale‑socket bugs. See `docs/REVIEW-FINDINGS.md` for the
  full verified list and remaining items.
- Not done: shipping‑grade Overwolf app signing/whitelisting (needs an Overwolf
  developer account); a real live Valorant capture (needs the game + a Twitch
  affiliate account) — see the critic's "verify during implementation" list in
  `docs/research/08-gap-analysis-critic.md`.

## Gotchas

- Use the **VS‑bundled cmake** (PATH `cmake` is 3.21, too old).
- Portable OBS keeps config under `…\obs-portable\config\obs-studio\`, **not**
  `%APPDATA%`.
- `Date.now()`/`Math.random()` are unavailable inside Workflow scripts.
- Shell scripts under `.github/scripts/` need the **+x** bit in git or mac/linux
  CI fails with exit 126 — set via `git update-index --chmod=+x`.
- The research reports in `docs/research/` have some mojibake (em‑dashes) from
  the extraction round‑trip; content is accurate.
