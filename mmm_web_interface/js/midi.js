// Web MIDI + SysEx transport for the FoxWizard Mini MIDI Multitool.
// Wire format: see ../SYSEX_PROTOCOL.md — keep this file and the firmware's command table in sync.
// The device talks the same protocol over USB or DIN; Chrome's Web MIDI only ever exposes it via
// USB, so that's the only path this file actually drives, but nothing here assumes USB-only.

export const CMD = {
  IDENTITY_REQUEST: 0x01,
  IDENTITY_REPLY: 0x02,
  GET_GLOBAL: 0x10,
  GET_STATUS: 0x13,
  STATUS_DATA: 0x14,
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
  FW_BEGIN: 0x40,
  FW_DATA: 0x41,
  FW_END: 0x42,
  ACK: 0x7E
};

const MFR_ID = 0x7D;
const DEVICE_ID = 0x4D;
export const ROUTE_NAME_MAX_LEN = 24; // must match ROUTE_NAME_MAX_LEN in the firmware

// Ports, one bit each, same as PORT_* in the firmware. Inputs are bits 0-6, TRS 3-6 are outputs only.
// Routes carry them as inPorts/outPorts. Protocol 1 firmware (1.0-1.3) only has TRS/DIN and USB Device.
export const PORTS = [
  { bit: 0, key: 'trs', label: 'TRS/DIN', short: 'TRS', input: true, sidecar: false },
  { bit: 1, key: 'usb', label: 'USB Device', short: 'USB', input: true, sidecar: false },
  { bit: 2, key: 'host1', label: 'USB Host 1', short: 'Host1', input: true, sidecar: true },
  { bit: 3, key: 'host2', label: 'USB Host 2', short: 'Host2', input: true, sidecar: true },
  { bit: 4, key: 'host3', label: 'USB Host 3', short: 'Host3', input: true, sidecar: true },
  { bit: 5, key: 'host4', label: 'USB Host 4', short: 'Host4', input: true, sidecar: true },
  { bit: 6, key: 'trs2', label: 'TRS 2', short: 'TRS2', input: true, sidecar: true },
  { bit: 7, key: 'trs3', label: 'TRS 3', short: 'TRS3', input: false, sidecar: true },
  { bit: 8, key: 'trs4', label: 'TRS 4', short: 'TRS4', input: false, sidecar: true },
  { bit: 9, key: 'trs5', label: 'TRS 5', short: 'TRS5', input: false, sidecar: true },
  { bit: 10, key: 'trs6', label: 'TRS 6', short: 'TRS6', input: false, sidecar: true }
];
export const PORT_TRS = 1 << 0;
export const PORT_USB = 1 << 1;
export const SIDECAR_INPUT_PORTS = 0x007C;
export const SIDECAR_OUTPUT_PORTS = 0x07FC;

// Protocol 1 routes are 41 raw bytes. Protocol 2 adds inPorts and outPorts (2 bytes each, little
// endian) at the end, used when flags bit6 is set.
const ROUTE_BYTES_V1 = 17 + ROUTE_NAME_MAX_LEN;
const DEFAULT_TIMEOUT_MS = 1500;

// Firmware update over SysEx (FW_BEGIN/FW_DATA/FW_END). Must match FW_CHUNK_BYTES in the firmware.
export const FW_CHUNK_BYTES = 128;
const FW_BEGIN_TIMEOUT_MS = 10000; // first use may have to format the device's update partition
const FW_DATA_TIMEOUT_MS = 3000;   // a chunk can land on a 4 KB flash write
const FW_END_TIMEOUT_MS = 10000;   // device re-reads the whole file to check its MD5
const FW_DATA_ATTEMPTS = 4;

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

// Legacy input value (0 TRS/DIN, 1 USB Device, 2 both) for protocol 1 firmware and old Patchlings.
function legacyInput(inPorts) {
  const trs = !!(inPorts & PORT_TRS), usb = !!(inPorts & PORT_USB);
  return trs && !usb ? 0 : usb && !trs ? 1 : 2;
}

