# GamePulse WebSocket protocol

The OBS plugin runs a WebSocket **server** on `ws://127.0.0.1:<port>` (default
`4477`). Any client — the bundled Overwolf companion, the simulator, or your own
script — connects and streams game events as JSON text frames (one JSON object
per frame). This document is the whole contract, so you can build a producer for
any game or source.

## Handshake & auth

Standard RFC 6455. If a **token** is set in the dock, present it either as
`ws://127.0.0.1:4477/?token=<token>` in the connect path **or** as an
`Authorization: Bearer <token>` header. Without a valid token the server replies
`401` and closes.

On connect the server sends:

```json
{"t":"welcome","plugin":"obs-gamepulse","version":"0.1.0","obs":"32.1.2"}
{"t":"status","streaming":false,"recording":false,"replay":false,"stream_ms":-1,"record_ms":-1}
```

The client should send a `hello` first:

```json
{"t":"hello","client":"my-producer","version":"1.0.0","token":"<token-if-any>"}
```

## Client → plugin messages

Every message is a JSON object with a `t` (type) field.

### `event` — a discrete game event

```json
{
  "t": "event",
  "game": { "id": 21640, "name": "VALORANT" },
  "name": "kill",
  "label": "Kill",              // optional; taxonomy fills a default
  "detail": "Vandal → Reyna (HS)", // optional human detail
  "importance": 2,               // optional 0..3 (debug/minor/notable/epic)
  "ts": 1730000000000,           // optional epoch ms; server stamps if absent
  "data": { }                    // optional raw passthrough
}
```

`name` is the machine key. Known keys get a label + importance from the plugin's
taxonomy (`gp-taxonomy.cpp`); unknown keys degrade to a prettified label at
minor importance, so **any** event name works. The rules engine derives
`multikill_N` and `ace` from `kill` events itself — don't send those.

### `info` — a state update (used to synthesize round boundaries)

```json
{ "t":"info", "game":{"id":21640,"name":"VALORANT"},
  "feature":"match_info", "category":"match_info",
  "key":"round_phase", "value":"combat", "ts":1730000000000 }
```

The plugin tracks info state and synthesizes `round_start` / `round_end` events
from `round_phase` transitions (`shopping` → `round_start`, `end`/`game_end` →
`round_end`), because Valorant GEP has no native round events. Other info keys
are stored for consumers but don't fire actions.

### `info` with `feature:"context"` — display context (v0.2)

The companion emits normalized, display‑ready context the plugin shows on the
overlay/dock and uses for scene automation:

```json
{ "t":"info", "game":{"id":21640,"name":"VALORANT"},
  "feature":"context", "key":"map",   "value":"Haven" }
{ "t":"info", "feature":"context", "key":"agent", "value":"Jett" }
{ "t":"info", "feature":"context", "key":"score", "value":"8-4" }
{ "t":"info", "feature":"context", "key":"round", "value":"13" }
{ "t":"info", "feature":"context", "key":"scene", "value":"agent_select" }
```

`scene` values (`agent_select` | `map` | `menu` | `other`) drive optional OBS
scene switching. `map`/`agent`/`score`/`round` populate the overlay stats bar
and the dock's context line.

### Events the companion derives (Valorant)

Beyond the raw GEP keys, the companion emits: `first_blood`, `clutch_1v1`..
`clutch_1v5` (local player alone → round won), `round_won`/`round_lost`, and a
`match_end` whose `detail` is the match summary. `multikill_N` and `ace` are
derived by the **plugin** from the `kill` stream (single source of truth).

### `game` — game lifecycle

```json
{ "t":"game", "state":"detected", "game":{"id":21640,"name":"VALORANT"} }
```

`state` ∈ `detected` | `running` | `closed`. Sets the active game shown in the
dock/overlay and resets derive state on change.

### `batch` — many items at once

```json
{ "t":"batch", "items":[ {event…}, {info…} ] }
```

Each item is an `event` or `info` object (with its own `t`). Useful for bursts.

### `ping`

```json
{ "t":"ping" }
```

Server replies `{"t":"pong"}`. (WebSocket‑level ping/pong frames also work.)

## Plugin → client messages

- `{"t":"welcome",…}` — on connect (see above).
- `{"t":"status","streaming":b,"recording":b,"replay":b,"stream_ms":n,"record_ms":n}` —
  on connect and whenever streaming/recording/replay state changes. `*_ms` is
  elapsed milliseconds since that output started (pause‑adjusted for recording),
  or `-1` when inactive.
- `{"t":"pong"}` — reply to `ping`.
- `{"t":"error","message":"…"}` — a message couldn't be parsed/handled.

## Value decoding note

Overwolf GEP delivers many values as JSON‑encoded strings (and Rocket League as
URL‑encoded JSON). The companion decodes these before forwarding, so `data`/
`value` you send should already be real JSON types. The plugin also accepts the
game `id` as a number or a string.

## Minimal producer example (Node)

```js
const net = require('net'); // or use `ws`
// ...connect, do the RFC6455 handshake, then send text frames:
send({ t: 'hello', client: 'demo', version: '1.0.0' });
send({ t: 'game', state: 'detected', game: { id: 21640, name: 'VALORANT' } });
send({ t: 'event', game: { id: 21640, name: 'VALORANT' }, name: 'kill' });
```

See `companion/src/ws-forwarder.js` for a complete dependency‑free client and
`companion/test/harness.js` for a full producer that replays a mock match.
