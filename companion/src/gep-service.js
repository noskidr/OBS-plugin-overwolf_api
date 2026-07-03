'use strict';

/*
 * GamePulse Companion — Overwolf GEP service (Valorant-only for v1).
 *
 * Wraps the ow-electron Game Events Provider package. On the 'gep' package
 * becoming ready it listens for Valorant (game id 21640) being detected,
 * enables GEP for it, requests all features, and emits normalized events.
 *
 * Verified against @overwolf/ow-electron typings (ow-electron.d.ts) and the
 * official ow-electron-packages-sample:
 *   app.overwolf.packages.on('ready', (e, name, version) => ...)
 *   gep.on('game-detected', (e, gameId, name, gameInfo) => e.enable())
 *   gep.setRequiredFeatures(gameId, null)   // null = all features
 *   gep.on('new-game-event', (e, gameId, data) => ...)   // {feature,key,value}
 *   gep.on('new-info-update', (e, gameId, data) => ...)  // {feature,category,key,value}
 */

const EventEmitter = require('events');
const https = require('https');

const VALORANT_ID = 21640;

/* Overwolf publishes per-game GEP health: state 1=green, 2=partial, 3=down. */
function checkGepHealth(gameId, cb) {
  https
    .get(`https://game-events-status.overwolf.com/${gameId}_prod.json`, (res) => {
      let body = '';
      res.on('data', (c) => (body += c));
      res.on('end', () => {
        try {
          cb(null, JSON.parse(body));
        } catch (err) {
          cb(err);
        }
      });
    })
    .on('error', (err) => cb(err));
}

class GepService extends EventEmitter {
  constructor(app) {
    super();
    this.app = app; // electron app cast to overwolf app
    this.gep = null;
    this.activeGame = 0;
    this.trackedIds = [VALORANT_ID];
  }

  start() {
    // Surface upstream event health regardless of runtime (harmless HTTPS GET).
    checkGepHealth(VALORANT_ID, (err, health) => {
      if (err) {
        this.emit('log', 'debug', `GEP health check failed: ${err.message || err}`);
        return;
      }
      const states = { 1: 'green (all good)', 2: 'yellow (some events degraded)', 3: 'red (events down)' };
      const label = states[health.state] || `state ${health.state}`;
      const level = health.state >= 3 || health.disabled_electron ? 'error' : health.state === 2 ? 'warn' : 'info';
      this.emit('log', level, `Overwolf Valorant event health: ${label}` +
        (health.disabled_electron ? ' — DISABLED for ow-electron right now' : ''));
      this.emit('health', health);
    });

    if (!this.app.overwolf || !this.app.overwolf.packages) {
      this.emit('log', 'error', 'ow-electron packages API not present — are you running under ow-electron? (use `npm start`, not `npm run start:electron`)');
      return;
    }

    this.app.overwolf.packages.on('ready', (_e, packageName, version) => {
      if (packageName !== 'gep') return;
      this.emit('log', 'info', `GEP package ready (v${version})`);
      this._wireGep();
    });

    this.app.overwolf.packages.on('failed-to-initialize', (_e, packageName) => {
      this.emit('log', 'error', `package failed to initialize: ${packageName}`);
    });

    this.app.overwolf.packages.on('crashed', (_e, canRecover) => {
      this.emit('log', 'error', `GEP package crashed (recoverable: ${canRecover})`);
    });
  }

