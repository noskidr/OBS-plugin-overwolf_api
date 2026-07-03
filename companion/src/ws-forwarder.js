'use strict';

/*
 * GamePulse Companion — WebSocket forwarder.
 *
 * Maintains a client connection to the OBS plugin's localhost WebSocket
 * server (ws://127.0.0.1:<port>[/?token=..]) with automatic reconnect and a
 * small outbound queue so events survive brief disconnects. Uses a raw RFC
 * 6455 client over net.Socket so the companion has zero runtime npm deps
 * (ow-electron fetches its packages at runtime; we keep node_modules empty in
 * production).
 */

const net = require('net');
const crypto = require('crypto');
const EventEmitter = require('events');

class WsForwarder extends EventEmitter {
  constructor() {
    super();
    this.host = '127.0.0.1';
    this.port = 4477;
    this.token = '';
    this.socket = null;
    this.connected = false;
    this.handshakeDone = false;
    this.shouldRun = false;
    this.reconnectTimer = null;
    this.queue = [];
    this.maxQueue = 200;
    this.recvBuffer = Buffer.alloc(0);
    this.pingTimer = null;
  }

  configure(port, token) {
    const changed = port !== this.port || token !== this.token;
    this.port = port;
    this.token = token || '';
    if (changed && this.shouldRun) {
      this._teardown();
      this._connect();
    }
  }

  start() {
    this.shouldRun = true;
    this._connect();
  }

