'use strict';

/*
 * GamePulse Companion — Valorant event normalizer + match state machine.
 *
 * Turns raw Overwolf GEP Valorant events/info-updates into GamePulse protocol
 * events with human detail, and derives the Valorant-signature moments that
 * raw GEP does not emit:
 *
 *   first_blood     — the round's first kill, when it's the local player's
 *   clutch_1vN      — local player alone vs N enemies (from live scoreboard
 *                     alive-states), then the round is WON
 *   round_won/lost  — score-change outcomes
 *   match summary   — final score · map · K/D/A detail on match_end
 *
 * It also tracks context (agent, map, score, round) and emits it as protocol
 * "context" infos so the plugin can label chapters/markers/overlay with
 * "Haven · 8-4 · R13".
 *
 * Multikill/ace stay derived in the OBS plugin (single source of truth for
 * anything computable from the kill stream alone); clutch/first-blood need
 * roster/score state only the companion sees, so they are derived here.
 *
 * GEP Valorant schema (verified against dev.overwolf.com + insights.gg mining):
 *   feature "kill":  keys kill|assist|headshot (running totals)
 *   feature "death": key death (running total)
 *   feature "me":    player_name, agent (codename), health…
 *   feature "game_info": scene (map codename / MainMenu / CharacterSelect…)
 *   feature "match_info": kill_feed, match_start/end, spike_*, round_phase,
 *                         round_number, score {won,lost}, scoreboard_N, roster_N
 */

const IMP = { DEBUG: 0, MINOR: 1, NOTABLE: 2, EPIC: 3 };

// TX_Hud_* weapon texture id -> display name.
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

// Internal agent codename -> display name (suffix _PC_C stripped first).
const AGENT_DICT = {
  Clay: 'Raze',
  Pandemic: 'Viper',
  Wraith: 'Omen',
  Hunter: 'Sova',
  Thorne: 'Sage',
  Phoenix: 'Phoenix',
  Wushu: 'Jett',
  Gumshoe: 'Cypher',
  Sarge: 'Brimstone',
  Breach: 'Breach',
  Vampire: 'Reyna',
  Killjoy: 'Killjoy',
  Guide: 'Skye',
  Stealth: 'Yoru',
  Rift: 'Astra',
  Grenadier: 'KAY/O',
  Deadeye: 'Chamber',
  Sprinter: 'Neon',
  BountyHunter: 'Fade',
  Mage: 'Harbor',
  AggroBot: 'Gekko',
  Cable: 'Deadlock',
  Sequoia: 'Iso',
  Smonk: 'Clove',
  Nox: 'Vyse',
  Cashew: 'Tejo',
  Terra: 'Waylay',
};