  async _wireGep() {
    this.gep = this.app.overwolf.packages.gep;
    this.gep.removeAllListeners();

    this.gep.on('game-detected', (e, gameId, name, gameInfo) => {
      if (!this.trackedIds.includes(gameId)) {
        this.emit('log', 'debug', `ignoring non-tracked game ${name} (${gameId})`);
        return; // NOT calling enable() means GEP won't attach
      }
      this.emit('log', 'info', `Valorant detected (pid ${gameInfo && gameInfo.pid}) — enabling GEP`);
      e.enable();
      this.activeGame = gameId;
      this.emit('game', 'detected', { id: gameId, name: name || 'VALORANT' });
      // features must be set after enable; retry a few times as they can
      // register late relative to game launch.
      this._setFeaturesWithRetry(gameId, 5);
    });

    this.gep.on('elevated-privileges-required', (_e, gameId) => {
      this.emit('log', 'warn', `Valorant is running elevated — run GamePulse Companion as administrator to receive events (game ${gameId})`);
      this.emit('elevated-required', gameId);
    });

    this.gep.on('new-game-event', (_e, gameId, data) => {
      this._onGameEvent(gameId, data);
    });

    this.gep.on('new-info-update', (_e, gameId, data) => {
      this._onInfoUpdate(gameId, data);
    });

    this.gep.on('game-exit', (_e, gameId, gameName) => {
      this.emit('log', 'info', `Valorant exited (${gameName || gameId})`);
      if (gameId === this.activeGame) this.activeGame = 0;
      this.emit('game', 'closed', { id: gameId, name: gameName || 'VALORANT' });
    });

    this.gep.on('error', (_e, gameId, error) => {
      this.emit('log', 'error', `GEP error (game ${gameId}): ${error}`);
      if (gameId === this.activeGame) this.activeGame = 0;
    });

    // If Valorant is already running when we connect, getInfo primes state.
    try {
      const games = await this.gep.getSupportedGames();
      const val = (games || []).find((g) => g.id === VALORANT_ID);
      this.emit('log', 'info', `GEP supports Valorant: ${val ? 'yes' : 'not in this build'}`);
    } catch (err) {
      this.emit('log', 'debug', `getSupportedGames failed: ${err}`);
    }
  }

  async _setFeaturesWithRetry(gameId, attempts) {
    for (let i = 0; i < attempts; i++) {
      try {
        // null = subscribe to ALL features (matches the official sample)
        await this.gep.setRequiredFeatures(gameId, null);
        this.emit('log', 'info', 'GEP required features set (all)');
        this._primeFromInfo(gameId);
        return;
      } catch (err) {
        this.emit('log', 'debug', `setRequiredFeatures attempt ${i + 1} failed: ${err}`);
        await new Promise((r) => setTimeout(r, 3000));
      }
    }
    this.emit('log', 'warn', 'could not set GEP features after retries — events may not flow');
  }

  /* If the companion starts mid-match, getInfo() returns the current info
     dictionary — replay it through the normal info path so player name /
     agent / map / score / round are known immediately. */
  async _primeFromInfo(gameId) {
    try {
      const snapshot = await this.gep.getInfo(gameId);
      if (!snapshot || typeof snapshot !== 'object') return;
      const root = snapshot.res && typeof snapshot.res === 'object' ? snapshot.res : snapshot;
      let fed = 0;
      for (const [feature, group] of Object.entries(root)) {
        if (!group || typeof group !== 'object' || Array.isArray(group)) continue;
        for (const [key, value] of Object.entries(group)) {
          this.emit('gep-info', { gameId, feature, category: feature, key, value: this._decode(value) });
          fed++;
        }
      }
      if (fed) this.emit('log', 'info', `primed ${fed} info values from getInfo()`);
    } catch (err) {
      this.emit('log', 'debug', `getInfo prime failed: ${err}`);
    }
  }

  _onGameEvent(gameId, data) {
    // data: { feature, key, value } (value may be number|string|json-string)
    if (!data) return;
    const key = data.key || data.name;
    const value = this._decode(data.value);
    this.emit('log', 'debug', `event ${key} = ${JSON.stringify(value)}`);
    this.emit('gep-event', { gameId, feature: data.feature, key, value });
  }

  _onInfoUpdate(gameId, data) {
    // data: { feature, category, key, value }
    if (!data) return;
    const value = this._decode(data.value);
    this.emit('gep-info', {
      gameId,
      feature: data.feature,
      category: data.category,
      key: data.key,
      value,
    });
  }

  // GEP values arrive as number | plain string | JSON-encoded string |
  // URI-encoded JSON string | "". Decode opportunistically.
  _decode(value) {
    if (typeof value !== 'string') return value;
    let s = value;
    if (s.length === 0) return s;
    if (s.indexOf('%7B') === 0 || s.indexOf('%22') === 0) {
      try {
        s = decodeURIComponent(s);
      } catch (_) {
        /* leave as-is */
      }
    }
    const t = s.trim();
    if ((t[0] === '{' && t[t.length - 1] === '}') || (t[0] === '[' && t[t.length - 1] === ']')) {
      try {
        return JSON.parse(t);
      } catch (_) {
        return s;
      }
    }
    return s;
  }
}

module.exports = { GepService, VALORANT_ID };
