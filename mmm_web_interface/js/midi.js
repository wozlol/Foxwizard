// Web MIDI + SysEx transport for the Foxwizard Mini MIDI Multitool.
// Wire format: see ../SYSEX_PROTOCOL.md — keep this file and the firmware's command table in sync.
// The device talks the same protocol over USB or DIN; Chrome's Web MIDI only ever exposes it via
// USB, so that's the only path this file actually drives, but nothing here assumes USB-only.

export const CMD = {
  IDENTITY_REQUEST: 0x01,
  IDENTITY_REPLY: 0x02,
  GET_GLOBAL: 0x10,
  GLOBAL_DATA: 0x11,
  SET_GLOBAL: 0x12,
  GET_ROUTE_COUNT: 0x20,
  ROUTE_COUNT: 0x21,
  GET_ROUTE: 0x22,
  ROUTE_DATA: 0x23,
  ADD_ROUTE: 0x24,
  SET_ROUTE: 0x25,
  DELETE_ROUTE: 0x26,
  MOVE_ROUTE: 0x27,
  CLEAR_ALL_ROUTES: 0x28,
  PREVIEW_BRIGHTNESS: 0x30,
  COMMIT: 0x31,
  FACTORY_RESET: 0x32,
  ACK: 0x7E
};

const MFR_ID = 0x7D;
const DEVICE_ID = 0x4D;
export const ROUTE_NAME_MAX_LEN = 24; // must match ROUTE_NAME_MAX_LEN in the firmware
const ROUTE_RAW_BYTES = 13 + ROUTE_NAME_MAX_LEN;
const DEFAULT_TIMEOUT_MS = 1500;

// Fixed-length, zero-padded ASCII — matches the firmware's raw char[ROUTE_NAME_MAX_LEN] field
// exactly, so there's no separate length byte to keep in sync.
function encodeName(name) {
  const bytes = [];
  for (let i = 0; i < ROUTE_NAME_MAX_LEN; i++) {
    const ch = (name || '')[i];
    // restrict to printable ASCII — anything else (emoji, accents, etc.) would need multiple raw
    // bytes per character and break the fixed-length assumption on both ends of the wire
    const code = ch ? ch.charCodeAt(0) : 0;
    bytes.push(code >= 32 && code <= 126 ? code : (ch ? 0x3F /* '?' */ : 0));
  }
  return bytes;
}

function decodeName(bytes) {
  let out = '';
  for (const b of bytes) {
    if (b === 0) break;
    out += String.fromCharCode(b);
  }
  return out;
}

export function pack7(raw) {
  const out = [];
  for (let i = 0; i < raw.length; i += 7) {
    const group = raw.slice(i, i + 7);
    let msbs = 0;
    for (let j = 0; j < group.length; j++) msbs |= ((group[j] >> 7) & 1) << j;
    out.push(msbs);
    for (let j = 0; j < group.length; j++) out.push(group[j] & 0x7F);
  }
  return out;
}

export function unpack7(packed) {
  const out = [];
  let i = 0;
  while (i < packed.length) {
    const msbs = packed[i++];
    const groupLen = Math.min(7, packed.length - i);
    for (let j = 0; j < groupLen; j++) {
      out.push(packed[i + j] | (((msbs >> j) & 1) << 7));
    }
    i += groupLen;
  }
  return out;
}

export function checksum7(bytes) {
  let sum = 0;
  for (const b of bytes) sum += b;
  return sum % 128;
}

function read16(bytes, offset) {
  return bytes[offset] | (bytes[offset + 1] << 7);
}

function write16(v) {
  return [v & 0x7F, (v >> 7) & 0x7F];
}

/** Route field object <-> 37 raw wire bytes (13 fixed fields + 24-byte name). */
export function routeToBytes(r) {
  return [
    (r.enabled ? 0x80 : 0) | (r.inputDevice & 0x03),
    r.inputChannels & 0x7F,
    (r.inputChannels >> 8) & 0x7F,
    r.typeFlags & 0x7F,
    r.noteStart & 0x7F,
    r.noteEnd & 0x7F,
    r.ccStart & 0x7F,
    r.ccEnd & 0x7F,
    r.outputDevice & 0x03,
    r.outputChannels & 0x7F,
    (r.outputChannels >> 8) & 0x7F,
    (r.transpose + 64) & 0x7F,
    r.ccMapStart & 0x7F,
    ...encodeName(r.name)
  ];
}

