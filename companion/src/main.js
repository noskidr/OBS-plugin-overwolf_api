'use strict';

/*
 * GamePulse Companion — ow-electron main process.
 *
 * Pipeline:  GEP (or Simulator) --raw--> ValorantNormalizer --protocol--> WsForwarder --> OBS plugin
 *
 * Run with `npm start` (ow-electron). `npm run simulate` or the tray toggle
 * feeds a mock match without Valorant. Settings persist to userData/gamepulse.json.
 */

const { app, Tray, Menu, BrowserWindow, nativeImage, shell } = require('electron');
const path = require('path');
const fs = require('fs');

const { GepService, VALORANT_ID } = require('./gep-service');
const { ValorantNormalizer } = require('./valorant-normalizer');
const { WsForwarder } = require('./ws-forwarder');
const { Simulator } = require('./simulator');

// Do not send anonymous analytics without user consent.
try {
  if (app.overwolf && typeof app.overwolf.disableAnonymousAnalytics === 'function') {
    app.overwolf.disableAnonymousAnalytics();
  }
} catch (_) {}

let tray = null;
let win = null;
let forwarder = null;
let normalizer = null;
let gep = null;
let simulator = null;
let settings = { port: 4477, token: '', autoConnect: true };
const logBuffer = [];

function settingsPath() {
  return path.join(app.getPath('userData'), 'gamepulse.json');
}

function loadSettings() {
  try {
    const raw = fs.readFileSync(settingsPath(), 'utf8');
    settings = Object.assign(settings, JSON.parse(raw));
  } catch (_) {
    /* defaults */
  }
}

function saveSettings() {
  try {
    fs.mkdirSync(app.getPath('userData'), { recursive: true });
    fs.writeFileSync(settingsPath(), JSON.stringify(settings, null, 2));
  } catch (err) {
    log('warn', `could not save settings: ${err}`);
  }
}

function log(level, message) {
  const line = `[${new Date().toISOString()}] ${level.toUpperCase()} ${message}`;
  logBuffer.push(line);
  if (logBuffer.length > 500) logBuffer.shift();
  // eslint-disable-next-line no-console
  console.log(line);
  if (win && !win.isDestroyed()) win.webContents.send('log', line);
}

function forwardEvents(rawEvents) {
  const ts = Date.now();
  for (const raw of rawEvents) {
    const protoEvents = normalizer.handleEvent(raw, ts);
    for (const p of protoEvents) forwarder.send(p);
  }
}

function wireSource(source) {
  source.on('log', (level, msg) => log(level, msg));
  source.on('game', (state, game) => {
    forwarder.send({ t: 'game', state, game });
    if (state === 'closed' || state === 'detected') normalizer.reset();
    updateTray();
  });
  source.on('gep-event', (evt) => {
    const ts = Date.now();
    const protoEvents = normalizer.handleEvent(evt, ts);
    for (const p of protoEvents) forwarder.send(p);
  });
  source.on('gep-info', (info) => {
    const ts = Date.now();
    const infos = normalizer.handleInfo(info, ts);
    for (const p of infos) forwarder.send(p);
  });
  source.on('prime', (snapshot) => {
    // mid-match start: adopt state (map/agent/score/round) without replaying
    // already-past round outcomes
    for (const p of normalizer.primeFromSnapshot(snapshot, Date.now())) forwarder.send(p);
  });
}

function startSimulator() {
  if (!simulator) {
    simulator = new Simulator();
    wireSource(simulator);
  }
  simulator.start();
  updateTray();
}

function stopSimulator() {
  if (simulator) simulator.stop();
  updateTray();
}

function trayIcon() {
  // 16x16 solid dot; green when connected, gray otherwise. Generated in-memory
  // so the app has no binary asset dependency.
  const connected = forwarder && forwarder.isConnected();
  const size = 16;
  const buf = Buffer.alloc(size * size * 4);
  const [r, g, b] = connected ? [46, 204, 113] : [130, 138, 150];
  for (let i = 0; i < size * size; i++) {
    const x = i % size;
    const y = Math.floor(i / size);
    const dx = x - 7.5;
    const dy = y - 7.5;
    const inside = dx * dx + dy * dy <= 49;
    buf[i * 4] = r;
    buf[i * 4 + 1] = g;
    buf[i * 4 + 2] = b;
    buf[i * 4 + 3] = inside ? 255 : 0;
  }
  return nativeImage.createFromBuffer(buf, { width: size, height: size });
}

