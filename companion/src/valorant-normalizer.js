'use strict';

/*
 * GamePulse Companion — Valorant event normalizer + match state machine.
 *
 * Turns raw Overwolf GEP Valorant events/info-updates into GamePulse protocol
 * events with human detail, and derives the Valorant-signature moments raw GEP
 * does not emit: first_blood, clutch_1vN, round win/loss, and a match summary.
 * Multikill/ace stay derived in the OBS plugin (single source of truth for
 * anything computable from the kill stream alone).
 *
 * NAME MATCHING (important): GEP me.player_name carries the Riot tagline
 * ("Doom#5339"); kill_feed attacker/victim are bare in-game names
 * ("YTDestruct28"); scoreboard_N.name uses a spaced form ("MrTest #1111").
 * All player comparisons therefore go through baseName() (drop "#tag",
 * collapse spaces, lowercase). is_local from the scoreboard is the primary
 * local-player signal; the name match is the fallback.
 */

const IMP = { DEBUG: 0, MINOR: 1, NOTABLE: 2, EPIC: 3 };

const WEAPON_DICT = {
  TX_Hud_Pistol_Classic: 'Classic',
  TX_Hud_Pistol_Slim: 'Shorty',
  TX_Hud_Pistol_Boom: 'Frenzy',
  TX_Hud_Pistol_AutoPistol: 'Ghost',
  TX_Hud_Pistol_Luger: 'Sheriff',
  TX_Hud_SMG_Vector: 'Stinger',
  TX_Hud_SMG_Ninja: 'Spectre',
  TX_Hud_SG_Bucky: 'Bucky',
  TX_Hud_SG_Punch: 'Judge',
  TX_Hud_AR_Burst: 'Bulldog',
  TX_Hud_AR_Ghost: 'Guardian',
  TX_Hud_AR_Standard: 'Phantom',
  TX_Hud_AR_Vandal: 'Vandal',
  TX_Hud_SR_Leveraction: 'Marshal',
  TX_Hud_SR_Bolt: 'Outlaw',
  TX_Hud_SR_Sniper: 'Operator',
  TX_Hud_LMG_Ares: 'Ares',
  TX_Hud_LMG_HMG: 'Odin',
  TX_Hud_Melee_Standard: 'Melee',
};

const AGENT_DICT = {
  Clay: 'Raze', Pandemic: 'Viper', Wraith: 'Omen', Hunter: 'Sova', Thorne: 'Sage',
  Phoenix: 'Phoenix', Wushu: 'Jett', Gumshoe: 'Cypher', Sarge: 'Brimstone', Breach: 'Breach',
  Vampire: 'Reyna', Killjoy: 'Killjoy', Guide: 'Skye', Stealth: 'Yoru', Rift: 'Astra',
  Grenadier: 'KAY/O', Deadeye: 'Chamber', Sprinter: 'Neon', BountyHunter: 'Fade', Mage: 'Harbor',
  AggroBot: 'Gekko', Cable: 'Deadlock', Sequoia: 'Iso', Smonk: 'Clove', Nox: 'Vyse',
  Cashew: 'Tejo', Terra: 'Waylay',
};

const MAP_DICT = {
  Ascent: 'Ascent', Triad: 'Haven', Duality: 'Bind', Bonsai: 'Split', Port: 'Icebox',
  Foxtrot: 'Breeze', Canyon: 'Fracture', Pitt: 'Pearl', Jam: 'Lotus', Juliett: 'Sunset',
  Infinity: 'Abyss', Rook: 'Corrode', Range: 'Practice Range', HURM_Alley: 'District',
  HURM_Yard: 'Piazza', HURM_Bowl: 'Kasbah', HURM_Helix: 'Drift', HURM_HighTide: 'Glitch',
};

function prettyWeapon(tex) {
  if (!tex) return '';
  if (WEAPON_DICT[tex]) return WEAPON_DICT[tex];
  return String(tex).replace(/^TX_(Hud|Killfeed)_/i, '').replace(/_/g, ' ').trim();
}

function prettyAgent(codename) {
  if (!codename) return '';
  const base = String(codename).replace(/_PC_C$/i, '');
  return AGENT_DICT[base] || base;
}

function prettyMap(scene) {
  if (!scene) return '';
  return MAP_DICT[scene] || '';
}

/* Canonical player key: drop the "#TAG"/" #TAG" tagline, collapse internal
   whitespace, lowercase. "Doom#5339" / "Doom #5339" / "doom" all collapse to
   "doom" so names from me/kill_feed/scoreboard compare equal. */