export function bytesToRoute(b) {
  return {
    enabled: !!(b[0] & 0x80),
    inputDevice: b[0] & 0x03,
    inputChannels: b[1] | (b[2] << 8),
    typeFlags: b[3],
    noteStart: b[4],
    noteEnd: b[5],
    ccStart: b[6],
    ccEnd: b[7],
    outputDevice: b[8],
    outputChannels: b[9] | (b[10] << 8),
    transpose: b[11] - 64,
    ccMapStart: b[12],
    name: decodeName(b.slice(13, 13 + ROUTE_NAME_MAX_LEN))
  };
}

export const DATA_TYPE_BITS = {
  note: 0x01, cc: 0x02, programChange: 0x04, pitchBend: 0x08,
  aftertouch: 0x10, pressure: 0x20, sysex: 0x40
};

export class DeviceLink extends EventTarget {
  constructor() {
    super();
    this.access = null;
    this.input = null;
    this.output = null;
    this.connected = false;
    this.deviceInfo = null;
    this._pending = null; // { resolve, reject, matchCmd, timer }
  }

  emit(name, detail) { this.dispatchEvent(new CustomEvent(name, { detail })); }

  async init() {
    if (!navigator.requestMIDIAccess) {
      this.emit('status', { state: 'unsupported' });
      return;
    }
    try {
      this.access = await navigator.requestMIDIAccess({ sysex: true });
    } catch (err) {
      this.emit('status', { state: 'denied' });
      return;
    }
    this.access.onstatechange = () => this._tryAutoConnect();
    await this._tryAutoConnect();
  }

  async _tryAutoConnect() {
    if (this.connected) return;
    let foundInput = null, foundOutput = null;
    for (const input of this.access.inputs.values()) {
      if (/foxwizard/i.test(input.name || '')) { foundInput = input; break; }
    }
    for (const output of this.access.outputs.values()) {
      if (/foxwizard/i.test(output.name || '')) { foundOutput = output; break; }
    }
    if (!foundInput || !foundOutput) {
      this.emit('status', { state: 'disconnected' });
      return;
    }
    this.emit('status', { state: 'connecting' });
    this.input = foundInput;
    this.output = foundOutput;
    this.input.onmidimessage = (e) => this._onMessage(e);

    try {
      const identity = await this._sendAndWait(
        [CMD.IDENTITY_REQUEST], CMD.IDENTITY_REPLY, DEFAULT_TIMEOUT_MS
      );
      const nameBytes = identity.slice(5);
      this.deviceInfo = {
        protocolVersion: identity[0],
        fwMajor: identity[1],
        fwMinor: identity[2],
        maxRoutes: read16(identity, 3),
        name: String.fromCharCode(...nameBytes)
      };
      this.connected = true;
      this.emit('status', { state: 'connected', deviceInfo: this.deviceInfo });
    } catch (err) {
      this.connected = false;
      this.emit('status', { state: 'disconnected' });
    }
  }

  disconnectManually() {
    this.connected = false;
    if (this.input) this.input.onmidimessage = null;
    this.input = null;
    this.output = null;
    this.emit('status', { state: 'disconnected' });
  }

  _onMessage(e) {
    const data = e.data;
    if (data[0] !== 0xF0 || data[1] !== MFR_ID || data[2] !== DEVICE_ID) return;
    const cmd = data[3];
    const payload = Array.from(data.slice(4, data.length - 1)); // strip F0 7D 4D cmd ... F7
    if (this._pending && (this._pending.matchCmd === cmd || (this._pending.matchCmd === CMD.ACK && cmd === CMD.ACK))) {
      const p = this._pending;
      this._pending = null;
      clearTimeout(p.timer);
      p.resolve(payload);
    }
  }

  _sendRaw(cmd, payload) {
    const bytes = [0xF0, MFR_ID, DEVICE_ID, cmd, ...payload, 0xF7];
    this.output.send(bytes);
  }