function updateTray() {
  if (!tray) return;
  const connected = forwarder && forwarder.isConnected();
  const simRunning = simulator && simulator.running;
  tray.setImage(trayIcon());
  tray.setToolTip(`GamePulse Companion — ${connected ? 'connected' : 'waiting for OBS'}`);
  const menu = Menu.buildFromTemplate([
    { label: connected ? '● Connected to OBS' : '○ Waiting for OBS plugin…', enabled: false },
    { type: 'separator' },
    {
      label: simRunning ? 'Stop simulator' : 'Start simulator (mock match)',
      click: () => (simRunning ? stopSimulator() : startSimulator()),
    },
    { label: 'Reconnect', click: () => forwarder.reconnect() },
    { type: 'separator' },
    { label: 'Settings…', click: () => showWindow() },
    { label: 'Open log folder', click: () => shell.openPath(app.getPath('userData')) },
    { type: 'separator' },
    { label: 'Quit', click: () => app.quit() },
  ]);
  tray.setContextMenu(menu);
}

function showWindow() {
  if (win && !win.isDestroyed()) {
    win.show();
    win.focus();
    return;
  }
  win = new BrowserWindow({
    width: 460,
    height: 560,
    resizable: false,
    title: 'GamePulse Companion',
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });
  win.setMenuBarVisibility(false);
  win.loadFile(path.join(__dirname, 'ui.html'));
  win.webContents.on('did-finish-load', () => {
    win.webContents.send('settings', settings);
    for (const line of logBuffer.slice(-100)) win.webContents.send('log', line);
  });
}

function initIpc() {
  const { ipcMain } = require('electron');
  ipcMain.handle('get-settings', () => settings);
  ipcMain.handle('save-settings', (_e, incoming) => {
    settings = Object.assign(settings, incoming || {});
    saveSettings();
    forwarder.configure(settings.port, settings.token);
    return settings;
  });
  ipcMain.handle('toggle-simulator', () => {
    if (simulator && simulator.running) stopSimulator();
    else startSimulator();
    return simulator ? simulator.running : false;
  });
  ipcMain.handle('reconnect', () => {
    forwarder.reconnect();
    return true;
  });
  ipcMain.handle('status', () => ({
    connected: forwarder && forwarder.isConnected(),
    simulating: simulator && simulator.running,
    activeGame: gep ? gep.activeGame : 0,
  }));
}

function bootstrap() {
  loadSettings();

  normalizer = new ValorantNormalizer();
  forwarder = new WsForwarder();
  forwarder.on('log', (level, msg) => log(level, msg));
  forwarder.on('status', () => updateTray());
  forwarder.on('plugin-message', (text) => {
    try {
      const msg = JSON.parse(text);
      if (msg.t === 'welcome') log('info', `plugin: ${msg.plugin} v${msg.version} on OBS ${msg.obs}`);
    } catch (_) {}
  });
  forwarder.configure(settings.port, settings.token);
  forwarder.start();

  // Real GEP source (no-op if not under ow-electron).
  gep = new GepService(app);
  wireSource(gep);
  gep.start();

  // Tray
  tray = new Tray(trayIcon());
  updateTray();
  tray.on('double-click', () => showWindow());

  initIpc();

  if (process.argv.includes('--gp-simulate')) {
    startSimulator();
  }

  log('info', `GamePulse Companion started (forwarding to ws://127.0.0.1:${settings.port})`);
}

app.on('window-all-closed', (e) => {
  // Keep running in the tray.
});

app.whenReady().then(bootstrap);

app.on('before-quit', () => {
  if (simulator) simulator.stop();
  if (forwarder) forwarder.stop();
  saveSettings();
});