function baseName(s) {
  if (!s) return '';
  return String(s).replace(/\s*#.*$/, '').replace(/\s+/g, ' ').trim().toLowerCase();
}

class ValorantNormalizer {
  constructor() {
    this.reset();
  }

  reset() {
    this.localPlayer = ''; // canonical (baseName) key of the local player
    this.agent = '';
    this.map = '';
    this.lastRoundPhase = '';
    this.roundNumber = 0;
    this.scoreWon = 0;
    this.scoreLost = 0;
    this.kills = 0;
    this.deaths = 0;
    this.assists = 0;
    this.headshots = 0;
    // per-round state
    this.roundFirstKillSeen = false;
    this.clutchArmed = false;
    this.clutchVs = 0;
    this.localDeadThisRound = false;
    // kill reconciliation: the GEP counter is the authoritative count of the
    // local player's kills; kill_feed supplies detail. We emit exactly
    // this.kills kill events, pulling queued detail FIFO so ordering between
    // the counter and the feed never double-counts or drops a kill.
    this.emittedKills = 0;
    this.pendingKillDetail = []; // [{detail, victim}] from local kill_feed lines
    // scoreboard key(baseName) -> { teammate, alive, isLocal, lastRound }
    this.scoreboard = new Map();
    this.priming = false; // true while replaying a getInfo() snapshot
  }

  _resetMatch() {
    const lp = this.localPlayer, agent = this.agent, map = this.map;
    this.reset();
    this.localPlayer = lp;
    this.agent = agent;
    this.map = map;
  }

  _resetRound() {
    this.roundFirstKillSeen = false;
    this.clutchArmed = false;
    this.clutchVs = 0;
    this.localDeadThisRound = false;
    this.pendingKillDetail = []; // detail doesn't carry across rounds
    for (const s of this.scoreboard.values()) s.alive = true;
  }

  contextDetail() {
    const bits = [];
    if (this.scoreWon || this.scoreLost) bits.push(`${this.scoreWon}-${this.scoreLost}`);
    if (this.roundNumber) bits.push(`R${this.roundNumber}`);
    return bits.join(' · ');
  }

  handleEvent(evt, ts) {
    const out = [];
    const key = evt.key;
    const value = evt.value;

    switch (key) {
      case 'kill':
        this.kills = this._num(value, this.kills + 1);
        this._reconcileKills(out, ts);
        break;
      case 'assist':
        this.assists = this._num(value, this.assists + 1);
        out.push(this._mk('assist', 'Assist', '', IMP.MINOR, ts));
        break;
      case 'headshot':
        this.headshots = this._num(value, this.headshots + 1);
        break;
      case 'death': {
        this.deaths = this._num(value, this.deaths + 1);
        this._localDied(out, ts);
        out.push(this._mk('death', 'Death', '', IMP.MINOR, ts));
        break;
      }
      case 'match_start':
        this._resetMatch();
        out.push(this._mk('match_start', 'Match Start', this.map || '', IMP.NOTABLE, ts));
        break;
      case 'match_end':
        out.push(this._mk('match_end', 'Match End', this._matchSummary(), IMP.NOTABLE, ts));
        break;
      case 'spike_defused':
        out.push(this._mk('spike_defused', 'Spike Defused', this.contextDetail(), IMP.EPIC, ts));
        break;
      case 'spike_detonated':
        out.push(this._mk('spike_detonated', 'Spike Detonated', this.contextDetail(), IMP.NOTABLE, ts));
        break;
      case 'planted_location':
        out.push(this._mk('spike_planted', 'Spike Planted', `Site ${value}`, IMP.NOTABLE, ts));
        break;
      case 'kill_feed':
        this._killFeed(value, ts, out);
        break;
      default:
        break;
    }
    return out;
  }

  handleInfo(info, ts) {
    const out = [];
    const key = info.key;
    const value = info.value;

    if (key === 'player_name' && value) {
      this.localPlayer = baseName(value);
      // retroactively tag any scoreboard entry that matches
      for (const s of this.scoreboard.values()) if (s.key === this.localPlayer) s.isLocal = true;
      return out;
    }

    if (key === 'agent' && value) {
      const display = prettyAgent(value);
      if (display && display !== this.agent) {
        this.agent = display;
        out.push(this._ctx('agent', display, ts));
      }
      return out;
    }

    if (key === 'scene' && value) {
      const scene = String(value);
      const map = prettyMap(scene);
      if (map && map !== this.map) {
        this.map = map;
        out.push(this._ctx('map', map, ts));
      }
      let kind = 'other';
      if (/^CharacterSelect/i.test(scene)) kind = 'agent_select';
      else if (/^MainMenu$/i.test(scene)) kind = 'menu';
      else if (map) kind = 'map';
      out.push(this._ctx('scene', kind, ts));
      return out;
    }

    if (key === 'round_number') {
      const n = parseInt(value, 10);
      if (n && n !== this.roundNumber) {
        this.roundNumber = n;
        out.push(this._ctx('round', String(n), ts));
      }
      return out;
    }

    if (key === 'score' && value && typeof value === 'object') {
      const won = this._num(value.won, this.scoreWon);
      const lost = this._num(value.lost, this.scoreLost);
      // During getInfo() priming we adopt the score without emitting round
      // outcomes (they already happened before the companion attached).
      if (!this.priming) {
        if (won > this.scoreWon) {
          if (this.clutchArmed && this.clutchVs >= 1 && !this.localDeadThisRound) {
            const n = Math.min(this.clutchVs, 5);
            out.push(this._mk(`clutch_1v${n}`, `CLUTCH 1v${n}`, `round ${this.roundNumber}`,
                              n >= 2 ? IMP.EPIC : IMP.NOTABLE, ts));
          }
          out.push(this._mk('round_won', 'Round Won', '', IMP.MINOR, ts));
        } else if (lost > this.scoreLost) {
          out.push(this._mk('round_lost', 'Round Lost', '', IMP.DEBUG, ts));
        }
      }
      this.scoreWon = won;
      this.scoreLost = lost;
      this.clutchArmed = false;
      this.clutchVs = 0;
      out.push(this._ctx('score', `${won}-${lost}`, ts));
      return out;
    }

    if (/^scoreboard_\d+$/.test(key) && value && typeof value === 'object') {
      this._scoreboardUpdate(value, out, ts);
      return out;
    }

    if (key === 'round_phase') {
      const phase = String(value);
      if (phase !== this.lastRoundPhase) {
        this.lastRoundPhase = phase;
        if (phase === 'shopping') this._resetRound();
        if (!this.priming) {
          out.push({
            t: 'info',
            game: { id: 21640, name: 'VALORANT' },
            feature: 'match_info',
            category: 'match_info',
            key: 'round_phase',
            value: phase,
            ts,
          });
        }
      }
      return out;
    }

    return out;
  }

  /* Replay a gep.getInfo() snapshot to adopt current state without emitting
     derived events (round outcomes / clutch) for things that already happened.
     Accepts the {feature:{key:value}} shape; values are assumed decoded. */
  primeFromSnapshot(snapshot, ts) {
    const out = [];
    if (!snapshot || typeof snapshot !== 'object') return out;
    const root = snapshot.res && typeof snapshot.res === 'object' ? snapshot.res : snapshot;
    this.priming = true;
    try {
      for (const [feature, group] of Object.entries(root)) {
        if (!group || typeof group !== 'object' || Array.isArray(group)) continue;
        for (const [key, value] of Object.entries(group)) {
          out.push(...this.handleInfo({ gameId: 21640, feature, category: feature, key, value }, ts));
        }
      }
    } finally {
      this.priming = false;
    }
    return out;
  }

  _scoreboardUpdate(sb, out, ts) {
    const rawName = sb.name || sb.player_id;
    if (!rawName) return;
    const key = baseName(rawName);
    const isLocal =
      sb.is_local === true || sb.is_local === 'true' || (this.localPlayer && key === this.localPlayer);
    const prev = this.scoreboard.get(key);
    this.scoreboard.set(key, {
      key,
      teammate: sb.teammate === true || sb.teammate === 'true',
      alive: sb.alive === true || sb.alive === 'true',
      isLocal,
      lastRound: this.roundNumber,
    });
    if (isLocal && !(prev && prev.alive) && !(sb.alive === true || sb.alive === 'true')) {
      // scoreboard reports us dead
      this._localDied(out, ts);
    }
    this._maybeArmClutch(out, ts);
  }

  /* Arm a clutch when: local alive, 0 living teammates, >=1 living enemies,
     during combat. Called after any alive-state change (scoreboard or feed).
     Uses only entries seen this round so stale/disconnected players don't
     inflate the count. */
  _maybeArmClutch(out, ts) {
    if (this.lastRoundPhase !== 'combat' || this.localDeadThisRound) return;
    const me = this._findLocal();
    if (!me || !me.alive) return;

    let aliveTeammates = 0;
    let aliveEnemies = 0;
    let known = 0;
    for (const s of this.scoreboard.values()) {
      if (s.lastRound !== this.roundNumber && this.roundNumber !== 0) continue; // stale
      known++;
      if (s.isLocal) continue;
      if (s.teammate && s.alive) aliveTeammates++;
      else if (!s.teammate && s.alive) aliveEnemies++;
    }
    if (aliveTeammates !== 0 || aliveEnemies < 1 || known < 4) return;

    // Track the PEAK living-enemy count while alone: a 1v3 that you trade down
    // to 1v1 is still a 1v3, and an incrementally-populated scoreboard would
    // otherwise latch a too-low N. Only (re)emit context when N rises.
    if (!this.clutchArmed || aliveEnemies > this.clutchVs) {
      this.clutchArmed = true;
      this.clutchVs = Math.max(this.clutchVs, aliveEnemies);
      out.push(this._ctx('clutch', `1v${this.clutchVs}`, ts));
    }
  }

  _localDied(out, ts) {
    this.localDeadThisRound = true;
    // a dead player can't clutch (a post-mortem round win isn't a clutch)
    this.clutchArmed = false;
    this.clutchVs = 0;
    const me = this._findLocal();
    if (me) me.alive = false;
  }

  _findLocal() {
    for (const s of this.scoreboard.values()) if (s.isLocal) return s;
    if (this.localPlayer) return this.scoreboard.get(this.localPlayer) || null;
    return null;
  }

  _matchSummary() {
    const bits = [];
    if (this.scoreWon || this.scoreLost) bits.push(`${this.scoreWon}-${this.scoreLost}`);
    if (this.map) bits.push(this.map);
    if (this.agent) bits.push(this.agent);
    bits.push(`${this.kills}/${this.deaths}/${this.assists}`);
    if (this.kills > 0 && this.headshots > 0) bits.push(`HS ${Math.round((100 * this.headshots) / this.kills)}%`);
    return bits.join(' · ');
  }

  _killFeed(value, ts, out) {
    if (!value || typeof value !== 'object') return;

    const attackerKey = baseName(value.attacker);
    const victimKey = baseName(value.victim);

    // update alive-states from the feed, then re-check clutch arming
    const v = this.scoreboard.get(victimKey);
    if (v) v.alive = false;
    if (this.localPlayer && victimKey === this.localPlayer) this._localDied(out, ts);

    if (!(this.localPlayer && attackerKey === this.localPlayer)) {
      // someone else's kill; it may still have armed our clutch
      this._maybeArmClutch(out, ts);
      return;
    }

    // our kill — queue the detail; the kill event itself is emitted when the
    // authoritative counter increments (may already have, if the counter led).
    const weapon = prettyWeapon(value.weapon);
    const hs = value.headshot ? ' (HS)' : '';
    const parts = [weapon, value.victim ? '→ ' + value.victim : ''].filter(Boolean);
    this.pendingKillDetail.push({ detail: (parts.join(' ') + hs).trim(), victim: value.victim || '' });
    this._reconcileKills(out, ts);
    this._maybeArmClutch(out, ts);
  }

  /* Emit exactly (this.kills - emittedKills) kill events, pulling queued detail
     FIFO. first_blood fires on the round's first emitted kill. */
  _reconcileKills(out, ts) {
    while (this.emittedKills < this.kills) {
      const d = this.pendingKillDetail.shift();
      const ctx = this.contextDetail();
      let detail = d ? d.detail : '';
      if (ctx) detail = (detail ? detail + '  · ' : '') + ctx;
      out.push(this._mk('kill', 'Kill', detail.trim(), IMP.NOTABLE, ts));
      this.emittedKills++;
      if (!this.roundFirstKillSeen) {
        this.roundFirstKillSeen = true;
        out.push(this._mk('first_blood', 'First Blood', d && d.victim ? `on ${d.victim}` : '', IMP.NOTABLE, ts));
      }
    }
  }

  _num(v, fallback) {
    const n = typeof v === 'number' ? v : parseInt(v, 10);
    return Number.isFinite(n) ? n : fallback;
  }

  _mk(name, label, detail, importance, ts) {
    return { t: 'event', game: { id: 21640, name: 'VALORANT' }, name, label, detail, importance, ts };
  }

  _ctx(key, value, ts) {
    return { t: 'info', game: { id: 21640, name: 'VALORANT' }, feature: 'context', category: 'context', key, value, ts };
  }
}

module.exports = { ValorantNormalizer, prettyWeapon, prettyAgent, prettyMap, baseName };
