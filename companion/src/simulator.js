'use strict';

/*
 * GamePulse Companion — Valorant match simulator.
 *
 * Emits a scripted, GEP-shaped Valorant match through the SAME normalizer path
 * as real GEP, so the whole pipeline (companion → WS → OBS plugin → chapters/
 * clips/overlay/exports) is testable without launching Valorant.
 *
 * The scripted match exercises every derivation the normalizer does:
 *   agent select scene → map load (Ascent)     context + scene automation
 *   round 1: our 5-kill burst                  first_blood, multikill→ACE
 *   round 2: teammates wiped, 1v3 clutch won   clutch_1v3
 *   score updates, match_end                   round_won/lost, match summary
 *
 * NOTE: values are emitted DECODED (objects, numbers) because GepService
 * decodes real GEP's JSON-string values before the normalizer sees them.
 */

const EventEmitter = require('events');

const TEAMMATES = ['TeamAlpha', 'TeamBravo', 'TeamCharlie', 'TeamDelta'];
const ENEMIES = ['EnemyOne', 'EnemyTwo', 'EnemyThree', 'EnemyFour', 'EnemyFive'];

class Simulator extends EventEmitter {
  constructor() {
    super();
    this.timer = null;
    this._running = false;
    this._kills = 0;
    this._deaths = 0;
    this._assists = 0;
    this._headshots = 0;
    this._won = 0;
    this._lost = 0;
  }

  get running() {
    return this._running;
  }

  start() {
    if (this._running) return;
    this._running = true;
    this._kills = this._deaths = this._assists = this._headshots = 0;
    this._won = this._lost = 0;

    this.emit('game', 'detected', { id: 21640, name: 'VALORANT' });
    this.emit('log', 'info', 'simulator started — feeding a mock Valorant match');

    const seq = [];
    let t = 0;
    const at = (delay, fn) => {
      t += delay;
      seq.push([t, fn]);
    };

    // ---- pre-match: identity, agent select, map load ----
    at(200, () => this._info('me', 'player_name', 'You'));
    at(200, () => this._info('me', 'agent', 'Wushu_PC_C')); // Jett
    at(300, () => this._info('game_info', 'scene', 'CharacterSelectPersistentLevel'));
    at(2000, () => this._info('game_info', 'scene', 'Ascent'));
    at(400, () => this._event('match_info', 'match_start', ''));

    // ---- round 1: hot start, our ace ----
    at(600, () => this._round(1));
    at(300, () => this._scoreboardAll(true));
    at(2000, () => this._info('match_info', 'round_phase', 'combat'));
    at(800, () => this._ourKill('Vandal', 'EnemyOne', true)); // first blood
    at(1100, () => this._ourKill('Sheriff', 'EnemyTwo', true));
    at(1100, () => this._ourKill('Operator', 'EnemyThree', false));
    at(1100, () => this._ourKill('Vandal', 'EnemyFour', true));
    at(1100, () => this._ourKill('Vandal', 'EnemyFive', true)); // 5th → ACE
    at(700, () => this._event('match_info', 'spike_defused', ''));
    at(600, () => this._score(true));
    at(400, () => this._info('match_info', 'round_phase', 'end'));

    // ---- round 2: trades leave a true 1v3, we clutch it ----
    at(2500, () => this._round(2));
    at(300, () => this._scoreboardAll(true));
    at(2000, () => this._info('match_info', 'round_phase', 'combat'));
    at(900, () => this._enemyKill('EnemyOne', 'TeamAlpha'));
    at(700, () => this._ourKill('Vandal', 'EnemyOne', false));
    at(700, () => this._enemyKill('EnemyTwo', 'TeamBravo'));
    at(700, () => this._ourKill('Sheriff', 'EnemyTwo', true));
    at(700, () => this._enemyKill('EnemyThree', 'TeamCharlie'));
    at(700, () => this._enemyKill('EnemyFour', 'TeamDelta'));
    // scoreboard now: us alive, 0 teammates, Enemy Three/Four/Five alive -> 1v3 armed
    at(400, () => this._scoreboardSync());
    at(900, () => this._ourKill('Vandal', 'EnemyThree', true));
    at(1100, () => this._ourKill('Vandal', 'EnemyFour', false));
    at(1100, () => this._ourKill('Operator', 'EnemyFive', true)); // 5th kill of the round → ACE too
    at(700, () => this._score(true)); // round won while clutch armed -> clutch_1v3
    at(400, () => this._info('match_info', 'round_phase', 'end'));

    // ---- round 3: we lose one, then match ends ----
    at(2500, () => this._round(3));
    at(300, () => this._scoreboardAll(true));
    at(1500, () => this._info('match_info', 'round_phase', 'combat'));
    at(800, () => this._enemyKill('EnemyFive', 'You'));
    at(300, () => this._event('death', 'death', ++this._deaths));
    at(900, () => this._score(false));
    at(400, () => this._info('match_info', 'round_phase', 'end'));
    at(1200, () => this._event('match_info', 'match_end', ''));

    this._alive = {};
    this._runSequence(seq, () => {
      this.emit('log', 'info', 'simulator match finished');
      if (this._running) {
        // brief intermission, then play another match
        this.timer = setTimeout(() => {
          this._running = false;
          this.start();
        }, 5000);
      }
    });
  }