  stop() {
    this.shouldRun = false;
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null; // else a later start() can never reschedule
    }
    this._teardown();
  }

  // Force an immediate reconnect regardless of whether settings changed.
  reconnect() {
    if (!this.shouldRun) {
      this.start();
      return;
    }
    this._teardown();
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    this._connect();
  }

  isConnected() {
    return this.connected && this.handshakeDone;
  }

  send(obj) {
    const text = JSON.stringify(obj);
    if (this.isConnected()) {
      this._sendText(text);
    } else {
      if (this.queue.length >= this.maxQueue) this.queue.shift();
      this.queue.push(text);
    }
  }

  _connect() {
    if (!this.shouldRun || this.socket) return;

    const socket = new net.Socket();
    this.socket = socket;
    this.handshakeDone = false;
    this.recvBuffer = Buffer.alloc(0);

    const key = crypto.randomBytes(16).toString('base64');
    const expectedAccept = crypto
      .createHash('sha1')
      .update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
      .digest('base64');

    socket.setNoDelay(true);

    socket.on('connect', () => {
      this.connected = true;
      const path = this.token ? `/?token=${encodeURIComponent(this.token)}` : '/';
      const req =
        `GET ${path} HTTP/1.1\r\n` +
        `Host: ${this.host}:${this.port}\r\n` +
        `Upgrade: websocket\r\n` +
        `Connection: Upgrade\r\n` +
        `Sec-WebSocket-Key: ${key}\r\n` +
        `Sec-WebSocket-Version: 13\r\n\r\n`;
      socket.write(req);
    });

    socket.on('data', (chunk) => {
      this.recvBuffer = Buffer.concat([this.recvBuffer, chunk]);
      if (!this.handshakeDone) {
        const headerEnd = this.recvBuffer.indexOf('\r\n\r\n');
        if (headerEnd === -1) return;
        const header = this.recvBuffer.slice(0, headerEnd).toString('utf8');
        this.recvBuffer = this.recvBuffer.slice(headerEnd + 4);
        if (!/101/.test(header.split('\r\n')[0]) || header.indexOf(expectedAccept) === -1) {
          this.emit('log', 'warn', 'plugin handshake rejected (bad token or not a GamePulse server)');
          this._teardown();
          this._scheduleReconnect();
          return;
        }
        this.handshakeDone = true;
        this.emit('status', true);
        this.emit('log', 'info', `connected to OBS plugin at ws://${this.host}:${this.port}`);
        this._sendText(JSON.stringify({ t: 'hello', client: 'gamepulse-companion', version: '0.2.0', token: this.token }));
        this._flushQueue();
        this._startPing();
      }
      this._drainFrames();
    });

    const onGone = (why) => {
      // Ignore late close/error events from a socket we've already replaced —
      // otherwise a stale event would tear down the new connection.
      if (this.socket !== socket) return;
      this.emit('log', 'debug', `plugin connection closed (${why})`);
      this._teardown();
      this._scheduleReconnect();
    };

    socket.on('error', (err) => onGone(err.code || err.message));
    socket.on('close', () => onGone('close'));

    // socket.connect throws synchronously on an out-of-range port; don't let
    // that wedge the forwarder with this.socket set.
    try {
      socket.connect(this.port, this.host);
    } catch (err) {
      this.emit('log', 'warn', `invalid connection settings: ${err.message}`);
      onGone(err.message);
    }
  }

  _drainFrames() {
    // Minimal server->client frame parser (unmasked, text/close/ping).
    while (this.recvBuffer.length >= 2) {
      const b0 = this.recvBuffer[0];
      const b1 = this.recvBuffer[1];
      const opcode = b0 & 0x0f;
      const masked = (b1 & 0x80) !== 0;
      let len = b1 & 0x7f;
      let offset = 2;
      if (len === 126) {
        if (this.recvBuffer.length < 4) return;
        len = this.recvBuffer.readUInt16BE(2);
        offset = 4;
      } else if (len === 127) {
        if (this.recvBuffer.length < 10) return;
        const big = this.recvBuffer.readBigUInt64BE(2);
        // The plugin never sends frames this large; a bogus huge length would
        // otherwise stall the parser forever waiting for bytes. Drop the link.
        if (big > 4n * 1024n * 1024n) {
          this.emit('log', 'warn', 'oversized frame from plugin — dropping connection');
          this._teardown();
          this._scheduleReconnect();
          return;
        }
        len = Number(big);
        offset = 10;
      }
      const maskLen = masked ? 4 : 0;
      if (this.recvBuffer.length < offset + maskLen + len) return;
      let payload = this.recvBuffer.slice(offset + maskLen, offset + maskLen + len);
      if (masked) {
        const mask = this.recvBuffer.slice(offset, offset + 4);
        const copy = Buffer.from(payload);
        for (let i = 0; i < copy.length; i++) copy[i] ^= mask[i & 3];
        payload = copy;
      }
      this.recvBuffer = this.recvBuffer.slice(offset + maskLen + len);

      if (opcode === 0x8) {
        this._teardown();
        this._scheduleReconnect();
        return;
      } else if (opcode === 0x9) {
        this._sendFrame(0xa, payload); // pong
      } else if (opcode === 0x1) {
        this.emit('plugin-message', payload.toString('utf8'));
      }
    }
  }

  _sendText(text) {
    this._sendFrame(0x1, Buffer.from(text, 'utf8'));
  }

  _sendFrame(opcode, payload) {
    if (!this.socket || !this.connected) return;
    const len = payload.length;
    let header;
    // client frames MUST be masked
    const mask = crypto.randomBytes(4);
    if (len < 126) {
      header = Buffer.alloc(2);
      header[1] = 0x80 | len;
    } else if (len < 65536) {
      header = Buffer.alloc(4);
      header[1] = 0x80 | 126;
      header.writeUInt16BE(len, 2);
    } else {
      header = Buffer.alloc(10);
      header[1] = 0x80 | 127;
      header.writeBigUInt64BE(BigInt(len), 2);
    }
    header[0] = 0x80 | opcode;
    const masked = Buffer.from(payload);
    for (let i = 0; i < masked.length; i++) masked[i] ^= mask[i & 3];
    try {
      this.socket.write(Buffer.concat([header, mask, masked]));
    } catch (_) {
      /* teardown handles it */
    }
  }

  _flushQueue() {
    const pending = this.queue;
    this.queue = [];
    for (const text of pending) this._sendText(text);
  }

  _startPing() {
    if (this.pingTimer) clearInterval(this.pingTimer);
    this.pingTimer = setInterval(() => {
      if (this.isConnected()) this._sendFrame(0x9, Buffer.alloc(0));
    }, 20000);
  }

  _teardown() {
    if (this.pingTimer) {
      clearInterval(this.pingTimer);
      this.pingTimer = null;
    }
    if (this.socket) {
      const s = this.socket;
      this.socket = null;
      try {
        s.destroy();
      } catch (_) {}
    }
    if (this.connected || this.handshakeDone) this.emit('status', false);
    this.connected = false;
    this.handshakeDone = false;
  }

  _scheduleReconnect() {
    if (!this.shouldRun || this.reconnectTimer) return;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this._connect();
    }, 2500);
  }
}

module.exports = { WsForwarder };