/** Route field object <-> raw wire bytes: 41 for protocol 1, 45 for protocol 2. */
export function routeToBytes(r, protocolVersion = 2) {
  const v2 = protocolVersion >= 2;
  const bytes = [
    (r.enabled ? 0x80 : 0) | (v2 ? 0x40 : 0) | legacyInput(r.inPorts),
    // Full 8-bit low/high bytes, not 7-bit masked — inChLow/inChHigh each carry 8 channel bits
    // (ch1-8, ch9-16) on the firmware side, and pack7 (applied after this array is built) is what
    // actually makes every byte MIDI-safe, so masking to 7 bits here would silently drop channel
    // 8 and channel 16 (bit 7 of each byte) before pack7 ever sees them.
    r.inputChannels & 0xFF,
    (r.inputChannels >> 8) & 0xFF,
    // Full byte, not masked to 7 bits — bit7 (aftertouchMap) needs to survive pack7, which already
    // extracts each raw byte's true 8th bit into a separate MSB byte on the wire.
    r.typeFlags & 0xFF,
    r.noteStart & 0x7F,
    r.noteEnd & 0x7F,
    r.ccStart & 0x7F,
    r.ccEnd & 0x7F,
    r.outPorts & (PORT_TRS | PORT_USB), // legacy outDevice
    r.outputChannels & 0xFF,
    (r.outputChannels >> 8) & 0xFF,
    (r.transpose + 64) & 0x7F,
    // Full byte, not masked to 7 bits — range is 10-200, well past 0x7F, and pack7 already handles
    // any raw byte up to 255 safely.
    r.velocityScale & 0xFF,
    r.ccMapStart & 0x7F,
    r.atMapCC & 0x7F,
    // bit7=channel pressure remapped to CC, bits6-0=destination CC number — packed into one byte
    // since the CC number itself only needs 7 bits, same trick the firmware uses.
    (r.cpMapEnabled ? 0x80 : 0) | (r.cpMapCC & 0x7F),
    // bit0=Mono Retrig (poly->mono, last-note priority), bit1=Round Robin (cycles each new note
    // to a different selected output channel), bit2=System Common, bit3=System Realtime (Transport,
    // everything but Clock), bit4=MIDI Clock (split from Transport — continuous chatter, not a
    // one-off event, so worth gating independently), bit5=Round Robin Random (true random channel
    // pick instead of cycling — only meaningful with bit1 set).
    (r.monoRetrig ? 0x01 : 0) | (r.roundRobin ? 0x02 : 0) | (r.commonEnabled ? 0x04 : 0) |
      (r.realtimeEnabled ? 0x08 : 0) | (r.clockEnabled ? 0x10 : 0) | (r.roundRobinRandom ? 0x20 : 0),
    ...encodeName(r.name)
  ];
  if (v2) bytes.push(r.inPorts & 0xFF, (r.inPorts >> 8) & 0xFF, r.outPorts & 0xFF, (r.outPorts >> 8) & 0xFF);
  return bytes;
}

export function bytesToRoute(b) {
  const hasPorts = (b[0] & 0x40) && b.length >= ROUTE_BYTES_V1 + 4;
  const legacyIn = [PORT_TRS, PORT_USB, PORT_TRS | PORT_USB][b[0] & 0x03] ?? (PORT_TRS | PORT_USB);
  return {
    enabled: !!(b[0] & 0x80),
    inPorts: hasPorts ? b[ROUTE_BYTES_V1] | (b[ROUTE_BYTES_V1 + 1] << 8) : legacyIn,
    outPorts: hasPorts ? b[ROUTE_BYTES_V1 + 2] | (b[ROUTE_BYTES_V1 + 3] << 8) : (b[8] & (PORT_TRS | PORT_USB)),
    inputChannels: b[1] | (b[2] << 8),
    typeFlags: b[3],
    noteStart: b[4],
    noteEnd: b[5],
    ccStart: b[6],
    ccEnd: b[7],
    outputChannels: b[9] | (b[10] << 8),
    transpose: b[11] - 64,
    velocityScale: b[12],
    ccMapStart: b[13],
    atMapCC: b[14],
    cpMapEnabled: !!(b[15] & 0x80),
    cpMapCC: b[15] & 0x7F,
    monoRetrig: !!(b[16] & 0x01),
    roundRobin: !!(b[16] & 0x02),
    commonEnabled: !!(b[16] & 0x04),
    realtimeEnabled: !!(b[16] & 0x08),
    clockEnabled: !!(b[16] & 0x10),
    roundRobinRandom: !!(b[16] & 0x20),
    name: decodeName(b.slice(17, ROUTE_BYTES_V1))
  };
}

export const DATA_TYPE_BITS = {
  note: 0x01, cc: 0x02, programChange: 0x04, pitchBend: 0x08,
  aftertouch: 0x10, pressure: 0x20, sysex: 0x40, aftertouchMap: 0x80
};

export class DeviceLink extends EventTarget {
  constructor() {
    super();
    this.access = null;
    this.input = null;
    this.output = null;
    this.connected = false;
    this.deviceInfo = null;
    this._connecting = false;
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
    this.access.onstatechange = (e) => {
      // Our port going away (unplugged, or rebooting after a firmware update) ends the connection,
      // so the replugged or rebooted device connects fresh instead of being treated as still open.
      if (this.connected && e.port.state === 'disconnected' && (e.port === this.input || e.port === this.output)) {
        this.disconnectManually();
      }
      this._tryAutoConnect();
    };
    await this._tryAutoConnect();
  }