  stop() {
    if (!this._running && !this.timer) return;
    this._running = false;
    if (this.timer) {
      clearTimeout(this.timer);
      this.timer = null;
    }
    this.emit('game', 'closed', { id: 21640, name: 'VALORANT' });
    this.emit('log', 'info', 'simulator stopped');
  }

  _runSequence(seq, done) {
    let i = 0;
    let prev = 0;
    const step = () => {
      if (!this._running || i >= seq.length) {
        if (this._running) done();
        return;
      }
      const [when, fn] = seq[i++];
      const delay = Math.max(0, when - prev);
      prev = when;
      this.timer = setTimeout(() => {
        fn();
        step();
      }, delay);
    };
    step();
  }

  /* ---- emit helpers (GEP-decoded shapes) ---- */

  _event(feature, key, value) {
    this.emit('gep-event', { gameId: 21640, feature, key, value });
  }

  _info(feature, key, value) {
    this.emit('gep-info', { gameId: 21640, feature, category: feature, key, value });
  }

  _round(n) {
    this._info('match_info', 'round_number', String(n));
    this._info('match_info', 'round_phase', 'shopping');
  }

  _score(won) {
    if (won) this._won++;
    else this._lost++;
    this._info('match_info', 'score', { won: this._won, lost: this._lost });
  }

  _scoreboardAll(alive) {
    this._alive = { You: alive };
    for (const n of TEAMMATES) this._alive[n] = alive;
    for (const n of ENEMIES) this._alive[n] = alive;
    this._scoreboardSync();
  }

  _scoreboardSync() {
    const all = [
      { name: 'You', teammate: true, is_local: true },
      ...TEAMMATES.map((n) => ({ name: n, teammate: true, is_local: false })),
      ...ENEMIES.map((n) => ({ name: n, teammate: false, is_local: false })),
    ];
    all.forEach((p, i) => {
      this._info('match_info', `scoreboard_${i}`, {
        name: p.name,
        teammate: p.teammate,
        is_local: p.is_local,
        alive: this._alive[p.name] !== false,
        kills: p.is_local ? this._kills : 0,
        deaths: 0,
        assists: 0,
      });
    });
  }

  _ourKill(weapon, victim, headshot) {
    const weaponTex = {
      Vandal: 'TX_Hud_AR_Vandal',
      Operator: 'TX_Hud_SR_Sniper',
      Sheriff: 'TX_Hud_Pistol_Luger',
    };
    this._alive[victim] = false;
    this._event('match_info', 'kill_feed', {
      attacker: 'You',
      victim,
      is_attacker_teammate: true,
      is_victim_teammate: false,
      weapon: weaponTex[weapon] || 'TX_Hud_AR_Standard',
      headshot: !!headshot,
    });
    this._kills++;
    if (headshot) this._headshots++;
    this._event('kill', 'kill', this._kills);
    if (headshot) this._event('kill', 'headshot', this._headshots);
    this._scoreboardSync();
  }

  _enemyKill(attacker, victim) {
    this._alive[victim] = false;
    this._event('match_info', 'kill_feed', {
      attacker,
      victim,
      is_attacker_teammate: false,
      is_victim_teammate: victim !== 'You' || undefined,
      weapon: 'TX_Hud_AR_Standard',
      headshot: false,
    });
    this._scoreboardSync();
  }
}

module.exports = { Simulator };