  _sendAndWait(cmdAndPayload, expectCmd, timeoutMs) {
    return new Promise((resolve, reject) => {
      if (this._pending) { reject(new Error('busy')); return; }
      const cmd = cmdAndPayload[0];
      const payload = cmdAndPayload.slice(1);
      const timer = setTimeout(() => {
        this._pending = null;
        reject(new Error('timeout'));
      }, timeoutMs);
      this._pending = { resolve, reject, matchCmd: expectCmd, timer };
      this._sendRaw(cmd, payload);
    });
  }

  /** Sends a mutating command, waits for ACK, retries once on timeout/checksum mismatch. */
  async _sendMutating(cmd, payload, expectedChecksum) {
    for (let attempt = 0; attempt < 2; attempt++) {
      try {
        const ack = await this._sendAndWait([cmd, ...payload], CMD.ACK, DEFAULT_TIMEOUT_MS);
        const [originalCmd, status, checksum, extraLo, extraHi] = ack;
        const extra = extraLo | (extraHi << 7);
        if (status !== 0) throw new Error('device-error-' + status);
        if (expectedChecksum !== undefined && checksum !== expectedChecksum) throw new Error('checksum-mismatch');
        return { status, checksum, extra };
      } catch (err) {
        if (attempt === 1) throw err;
      }
    }
  }

  async getGlobal() {
    const reply = await this._sendAndWait([CMD.GET_GLOBAL], CMD.GLOBAL_DATA, DEFAULT_TIMEOUT_MS);
    return {
      mode: reply[0],
      buttonAction: reply[1],
      buttonParamA: reply[2],
      buttonParamB: reply[3],
      buttonToggleMomentary: reply[4],
      ledBrightnessStep: reply[5]
    };
  }

  async setGlobal(g) {
    const payload = [g.mode, g.buttonAction, g.buttonParamA, g.buttonParamB, g.buttonToggleMomentary, g.ledBrightnessStep];
    return this._sendMutating(CMD.SET_GLOBAL, payload, checksum7(payload));
  }

  async getRouteCount() {
    const reply = await this._sendAndWait([CMD.GET_ROUTE_COUNT], CMD.ROUTE_COUNT, DEFAULT_TIMEOUT_MS);
    return read16(reply, 0);
  }

  async getRoute(index) {
    const reply = await this._sendAndWait([CMD.GET_ROUTE, ...write16(index)], CMD.ROUTE_DATA, DEFAULT_TIMEOUT_MS);
    const packed = reply.slice(2);
    return bytesToRoute(unpack7(packed));
  }

  async addRoute(route) {
    const raw = routeToBytes(route);
    const result = await this._sendMutating(CMD.ADD_ROUTE, pack7(raw), checksum7(raw));
    return result.extra; // assigned index
  }

  async setRoute(index, route) {
    const raw = routeToBytes(route);
    return this._sendMutating(CMD.SET_ROUTE, [...write16(index), ...pack7(raw)], checksum7(raw));
  }

  async deleteRoute(index) {
    return this._sendMutating(CMD.DELETE_ROUTE, write16(index));
  }

  async moveRoute(from, to) {
    return this._sendMutating(CMD.MOVE_ROUTE, [...write16(from), ...write16(to)]);
  }

  async clearAllRoutes() {
    return this._sendMutating(CMD.CLEAR_ALL_ROUTES, []);
  }

  async previewBrightness(step) {
    return this._sendMutating(CMD.PREVIEW_BRIGHTNESS, [step], step);
  }

  async commit() {
    return this._sendMutating(CMD.COMMIT, []);
  }

  async factoryReset() {
    return this._sendMutating(CMD.FACTORY_RESET, []);
  }

  /** Pulls global settings + every route from the device. progressCb(done, total). */
  async loadAll(progressCb) {
    const global = await this.getGlobal();
    const count = await this.getRouteCount();
    const routes = [];
    for (let i = 0; i < count; i++) {
      routes.push(await this.getRoute(i));
      if (progressCb) progressCb(i + 1, count);
    }
    return { global, routes };
  }

  /** Pushes global settings + a full route list to the device, replacing whatever is there. */
  async saveAll(global, routes, progressCb) {
    await this.clearAllRoutes();
    for (let i = 0; i < routes.length; i++) {
      await this.addRoute(routes[i]);
      if (progressCb) progressCb(i + 1, routes.length);
    }
    await this.setGlobal(global);
    await this.commit();
  }
}