// Internal map codename -> display name.
const MAP_DICT = {
  Ascent: 'Ascent',
  Triad: 'Haven',
  Duality: 'Bind',
  Bonsai: 'Split',
  Port: 'Icebox',
  Foxtrot: 'Breeze',
  Canyon: 'Fracture',
  Pitt: 'Pearl',
  Jam: 'Lotus',
  Juliett: 'Sunset',
  Infinity: 'Abyss',
  Rook: 'Corrode',
  Range: 'Practice Range',
  HURM_Alley: 'District',
  HURM_Yard: 'Piazza',
  HURM_Bowl: 'Kasbah',
  HURM_Helix: 'Drift',
  HURM_HighTide: 'Glitch',
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

class ValorantNormalizer {
  constructor() {
    this.reset();
  }

  reset() {
    this.localPlayer = '';
    this.agent = '';
    this.map = '';
    this.lastRoundPhase = '';
    this.roundNumber = 0;
    this.scoreWon = 0;
    this.scoreLost = 0;
    // per-match running totals (from GEP counters)
    this.kills = 0;
    this.deaths = 0;
    this.assists = 0;
    this.headshots = 0;
    // per-round state
    this.roundFirstKillSeen = false;
    this.clutchArmed = false;
    this.clutchVs = 0;
    // scoreboard: name -> { teammate, alive, isLocal }
    this.scoreboard = new Map();
  }

  _resetMatch() {
    const lp = this.localPlayer;
    const agent = this.agent;
    const map = this.map;
    this.reset();
    this.localPlayer = lp;
    this.agent = agent;
    this.map = map;
  }

  _resetRound() {
    this.roundFirstKillSeen = false;
    this.clutchArmed = false;
    this.clutchVs = 0;
    for (const s of this.scoreboard.values()) s.alive = true;
  }

  contextDetail() {
    const bits = [];
    if (this.scoreWon || this.scoreLost) bits.push(`${this.scoreWon}-${this.scoreLost}`);
    if (this.roundNumber) bits.push(`R${this.roundNumber}`);
    return bits.join(' · ');
  }

  // Returns an array of protocol messages (events + context infos).
  handleEvent(evt, ts) {
    const out = [];
    const key = evt.key;
    const value = evt.value;

    switch (key) {
      case 'kill':
        this.kills = this._num(value, this.kills + 1);
        // Counter carries no detail; kill_feed is the canonical kill once the
        // local player is known. Fall back to the counter so kills are never lost.
        if (!this.localPlayer) out.push(this._mk('kill', 'Kill', this.contextDetail(), IMP.NOTABLE, ts));
        break;
      case 'assist':
        this.assists = this._num(value, this.assists + 1);
        out.push(this._mk('assist', 'Assist', '', IMP.MINOR, ts));
        break;
      case 'headshot':
        this.headshots = this._num(value, this.headshots + 1);
        // detail already carried by kill_feed's (HS); counter stays silent
        // once we can attribute feed lines.
        if (!this.localPlayer) out.push(this._mk('headshot', 'Headshot', '', IMP.NOTABLE, ts));
        break;
      case 'death': {
        this.deaths = this._num(value, this.deaths + 1);
        const me = this._findLocal();
        if (me) me.alive = false;
        out.push(this._mk('death', 'Death', '', IMP.MINOR, ts));
        break;
      }
      case 'match_start':
        this._resetMatch();
        out.push(this._mk('match_start', 'Match Start', this.map || '', IMP.NOTABLE, ts));
        break;
      case 'match_end': {
        const summary = this._matchSummary();
        out.push(this._mk('match_end', 'Match End', summary, IMP.NOTABLE, ts));
        break;
      }
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
      this.localPlayer = String(value);
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
      // normalized scene signal for OBS scene automation
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
      if (won > this.scoreWon) {
        // round won — if a clutch was armed, this is the payoff
        if (this.clutchArmed && this.clutchVs >= 1) {
          const n = Math.min(this.clutchVs, 5);
          out.push(this._mk(`clutch_1v${n}`, `CLUTCH 1v${n}`, `round ${this.roundNumber}`,
                            n >= 2 ? IMP.EPIC : IMP.NOTABLE, ts));
        }
        out.push(this._mk('round_won', 'Round Won', '', IMP.MINOR, ts));
      } else if (lost > this.scoreLost) {
        out.push(this._mk('round_lost', 'Round Lost', '', IMP.DEBUG, ts));
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
        // pass through for the plugin's round_start/round_end synthesis
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
      return out;
    }

    return out;
  }

  /* Prime state from a gep.getInfo() snapshot (companion started mid-match).
     Shape is a nested dict; walk two levels and feed through handleInfo. */
  primeFromSnapshot(snapshot, ts) {
    const out = [];
    if (!snapshot || typeof snapshot !== 'object') return out;
    const walk = (obj) => {
      for (const [k1, v1] of Object.entries(obj)) {
        if (v1 && typeof v1 === 'object' && !Array.isArray(v1)) {
          for (const [k2, v2] of Object.entries(v1)) {
            out.push(...this.handleInfo({ gameId: 21640, feature: k1, category: k1, key: k2, value: v2 }, ts));
          }
        }
      }
    };
    // common shapes: {res:{...}} or {feature:{key:value}}
    walk(snapshot.res && typeof snapshot.res === 'object' ? snapshot.res : snapshot);
    return out;
  }

  _scoreboardUpdate(sb, out, ts) {
    const name = sb.name || sb.player_id;
    if (!name) return;
    const isLocal = sb.is_local === true || sb.is_local === 'true' ||
                    (this.localPlayer && name === this.localPlayer);
    this.scoreboard.set(name, {
      teammate: sb.teammate === true || sb.teammate === 'true',
      alive: sb.alive === true || sb.alive === 'true',
      isLocal,
    });

    // Clutch arming: local alive, zero living teammates, >=1 living enemies.
    if (!this.clutchArmed && this.lastRoundPhase === 'combat') {
      const me = this._findLocal();
      if (me && me.alive) {
        let aliveTeammates = 0;
        let aliveEnemies = 0;
        for (const s of this.scoreboard.values()) {
          if (s.isLocal) continue;
          if (s.teammate && s.alive) aliveTeammates++;
          if (!s.teammate && s.alive) aliveEnemies++;
        }
        if (aliveTeammates === 0 && aliveEnemies >= 1 && this.scoreboard.size >= 4) {
          this.clutchArmed = true;
          this.clutchVs = aliveEnemies;
          out.push(this._ctx('clutch', `1v${aliveEnemies}`, ts));
        }
      }
    }
  }

  _findLocal() {
    for (const s of this.scoreboard.values()) if (s.isLocal) return s;
    return null;
  }

  _matchSummary() {
    const bits = [];
    if (this.scoreWon || this.scoreLost) bits.push(`${this.scoreWon}-${this.scoreLost}`);
    if (this.map) bits.push(this.map);
    if (this.agent) bits.push(this.agent);
    bits.push(`${this.kills}/${this.deaths}/${this.assists}`);
    if (this.kills > 0 && this.headshots > 0) {
      bits.push(`HS ${Math.round((100 * this.headshots) / this.kills)}%`);
    }
    return bits.join(' · ');
  }

  _killFeed(value, ts, out) {
    if (!value || typeof value !== 'object') return;

    const attacker = value.attacker || '';
    const victim = value.victim || '';

    // First blood: the round's first feed line, when the local player got it.
    const isFirstOfRound = !this.roundFirstKillSeen;
    this.roundFirstKillSeen = true;

    // Track feed deaths on the scoreboard even between scoreboard updates.
    const v = this.scoreboard.get(victim);
    if (v) v.alive = false;

    // Require a known local player so we don't double-count with the counter
    // fallback, and never mark someone else's kill as the streamer's.
    if (!this.localPlayer || attacker !== this.localPlayer) return;

    const weapon = prettyWeapon(value.weapon);
    const hs = value.headshot ? ' (HS)' : '';
    const parts = [weapon, victim ? '→ ' + victim : ''].filter(Boolean);
    const ctx = this.contextDetail();
    const detail = (parts.join(' ') + hs + (ctx ? `  · ${ctx}` : '')).trim();

    out.push(this._mk('kill', 'Kill', detail, IMP.NOTABLE, ts));
    if (isFirstOfRound) {
      out.push(this._mk('first_blood', 'First Blood', victim ? `on ${victim}` : '', IMP.NOTABLE, ts));
    }
  }

  _num(v, fallback) {
    const n = typeof v === 'number' ? v : parseInt(v, 10);
    return Number.isFinite(n) ? n : fallback;
  }

  _mk(name, label, detail, importance, ts) {
    return {
      t: 'event',
      game: { id: 21640, name: 'VALORANT' },
      name,
      label,
      detail,
      importance,
      ts,
    };
  }

  _ctx(key, value, ts) {
    return {
      t: 'info',
      game: { id: 21640, name: 'VALORANT' },
      feature: 'context',
      category: 'context',
      key,
      value,
      ts,
    };
  }
}

module.exports = { ValorantNormalizer, prettyWeapon, prettyAgent, prettyMap };
