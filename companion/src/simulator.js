'use strict';

/*
 * GamePulse Companion — Valorant match simulator.
 *
 * Emits a plausible stream of GEP-shaped Valorant events so the whole pipeline
 * (companion → WS → OBS plugin → chapters/clips/overlay/exports) can be tested
 * without launching Valorant. Enable with `--gp-simulate` or from the tray.
 *
 * It feeds the SAME normalizer path as real GEP by emitting raw {key,value}
 * events, so what you test is what ships.
 */

const EventEmitter = require('events');

class Simulator extends EventEmitter {
  constructor() {
    super();
    this.timer = null;
    this.round = 0;
    this.step = 0;
  }

  get running() {
    return this.timer !== null;
  }

  start() {
    if (this.timer) return;
    this.round = 0;
    this.emit('game', 'detected', { id: 21640, name: 'VALORANT' });
    this.emit('gep-event', { gameId: 21640, feature: 'match_info', key: 'match_start', value: '' });
    this.emit('log', 'info', 'simulator started — feeding a mock Valorant match');
    // Drive rounds on a self-scheduling chain so kills can burst close together
    // (tight enough to derive Triple/Quadra/Penta), with a pause between rounds.
    this._running = true;
    this._playRound();
  }

  stop() {
    this._running = false;
    if (this.timer) {
      clearTimeout(this.timer);
      this.timer = null;
    }
    this.emit('gep-event', { gameId: 21640, feature: 'match_info', key: 'match_end', value: '' });
    this.emit('game', 'closed', { id: 21640, name: 'VALORANT' });
    this.emit('log', 'info', 'simulator stopped');
  }

  _playRound() {
    if (!this._running) return;
    this.round++;

    // Build a timeline of [delayMs, action] for one round. Combat kills are
    // ~1.2s apart so the 5th lands inside the multikill window -> Penta + Ace.
    const kills = [
      ['Vandal', 'PhoenixDown', true],
      ['Sheriff', 'SovaMain', true],
      ['Operator', 'JettAndy', false],
      ['Vandal', 'SageWall', true],
      ['Vandal', 'OmenSmoke', true],
    ];
    const seq = [];
    seq.push([0, () => this._info('round_phase', 'shopping')]);
    seq.push([200, () => this._info('round_number', String(this.round))]);
    seq.push([2500, () => this._info('round_phase', 'combat')]);
    let t = 3200;
    kills.forEach(([w, v, hs], i) => {
      seq.push([t, () => this._killFeed(w, v, hs)]);
      seq.push([t + 60, () => this._event('kill', i + 1)]);
      t += 1200;
    });
    seq.push([t + 400, () => this._event('spike_defused', '')]);
    seq.push([t + 1400, () => this._info('round_phase', 'end')]);
    seq.push([t + 2200, () => this._event('death', 1)]); // breaks the chain before next round

    let i = 0;
    const run = () => {
      if (!this._running || i >= seq.length) {
        // schedule next round after a short intermission
        if (this._running) this.timer = setTimeout(() => this._playRound(), 3000);
        return;
      }
      const [delay, fn] = seq[i++];
      const prevDelay = i >= 2 ? seq[i - 2][0] : 0;
      this.timer = setTimeout(() => {
        fn();
        run();
      }, Math.max(0, delay - prevDelay));
    };
    run();
  }

  _event(key, value) {
    this.emit('gep-event', { gameId: 21640, feature: key === 'death' ? 'death' : 'match_info', key, value });
  }

  _info(key, value) {
    this.emit('gep-info', { gameId: 21640, feature: 'match_info', category: 'match_info', key, value });
  }

  _killFeed(weapon, victim, headshot) {
    const value = JSON.stringify({
      attacker: 'You',
      victim,
      is_attacker_teammate: true,
      is_victim_teammate: false,
      weapon: 'TX_Hud_AR_' + (weapon === 'Vandal' ? 'Vandal' : weapon === 'Operator' ? 'Sniper' : 'Standard'),
      headshot: !!headshot,
    });
    this.emit('gep-event', { gameId: 21640, feature: 'match_info', key: 'kill_feed', value });
  }
}

module.exports = { Simulator };
