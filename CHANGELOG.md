# Changelog

All notable changes to GamePulse for OBS. Valorant‑only (Overwolf game 21640).

## 0.2.0

New Valorant moments (companion‑derived from roster/score state that only the
companion sees; multikills/aces stay plugin‑derived from the kill stream):

- **First blood** — the round's first kill when it's the local player's.
- **1vN clutch** (`clutch_1v1`..`clutch_1v5`) — local player alone vs N living
  enemies, then the round is won. Derived from live scoreboard alive‑states.
- **Round won / lost** from score changes, and a **match summary** on match end
  (`8‑4 · Ascent · Jett · 24/11/6 · HS 32%`).
- **Richer kill labels** — `Vandal → Reyna (HS) · 8‑4 · R13` (weapon, victim,
  headshot, live score/round). Kill feed is now attributed to the local player
  only (fixes counting every player's kills as the streamer's).

Game automation (OBS frontend):

- **Auto replay buffer** — starts when Valorant is detected.
- **Auto‑record per match** — start/stop with the match, or **split the
  recording file** at match start instead.
- **Auto‑export** highlights when a match ends.
- **Scene automation** — switch to a privacy scene during agent select
  (anti‑stream‑snipe), an in‑game scene on map load, a lobby scene in menus.

Live context:

- Overlay stats bar and dock show **agent · map · score · round** from GEP
  state; cleared when the game closes.

Dock:

- New **Game Automation** panel (toggles + scene pickers, populated from your
  OBS scenes), a **Test Event** button (fires a fake kill through the full
  pipeline to verify setup without the game), and **Open Sessions**.

Companion:

- **GEP health check** on start (surfaces Overwolf's per‑game event status).
- **`getInfo()` priming** — if the companion starts mid‑match, current player /
  agent / map / score / round are applied immediately.
- Simulator now scripts a realistic match (agent select → map → ace round →
  1v3 clutch → match end) exercising every derivation; harness takes `--seconds`.

## 0.1.0

First release. OBS plugin: localhost WebSocket server, rules engine
(event→action gating + cooldowns, multikill/ace derivation), recording chapter
markers, replay‑buffer auto‑clips with metadata rename, Twitch stream markers
(Device Code Grant OAuth), CEA‑608 captions, native overlay source, control /
event‑log dock, hotkeys, session journal + YouTube/CSV/EDL exports, viewer
`!clip` via anonymous Twitch IRC. Overwolf ow‑electron companion: GEP wiring for
Valorant, event normalizer, WS forwarder, tray UI, match simulator + headless
harness. Windows‑only CI. Adversarial code review (27 fixes).
