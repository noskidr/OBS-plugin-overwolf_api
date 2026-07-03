# Changelog

All notable changes to GamePulse for OBS. Valorant‑only (Overwolf game 21640).

## 0.2.1

Correctness fixes from an adversarial review of the v0.2 additions (the
simulator's uniform test names had masked real‑match bugs):

- **Critical — player name matching.** GEP `me.player_name` carries the Riot
  tagline (`Doom#5339`), kill_feed names are bare (`YTDestruct28`), and
  scoreboard names are spaced (`MrTest #1111`). v0.2 compared them directly, so
  in a **real match** no local kills, first blood, multikills or aces would ever
  fire. All player comparisons now go through `baseName()` (drop `#tag`, collapse
  spaces, lowercase); scoreboard `is_local` stays the primary signal.
- **Kill reconciliation** — the GEP counter is now the authoritative kill count
  and kill_feed supplies detail; exactly `kills` events are emitted regardless of
  which arrives first (no double‑count, none lost if the feed degrades).
- **Clutch** — disarmed when the local player dies (no false clutch on a
  post‑mortem round win); armed from kill‑feed deaths too, not only scoreboard
  updates; tracks the **peak** enemy count while alone (a 1v3 traded to 1v1 is
  still a 1v3); stale/disconnected scoreboard rows excluded from the tally.
- **getInfo() priming** no longer replays already‑past round outcomes when the
  companion starts mid‑match (adopts score/round/map/agent silently).
- **Recording ownership** — the plugin only auto‑stops a recording it started:
  ownership is claimed on `RECORDING_STARTED` (not optimistically) and cleared on
  any `RECORDING_STOPPED`, so toggling auto‑record off or manually stopping/
  restarting can never make GamePulse stop a user‑owned recording.
- Added `companion/test/normalizer.test.js` (`npm test`) locking the above, and
  hardened the simulator to use tagline/spaced/bare name variants.

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
