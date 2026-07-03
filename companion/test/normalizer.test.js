'use strict';
const { ValorantNormalizer } = require('../src/valorant-normalizer.js');

let failures = 0;
const assert = (cond, msg) => { if (!cond) { console.log('FAIL:', msg); failures++; } else console.log('ok:', msg); };

function feed(n, evt, key, value) {
  const out = [];
  const list = evt ? n.handleEvent({ key, value }, 0) : n.handleInfo({ feature: value && value.__feature || 'match_info', key, value }, 0);
  return list;
}
function ev(n, key, value) { return n.handleEvent({ key, value }, 0); }
function info(n, feature, key, value) { return n.handleInfo({ feature, category: feature, key, value }, 0); }

// ---- Test 1: kill count reconciliation, both orderings, no double/loss ----
function killCount(order) {
  const n = new ValorantNormalizer();
  info(n, 'me', 'player_name', 'Doom#5339');
  ev(n, 'match_start', '');
  info(n, 'match_info', 'round_phase', 'combat');
  let emitted = 0;
  const feedLine = (victim) => ({ attacker: 'Doom', victim, is_attacker_teammate: true, weapon: 'TX_Hud_AR_Vandal', headshot: true });
  for (let i = 1; i <= 3; i++) {
    let batch = [];
    if (order === 'feed-first') {
      batch = batch.concat(ev(n, 'kill_feed', feedLine('V' + i)));
      batch = batch.concat(ev(n, 'kill', i));
    } else {
      batch = batch.concat(ev(n, 'kill', i));
      batch = batch.concat(ev(n, 'kill_feed', feedLine('V' + i)));
    }
    emitted += batch.filter((e) => e.t === 'event' && e.name === 'kill').length;
  }
  return emitted;
}
assert(killCount('feed-first') === 3, `feed-first emits exactly 3 kills (got ${killCount('feed-first')})`);
assert(killCount('counter-first') === 3, `counter-first emits exactly 3 kills (got ${killCount('counter-first')})`);

// ---- Test 2: degraded feed (counter only, no feed) still yields kills ----
(() => {
  const n = new ValorantNormalizer();
  info(n, 'me', 'player_name', 'Doom#5339');
  ev(n, 'match_start', '');
  info(n, 'match_info', 'round_phase', 'combat');
  let k = 0;
  for (let i = 1; i <= 3; i++) k += ev(n, 'kill', i).filter((e) => e.name === 'kill').length;
  assert(k === 3, `counter-only (no feed) still emits 3 kills (got ${k})`);
})();

// ---- Test 3: die-then-win must NOT emit a clutch ----
(() => {
  const n = new ValorantNormalizer();
  info(n, 'me', 'player_name', 'Doom#5339');
  ev(n, 'match_start', '');
  info(n, 'match_info', 'round_number', '5');
  info(n, 'match_info', 'round_phase', 'combat');
  // roster: me + 4 enemies alive, 0 teammates -> 1v4 armed
  const sb = (i, name, teammate, alive) => info(n, 'match_info', 'scoreboard_' + i, { name, teammate, alive });
  sb(0, 'Doom #5339', true, true);
  sb(1, 'E1 #1', false, true);
  sb(2, 'E2 #2', false, true);
  sb(3, 'E3 #3', false, true);
  sb(4, 'E4 #4', false, true);
  // now the local player dies (spike planted, they trade out)
  const deathOut = ev(n, 'death', 1);
  // defenders fail, spike detonates, round won
  const scoreOut = info(n, 'match_info', 'score', { won: 1, lost: 0 });
  const clutch = scoreOut.filter((e) => e.t === 'event' && /clutch/.test(e.name));
  assert(clutch.length === 0, `die-then-win emits NO clutch (got ${clutch.length})`);
  const won = scoreOut.filter((e) => e.name === 'round_won');
  assert(won.length === 1, 'die-then-win still emits round_won');
})();

// ---- Test 4: real 1v3 clutch (alive at win) DOES emit ----
(() => {
  const n = new ValorantNormalizer();
  info(n, 'me', 'player_name', 'Doom#5339');
  ev(n, 'match_start', '');
  info(n, 'match_info', 'round_number', '6');
  info(n, 'match_info', 'round_phase', 'combat');
  const sb = (i, name, teammate, alive) => info(n, 'match_info', 'scoreboard_' + i, { name, teammate, alive });
  // me alive, teammates all dead, 3 enemies alive
  sb(0, 'Doom #5339', true, true);
  sb(1, 'T1 #1', true, false);
  sb(2, 'T2 #2', true, false);
  sb(3, 'E1 #1', false, true);
  sb(4, 'E2 #2', false, true);
  sb(5, 'E3 #3', false, true);
  assert(n.clutchArmed && n.clutchVs === 3, `1v3 armed at peak (armed=${n.clutchArmed} vs=${n.clutchVs})`);
  const scoreOut = info(n, 'match_info', 'score', { won: 1, lost: 0 });
  const clutch = scoreOut.filter((e) => /clutch_1v3/.test(e.name));
  assert(clutch.length === 1, 'alive-at-win emits clutch_1v3');
})();

// ---- Test 5: priming (getInfo snapshot) emits NO round outcomes ----
(() => {
  const n = new ValorantNormalizer();
  const out = n.primeFromSnapshot({ me: { player_name: 'Doom#5339', agent: 'Wushu_PC_C' },
    match_info: { score: { won: 8, lost: 4 }, round_number: '13', round_phase: 'combat' },
    game_info: { scene: 'Ascent' } }, 0);
  const derived = out.filter((e) => e.t === 'event');
  assert(derived.length === 0, `priming emits no events (got ${derived.map((e) => e.name)})`);
  assert(n.scoreWon === 8 && n.scoreLost === 4, 'priming adopts score 8-4');
  assert(n.map === 'Ascent' && n.agent === 'Jett', 'priming adopts map + agent');
})();

console.log(failures === 0 ? '\nALL EDGE TESTS PASSED' : `\n${failures} FAILURE(S)`);
process.exit(failures === 0 ? 0 : 1);
