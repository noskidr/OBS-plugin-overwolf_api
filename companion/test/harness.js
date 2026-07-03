'use strict';

/*
 * Headless pipeline test harness (plain Node, no electron/overwolf).
 *
 * Wires Simulator -> ValorantNormalizer -> WsForwarder against a running
 * GamePulse OBS plugin (or the mock server in mock-server.js). Verifies the
 * exact code path the shipping companion uses.
 *
 *   node test/harness.js [--port 4477] [--token X] [--rounds 3]
 */

const path = require('path');
const { Simulator } = require('../src/simulator');
const { ValorantNormalizer } = require('../src/valorant-normalizer');
const { WsForwarder } = require('../src/ws-forwarder');

function arg(name, def) {
  const i = process.argv.indexOf('--' + name);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : def;
}

const port = parseInt(arg('port', '4477'), 10);
const token = arg('token', '');
// one scripted match takes ~45s; default runs a single full match
const seconds = parseInt(arg('seconds', arg('rounds', null) ? String(parseInt(arg('rounds', '1'), 10) * 50) : '50'), 10);

const normalizer = new ValorantNormalizer();
const forwarder = new WsForwarder();
const sim = new Simulator();

let sent = 0;

forwarder.on('log', (lvl, msg) => console.log(`[fwd:${lvl}] ${msg}`));
forwarder.on('status', (up) => console.log(`[fwd] connection ${up ? 'UP' : 'down'}`));
forwarder.on('plugin-message', (text) => console.log(`[plugin] ${text}`));

sim.on('log', (lvl, msg) => console.log(`[sim:${lvl}] ${msg}`));
sim.on('game', (state, game) => {
  forwarder.send({ t: 'game', state, game });
  if (state === 'closed' || state === 'detected') normalizer.reset();
  console.log(`[sim] game ${state}`);
});
sim.on('gep-event', (evt) => {
  const ts = Date.now();
  for (const p of normalizer.handleEvent(evt, ts)) {
    forwarder.send(p);
    sent++;
    console.log(`  -> ${p.name} "${p.label}"${p.detail ? ' (' + p.detail + ')' : ''}`);
  }
});
sim.on('gep-info', (info) => {
  const ts = Date.now();
  for (const p of normalizer.handleInfo(info, ts)) {
    forwarder.send(p);
    console.log(`  -> info ${p.key}=${p.value}`);
  }
});

forwarder.configure(port, token);
forwarder.start();

console.log(`Harness: forwarding a mock Valorant match to ws://127.0.0.1:${port}${token ? ' (token set)' : ''}`);

// Give the socket a moment to connect, then start the sim.
setTimeout(() => sim.start(), 800);

// Run for the configured duration (default: one full scripted match) then exit.
setTimeout(() => {
  sim.stop();
  setTimeout(() => {
    console.log(`\nHarness done. Sent ${sent} protocol events.`);
    forwarder.stop();
    process.exit(0);
  }, 500);
}, seconds * 1000);
