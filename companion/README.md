# GamePulse Companion

An [Overwolf **ow‑electron**](https://dev.overwolf.com/ow-electron/) desktop app
that receives live **Valorant** events from Overwolf's official **Game Events
Provider (GEP)** and forwards them to the [GamePulse OBS plugin](../README.md)
over a localhost WebSocket.

It runs in the system tray, reconnects to OBS automatically, and ships a
**match simulator** so you can test the whole pipeline without launching
Valorant.

## Why ow‑electron (not a classic Overwolf app)

The companion is a normal Electron app launched with the `ow-electron` binary,
which adds Overwolf's native `gep` package. Advantages for this project:

- Standalone — users don't need to install the full Overwolf client.
- The GEP→WebSocket forwarder is a few hundred lines in the main process.
- Valorant (game id `21640`) is in Overwolf's **PROD** GEP environment.
- **No memory reading on your side** — Overwolf's signed, Riot‑approved engine
  produces the events; you only receive structured data. (This is the same
  engine insights.gg / Outplayed use.)

## Requirements

- Windows (Overwolf GEP is Windows‑only).
- Node.js 18+ and npm.
- For **real** events: Valorant, plus (first run) internet access so Overwolf
  can fetch the GEP package. For **testing**: nothing — use the simulator.

## Install & run

```bash
cd companion
npm install
npm start          # launches under ow-electron with the gep package
```

- `npm start` → runs the app (tray icon appears).
- `npm run simulate` → starts with the mock‑match simulator already running.
- `npm run start:electron` → runs under **plain** Electron (no GEP; UI/tray/
  forwarder only — handy for UI work).

Double‑click the tray icon (or *Settings…*) to open the window: set the **port**
and **token** to match the OBS GamePulse dock, then **Save & Connect**.

## Test without Valorant

Tray → **Start simulator** (or `npm run simulate`, or the window button). It
feeds a scripted Valorant match — shopping → combat → five kills → an ace →
spike defuse — so you can watch OBS drop chapters/clips, the overlay update, and
a session export get written.

You can also run the pipeline **headless** (plain Node, no Electron), which is
how CI‑style verification is done:

```bash
node test/harness.js --port 4477 --rounds 2
```

This wires `Simulator → ValorantNormalizer → WsForwarder` — the exact code path
the shipping app uses — against a running OBS plugin.

## How it works

```
Overwolf GEP  ──(new-game-event / new-info-update)──►  gep-service.js
     (game 21640)                                          │
                                                           ▼
                                            valorant-normalizer.js
                                (kill_feed → weapon/victim/headshot detail)
                                                           │  protocol events
                                                           ▼
                                               ws-forwarder.js  ──►  OBS plugin
                                            (RFC6455 client, auto-reconnect)
```

| File | Responsibility |
|---|---|
| `src/main.js` | app lifecycle, tray, settings, wires source → normalizer → forwarder |
| `src/gep-service.js` | Overwolf GEP wiring: `ready` → `game-detected`→`enable()` → `setRequiredFeatures` → events |
| `src/valorant-normalizer.js` | raw GEP kill/death/kill_feed/round_phase → GamePulse protocol events |
| `src/ws-forwarder.js` | dependency‑free RFC 6455 client to the plugin, reconnect + outbound queue |
| `src/simulator.js` | scripted mock Valorant match |
| `src/ui.html` / `preload.js` | settings window |
| `test/harness.js` | headless pipeline test |

Settings persist to `…/userData/gamepulse.json`.

## GEP notes

- The app declares `"overwolf": { "packages": ["gep"] }` in `package.json`;
  Overwolf downloads the package at runtime.
- On `game-detected` for `21640` the service calls `e.enable()` then
  `setRequiredFeatures(21640, null)` (all features), retrying since features can
  register late relative to game launch.
- If Valorant runs elevated, GEP fires `elevated-privileges-required` — run the
  companion as administrator to receive events.
- **Shipping publicly** requires an Overwolf app proposal / whitelisting and a
  code‑signing certificate (Overwolf Developer Terms). For personal/dev use the
  sample‑style flow above works; see the ow‑electron docs.

## License

GPL‑2.0‑or‑later.