  /** Tries again after a failed or lost connection, asking for MIDI access again if it was never granted. */
  async retryConnect() {
    if (!this.access) { await this.init(); return; }
    await this._tryAutoConnect();
  }

  async _tryAutoConnect() {
    if (this.connected || this._connecting) return;
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
    this._connecting = true;
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
    } finally {
      this._connecting = false;
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

  // One request at a time. A request made while another is waiting queues behind it, so background
  // status checks never collide with Load or Save.
  async _sendAndWait(cmdAndPayload, expectCmd, timeoutMs) {
    while (this._pending) await new Promise((r) => setTimeout(r, 5));
    return new Promise((resolve, reject) => {
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

  /** Sends a mutating command, waits for ACK, retries on timeout/checksum mismatch (once by default). */
  async _sendMutating(cmd, payload, expectedChecksum, timeoutMs = DEFAULT_TIMEOUT_MS, attempts = 2) {
    for (let attempt = 0; attempt < attempts; attempt++) {
      try {
        const ack = await this._sendAndWait([cmd, ...payload], CMD.ACK, timeoutMs);
        const [originalCmd, status, checksum, extraLo, extraHi] = ack;
        const extra = extraLo | (extraHi << 7);
        if (status !== 0) throw new Error('device-error-' + status);
        if (expectedChecksum !== undefined && checksum !== expectedChecksum) throw new Error('checksum-mismatch');
        return { status, checksum, extra };
      } catch (err) {
        if (attempt === attempts - 1) throw err;
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

  get protocolVersion() { return this.deviceInfo?.protocolVersion ?? 1; }

  /**
   * Sidecar state, protocol 2 only: { sidecar, hostPortMask (bit 0 = USB Host 1), drops,
   * restartReason (0 none, 1 hang, 2 USB Host port stalled, 3-5 USB driver faults), restartPort,
   * restartCount }.
   */
  async getStatus() {
    if (this.protocolVersion < 2) return { sidecar: false, hostPortMask: 0, drops: 0, restartReason: 0, restartPort: 0, restartCount: 0 };
    const reply = await this._sendAndWait([CMD.GET_STATUS], CMD.STATUS_DATA, DEFAULT_TIMEOUT_MS);
    return {
      sidecar: reply[0] === 1, hostPortMask: reply[1], drops: reply[2],
      restartReason: reply[3] ?? 0, restartPort: reply[4] ?? 0, restartCount: reply[5] ?? 0
    };
  }

  async getRoute(index) {
    const reply = await this._sendAndWait([CMD.GET_ROUTE, ...write16(index)], CMD.ROUTE_DATA, DEFAULT_TIMEOUT_MS);
    const packed = reply.slice(2);
    return bytesToRoute(unpack7(packed));
  }

  async addRoute(route) {
    const raw = routeToBytes(route, this.protocolVersion);
    const result = await this._sendMutating(CMD.ADD_ROUTE, pack7(raw), checksum7(raw));
    return result.extra; // assigned index
  }

  async setRoute(index, route) {
    const raw = routeToBytes(route, this.protocolVersion);
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

  /**
   * Sends a firmware .bin to the device. Each 128-byte chunk is confirmed by checksum before the
   * next is sent. Nothing is installed until FW_END, where the device checks the size and MD5 of
   * the whole file, then reboots to install it. progressCb(bytesSent, totalBytes).
   */
  async updateFirmware(bin, md5Hex, progressCb) {
    const size = bin.length;
    const md5 = Array.from(md5Hex.toLowerCase(), (c) => c.charCodeAt(0));
    const begin = [size & 0x7F, (size >> 7) & 0x7F, (size >> 14) & 0x7F, ...md5];
    await this._sendMutating(CMD.FW_BEGIN, begin, checksum7(begin), FW_BEGIN_TIMEOUT_MS);
    const chunks = Math.ceil(size / FW_CHUNK_BYTES);
    for (let i = 0; i < chunks; i++) {
      const raw = Array.from(bin.subarray(i * FW_CHUNK_BYTES, (i + 1) * FW_CHUNK_BYTES));
      await this._sendMutating(CMD.FW_DATA, [...write16(i), ...pack7(raw)], checksum7(raw), FW_DATA_TIMEOUT_MS, FW_DATA_ATTEMPTS);
      if (progressCb) progressCb(Math.min(size, (i + 1) * FW_CHUNK_BYTES), size);
    }
    try {
      // Never retried: if the device staged the update but its ACK was lost, a retry would be
      // refused, yet the device still reboots into the new firmware.
      await this._sendMutating(CMD.FW_END, [], undefined, FW_END_TIMEOUT_MS, 1);
    } catch (err) {
      err.fwEndUnconfirmed = true;
      throw err;
    }
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
