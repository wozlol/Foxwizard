import { mapToBytes, bytesToMap, lpActionsToBytes, bytesToLpActions, defaultLpActions } from './mapper.js';
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
  LEARN: 0x15,
  LEARN_DATA: 0x16,
  GET_FEATURE: 0x17,
  FEATURE_DATA: 0x18,
  SET_FEATURE: 0x19,
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
  PLAY_NOTE: 0x33,
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
  { bit: 6, key: 'trs2', label: 'TRS 2', short: 'TRS2', input: true, sidecar: true, trs: true },
  { bit: 7, key: 'trs3', label: 'TRS 3', short: 'TRS3', input: false, sidecar: true, trs: true },
  { bit: 8, key: 'trs4', label: 'TRS 4', short: 'TRS4', input: false, sidecar: true, trs: true },
  { bit: 9, key: 'trs5', label: 'TRS 5', short: 'TRS5', input: false, sidecar: true, trs: true },
  { bit: 10, key: 'trs6', label: 'TRS 6', short: 'TRS6', input: false, sidecar: true, trs: true }
];
export const PORT_TRS = 1 << 0;
export const PORT_USB = 1 << 1;
export const SIDECAR_INPUT_PORTS = 0x007C;
export const SIDECAR_OUTPUT_PORTS = 0x07FC;
export const SIDECAR_TRS_PORTS = 0x07C0; // TRS 2-6, wired only when a TRS board is added on top of the bare sidecar module

// Protocol 1 routes are 41 raw bytes. Protocol 2 adds inPorts and outPorts (2 bytes each, little
// endian) at the end, used when flags bit6 is set. Protocol 4 adds features (2 bytes, FEATURE_*
// switches), arpSlot, and 5 spare bytes.
const ROUTE_BYTES_V1 = 17 + ROUTE_NAME_MAX_LEN;
const ROUTE_BYTES_V2 = ROUTE_BYTES_V1 + 4;
const ROUTE_BYTES_V4 = ROUTE_BYTES_V2 + 8;

// Route feature switches (route.features), same as FEATURE_* in the firmware.
export const FEATURE = { NOTE_TO_CC: 1 << 0, LAUNCHPAD: 1 << 1, SCALE: 1 << 2, AUTO_CHORD: 1 << 3, ECHO: 1 << 4, STUTTER: 1 << 5, CC_MAP: 1 << 6 };

// Feature blocks, shared by the routes that switch a feature on. Same as FEATURE_TYPE_* in the firmware.
export const FEATURE_TYPE = { NOTE_TO_CC: 1, LAUNCHPAD: 2, SCALE: 3, ARP_1: 4, ECHO: 8, STUTTER: 9, LOOPER: 10, NOTE_MAP: 11, CC_MAP: 12, LP_ACTIONS: 13 }; // arp slots 1-4 are 4-7

// Arp, firmware 1.13. Same order as the firmware's ARP_* modes and fw::MusicalDivision.
export const ARP_SLOTS = 4;
export const ARP_MODES = ['Up', 'Down', 'Up-Down 1', 'Up-Down 2', 'Drum Roll/Chord Pulse', 'Random',
  'Up 4-Step', 'Rhythm', 'Ostinato', 'Octave Walk', 'Fifth', 'Bass+Chord', 'Chord+Run',
  'As Played', 'Converge', 'Diverge', 'Thumb', 'Pinky']; // 1.20
// The order modes are listed in: the note orders together, then ARPnMIDI's patterns.
export const ARP_MODE_ORDER = [0, 1, 2, 3, 4, 5, 13, 14, 15, 16, 17, 6, 7, 8, 9, 10, 11, 12];
export const ARP_SQUARES = 7; // division table squares besides the default
export const DIVISIONS = ['1/1', '1/2D', '1/2', '1/4D', '1/2T', '1/4', '1/8D', '1/4T', '1/8', '1/16D', '1/8T', '1/16', '1/32D', '1/16T', '1/32', '1/64D', '1/32T', '1/64', '1/64T'];
export const DEFAULT_DIVISION = 11; // 1/16
export function defaultArp() {
  return {
    mode: 0, division: DEFAULT_DIVISION, octaves: 1, gate: 50, velocity: 0, aftertouch: false, bypassChannel: 0, bassChannel: 0, defaultTrigger: noTrigger(), holdType: 0, holdChannel: 16, holdNumber: 64,
    squares: Array.from({ length: ARP_SQUARES }, () => null) // { division, triggerType 0 none 1 note 2 CC, triggerChannel 0-15 16 any, triggerNumber }
  };
}
// 48 bytes: mode, division + 1, octaves, gate %, velocity (0 = played), holdType (0 none, 2 CC), holdChannel,
// holdNumber, aftertouch velocity, bypass channel (0 off, 1-16), bass channel (0 off, 1-16), default trigger (3,
// turns the arp off and on), 2 spare, then 7 division squares of (division + 1, 0 = unused, triggerType,
// triggerChannel, triggerNumber), 4 spare.
export function arpToBytes(a) {
  const b = [a.mode, a.division + 1, a.octaves, a.gate, a.aftertouch ? 0 : a.velocity, a.holdType, a.holdChannel, a.holdNumber, a.aftertouch ? 1 : 0, a.bypassChannel || 0, a.bassChannel || 0, ...triggerToBytes(a.defaultTrigger), a.followsLiveChannel ? 1 : 0, 0];
  for (let k = 0; k < ARP_SQUARES; k++) {
    const sq = a.squares?.[k];
    b.push(...(sq ? [sq.division + 1, sq.triggerType, sq.triggerChannel, sq.triggerNumber] : [0, 0, 0, 0]));
  }
  return b.concat([0, 0, 0, 0]);
}
// A division table's default square trigger (1.24), 3 bytes: type (0 none, 1 note, 2 CC), channel (16 any), number.
export function noTrigger() { return { triggerType: 0, triggerChannel: 16, triggerNumber: 0 }; }
function triggerToBytes(t) { return t ? [t.triggerType, t.triggerChannel, t.triggerNumber] : [0, 16, 0]; }
function bytesToTrigger(b, o) {
  return { triggerType: b[o] <= 2 ? b[o] : 0, triggerChannel: b[o + 1] <= 16 ? b[o + 1] : 16, triggerNumber: (b[o + 2] || 0) & 0x7F };
}

// Division squares, 4 bytes each: division + 1 (0 = unused), triggerType, triggerChannel, triggerNumber.
function squaresToBytes(squares) {
  const b = [];
  for (let k = 0; k < ARP_SQUARES; k++) {
    const sq = squares?.[k];
    b.push(...(sq ? [sq.division + 1, sq.triggerType, sq.triggerChannel, sq.triggerNumber] : [0, 0, 0, 0]));
  }
  return b;
}
function bytesToSquares(b, offset) {
  return Array.from({ length: ARP_SQUARES }, (_, k) => {
    const o = offset + k * 4;
    if (!(b[o] >= 1 && b[o] <= DIVISIONS.length)) return null;
    return { division: b[o] - 1, triggerType: b[o + 1] <= 2 ? b[o + 1] : 0, triggerChannel: b[o + 2] <= 16 ? b[o + 2] : 16, triggerNumber: (b[o + 3] || 0) & 0x7F };
  });
}

// Echo, firmware 1.23. 48 bytes: division + 1 (0 = 1/8), repeats (1-32, 0 = 4), level % (1-100, 0 = 70),
// 13 spare, 7 division squares whose triggers latch, 4 spare.
export function defaultEcho() {
  return { division: 8, repeats: 4, level: 70, defaultTrigger: noTrigger(), squares: Array.from({ length: ARP_SQUARES }, () => null) };
}
// Default trigger (bytes 3-5) turns Echo off and on.
export function echoToBytes(e) {
  return [e.division + 1, e.repeats, e.level, ...triggerToBytes(e.defaultTrigger), ...new Array(10).fill(0), ...squaresToBytes(e.squares), 0, 0, 0, 0];
}
export function bytesToEcho(b) {
  const d = defaultEcho();
  return {
    division: b[0] >= 1 && b[0] <= DIVISIONS.length ? b[0] - 1 : d.division,
    repeats: b[1] >= 1 && b[1] <= 32 ? b[1] : d.repeats,
    level: b[2] >= 1 && b[2] <= 100 ? b[2] : d.level,
    defaultTrigger: bytesToTrigger(b, 3),
    squares: bytesToSquares(b, 16)
  };
}

// Looper (the button's Looper mode), firmware 1.27. 48 bytes: bars (0 Free, 1-16), quantize (0 off, else
// division + 1), auto overdub (1.29), record inputs (0 outputs, 1 inputs), double tap clears (1.29, 0 =
// holding the button clears and a double tap stops, 1 the other way), play/arm trigger (3), stop/clear
// trigger (3), 37 spare.
export function defaultLooper() {
  return { bars: 0, quantize: -1, autoOverdub: false, recordInputs: false, holdClears: true };
}
export function looperToBytes(l) {
  return [l.bars, l.quantize + 1, l.autoOverdub ? 1 : 0, l.recordInputs ? 1 : 0, l.holdClears ? 0 : 1,
    ...new Array(43).fill(0)];
}
export function bytesToLooper(b) {
  return {
    bars: b[0] <= 16 ? b[0] : 0,
    quantize: b[1] >= 1 && b[1] <= DIVISIONS.length ? b[1] - 1 : -1,
    autoOverdub: b[2] === 1,
    recordInputs: b[3] === 1,
    holdClears: b[4] !== 1
  };
}

// Stutter, firmware 1.24. 48 bytes: default division + 1 (0 = 1/8), runout bars (1-16, 0 = 16), default
// trigger (3), 11 spare, 7 division squares, 4 spare. Every square is momentary, the default too.
export function defaultStutter() {
  return { division: 8, maxBars: 16, defaultTrigger: noTrigger(), squares: Array.from({ length: ARP_SQUARES }, () => null) };
}
export function stutterToBytes(st) {
  return [st.division + 1, st.maxBars, ...triggerToBytes(st.defaultTrigger), ...new Array(11).fill(0), ...squaresToBytes(st.squares), 0, 0, 0, 0];
}
export function bytesToStutter(b) {
  const d = defaultStutter();
  return {
    division: b[0] >= 1 && b[0] <= DIVISIONS.length ? b[0] - 1 : d.division,
    maxBars: b[1] >= 1 && b[1] <= 16 ? b[1] : d.maxBars,
    defaultTrigger: bytesToTrigger(b, 2),
    squares: bytesToSquares(b, 16)
  };
}

export function bytesToArp(b) {
  const d = defaultArp();
  return {
    mode: b[0] < ARP_MODES.length ? b[0] : d.mode,
    division: b[1] >= 1 && b[1] <= DIVISIONS.length ? b[1] - 1 : d.division,
    octaves: b[2] >= 1 && b[2] <= 4 ? b[2] : d.octaves,
    gate: b[3] >= 5 && b[3] <= 100 ? b[3] : d.gate,
    velocity: b[4] || 0,
    holdType: b[5] === 2 ? 2 : 0,
    holdChannel: b[6] <= 16 ? b[6] : 16,
    holdNumber: (b[7] || 0) & 0x7F,
    aftertouch: b[8] === 1,
    bypassChannel: b[9] <= 16 ? b[9] : 0,
    bassChannel: b[10] <= 16 ? b[10] : 0,
    defaultTrigger: bytesToTrigger(b, 11),
    followsLiveChannel: b[14] === 1,
    squares: Array.from({ length: ARP_SQUARES }, (_, k) => {
      const o = 16 + k * 4;
      if (!(b[o] >= 1 && b[o] <= DIVISIONS.length)) return null;
      return { division: b[o] - 1, triggerType: b[o + 1] <= 2 ? b[o + 1] : 0, triggerChannel: b[o + 2] <= 16 ? b[o + 2] : 16, triggerNumber: (b[o + 3] || 0) & 0x7F };
    })
  };
}

// Same order and steps as SCALES in the firmware. steps are semitones above the root.
export const SCALES = [
  { name: 'Major', steps: [0, 2, 4, 5, 7, 9, 11] },
  { name: 'Minor', steps: [0, 2, 3, 5, 7, 8, 10] },
  { name: 'Harmonic Major', steps: [0, 2, 4, 5, 7, 8, 11] },
  { name: 'Harmonic Minor', steps: [0, 2, 3, 5, 7, 8, 11] },
  { name: 'Melodic Major', steps: [0, 2, 4, 5, 7, 8, 10] },
  { name: 'Melodic Minor', steps: [0, 2, 3, 5, 7, 9, 11] },
  { name: 'Major Blues', steps: [0, 2, 3, 4, 7, 9] },
  { name: 'Minor Blues', steps: [0, 3, 5, 6, 7, 10] },
  { name: 'Major Pentatonic', steps: [0, 2, 4, 7, 9] },
  { name: 'Minor Pentatonic', steps: [0, 3, 5, 7, 10] },
  { name: 'Dorian', steps: [0, 2, 3, 5, 7, 9, 10] },
  { name: 'Mixolydian', steps: [0, 2, 4, 5, 7, 9, 10] }
];
export const LAYOUTS = ['Next Note Up', 'White Keys', 'Drum Pad 5ths', 'Drum Pad 4ths', 'Drum Pad 3rds'];

// Scale block: root (0-11), scaleType, layout (unused since 1.14) and chordMask (unused since 1.15), each route has its own, 3 spare.
export function defaultScale() {
  return { root: 0, scaleType: 0 };
}
export function scaleToBytes(sc) {
  return [sc.root, sc.scaleType, 0, 0, 0, 0, 0, 0];
}
export function bytesToScale(b) {
  return { root: (b[0] || 0) % 12, scaleType: b[1] < SCALES.length ? b[1] : 0 };
}

const DEFAULT_TIMEOUT_MS = 1500;

// Firmware update over SysEx (FW_BEGIN/FW_DATA/FW_END). Must match FW_CHUNK_BYTES in the firmware.
export const FW_CHUNK_BYTES = 128;
const FW_BEGIN_TIMEOUT_MS = 10000; // first use may have to format the device's update partition
const FW_DATA_TIMEOUT_MS = 3000;   // a chunk can land on a 4 KB flash write
const FW_END_TIMEOUT_MS = 10000;   // device re-reads the whole file to check its MD5
const FW_DATA_ATTEMPTS = 4;
const LEARN_TIMEOUT_MS = 10500; // the device listens for 10 s

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
  if (protocolVersion >= 4) bytes.push((r.features || 0) & 0xFF, ((r.features || 0) >> 8) & 0xFF, r.arpSlot || 0, r.scaleLayout || 0, (r.chordMask || 0) & 0xFF, ((r.chordMask || 0) >> 8) & 0xFF, r.lpOctaveDown ? 1 : 0, 0);
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
    name: decodeName(b.slice(17, ROUTE_BYTES_V1)),
    features: b.length >= ROUTE_BYTES_V4 ? b[ROUTE_BYTES_V2] | (b[ROUTE_BYTES_V2 + 1] << 8) : 0,
    arpSlot: b.length >= ROUTE_BYTES_V4 ? b[ROUTE_BYTES_V2 + 2] : 0,
    scaleLayout: b.length >= ROUTE_BYTES_V4 && b[ROUTE_BYTES_V2 + 3] < LAYOUTS.length ? b[ROUTE_BYTES_V2 + 3] : 0,
    chordMask: b.length >= ROUTE_BYTES_V4 ? (b[ROUTE_BYTES_V2 + 4] | (b[ROUTE_BYTES_V2 + 5] << 8)) & 0xFFF : 0,
    lpOctaveDown: b.length >= ROUTE_BYTES_V4 && (b[ROUTE_BYTES_V2 + 6] & 0x01) === 1
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
    // Notes the FoxWizard sends to the computer, on any channel, for Patchling's live displays.
    const type = data[0] & 0xF0;
    if ((type === 0x90 || type === 0x80) && data.length >= 3) {
      this.emit('note', { note: data[1], on: type === 0x90 && data[2] > 0, channel: data[0] & 0x0F });
      return;
    }
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

  // Protocol 3 adds the global clock settings after the original 6 bytes. Fields an older device
  // doesn't send are left out, so the caller's defaults stand.
  async getGlobal() {
    const reply = await this._sendAndWait([CMD.GET_GLOBAL], CMD.GLOBAL_DATA, DEFAULT_TIMEOUT_MS);
    const g = {
      mode: reply[0],
      buttonAction: reply[1],
      buttonParamA: reply[2],
      buttonParamB: reply[3],
      buttonToggleMomentary: reply[4],
      ledBrightnessStep: reply[5]
    };
    if (reply.length >= 15) {
      Object.assign(g, {
        clockSource: reply[6],
        bpm: reply[7] | (reply[8] << 7),
        beatsPerBar: reply[9],
        beatUnit: reply[10],
        clockOut: reply[11],
        tapType: reply[12],
        tapChannel: reply[13],
        tapNumber: reply[14]
      });
    }
    return g;
  }

  async setGlobal(g) {
    const payload = [g.mode, g.buttonAction, g.buttonParamA, g.buttonParamB, g.buttonToggleMomentary, g.ledBrightnessStep];
    if (this.protocolVersion >= 3) {
      payload.push(g.clockSource, g.bpm & 0x7F, (g.bpm >> 7) & 0x7F, g.beatsPerBar, g.beatUnit,
        g.clockOut, g.tapType, g.tapChannel, g.tapNumber);
    }
    return this._sendMutating(CMD.SET_GLOBAL, payload, checksum7(payload));
  }

  /** Firmware 1.34: plays a note on every port and channel, Patchling's piano. No reply. */
  playNote(on, note, velocity = 100) {
    if (!this.connected) return;
    this._sendRaw(CMD.PLAY_NOTE, [on ? 1 : 0, note & 0x7F, velocity & 0x7F]);
  }

  /** Protocol 3: waits up to 10 s for the next note or CC the device hears. { type 1 note / 2 CC, channel, number, port bit }. */
  async learn() {
    const r = await this._sendAndWait([CMD.LEARN], CMD.LEARN_DATA, LEARN_TIMEOUT_MS);
    return { type: r[0], channel: r[1], number: r[2], port: r[3] };
  }

  async getRouteCount() {
    const reply = await this._sendAndWait([CMD.GET_ROUTE_COUNT], CMD.ROUTE_COUNT, DEFAULT_TIMEOUT_MS);
    return read16(reply, 0);
  }

  get protocolVersion() { return this.deviceInfo?.protocolVersion ?? 1; }

  /**
   * Sidecar state, protocol 2 only: { sidecar, hostPortMask (bit 0 = USB Host 1), drops,
   * restartReason (0 none, 1 hang, 2 USB Host port stalled, 3-5 USB driver faults), restartPort,
   * restartCount, hasTrs (true when a TRS board is added on top of the bare sidecar module) }.
   */
  async getStatus() {
    if (this.protocolVersion < 2) return { sidecar: false, hostPortMask: 0, drops: 0, restartReason: 0, restartPort: 0, restartCount: 0, bpm: null, clockStatus: 0, hasTrs: true };
    const reply = await this._sendAndWait([CMD.GET_STATUS], CMD.STATUS_DATA, DEFAULT_TIMEOUT_MS);
    return {
      sidecar: reply[0] === 1, hostPortMask: reply[1], drops: reply[2],
      restartReason: reply[3] ?? 0, restartPort: reply[4] ?? 0, restartCount: reply[5] ?? 0,
      // Protocol 3: current BPM, and 0 clock unused, 1 master, 2 following, 3 following with no clock.
      bpm: reply.length >= 9 ? reply[6] | (reply[7] << 7) : null,
      clockStatus: reply[8] ?? 0,
      // Firmware before 1.44 never sends byte 9: assume TRS 2-6 exist (every deployed sidecar so far).
      hasTrs: reply[9] === undefined ? true : reply[9] === 1
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
  /** Protocol 4: a feature block's raw bytes. */
  async getFeature(type) {
    const reply = await this._sendAndWait([CMD.GET_FEATURE, type], CMD.FEATURE_DATA, DEFAULT_TIMEOUT_MS);
    return unpack7(reply.slice(1));
  }

  async setFeature(type, bytes) {
    return this._sendMutating(CMD.SET_FEATURE, [type, ...pack7(bytes)], checksum7(bytes));
  }

  async loadAll(progressCb) {
    const global = await this.getGlobal();
    const count = await this.getRouteCount();
    const routes = [];
    for (let i = 0; i < count; i++) {
      routes.push(await this.getRoute(i));
      if (progressCb) progressCb(i + 1, count);
    }
    const features = {};
    if (this.protocolVersion >= 4) {
      // The mapper's lists arrived in 1.40. Older firmware has Note to CC and Launchpad buttons.
      features.noteMap = bytesToMap(await this.getFeature(FEATURE_TYPE.NOTE_MAP));
      features.ccMap = bytesToMap(await this.getFeature(FEATURE_TYPE.CC_MAP));
      features.lpActions = bytesToLpActions(await this.getFeature(FEATURE_TYPE.LP_ACTIONS));
      features.scale = bytesToScale(await this.getFeature(FEATURE_TYPE.SCALE));
      features.arps = [];
      for (let s = 0; s < ARP_SLOTS; s++) features.arps.push(bytesToArp(await this.getFeature(FEATURE_TYPE.ARP_1 + s)));
      features.echo = bytesToEcho(await this.getFeature(FEATURE_TYPE.ECHO));
      features.stutter = bytesToStutter(await this.getFeature(FEATURE_TYPE.STUTTER));
      features.looper = bytesToLooper(await this.getFeature(FEATURE_TYPE.LOOPER));
    }
    return { global, routes, features };
  }

  /** Pushes global settings + a full route list to the device, replacing whatever is there. */
  async saveAll(global, routes, features, progressCb) {
    await this.clearAllRoutes();
    for (let i = 0; i < routes.length; i++) {
      await this.addRoute(routes[i]);
      if (progressCb) progressCb(i + 1, routes.length);
    }
    if (this.protocolVersion >= 4) {
      await this.setFeature(FEATURE_TYPE.NOTE_MAP, mapToBytes(features.noteMap || []));
      await this.setFeature(FEATURE_TYPE.CC_MAP, mapToBytes(features.ccMap || []));
      await this.setFeature(FEATURE_TYPE.LP_ACTIONS, lpActionsToBytes(features.lpActions || defaultLpActions()));
      await this.setFeature(FEATURE_TYPE.SCALE, scaleToBytes(features.scale || defaultScale()));
      for (let s = 0; s < ARP_SLOTS; s++) {
        await this.setFeature(FEATURE_TYPE.ARP_1 + s, arpToBytes(features.arps?.[s] || defaultArp()));
      }
      await this.setFeature(FEATURE_TYPE.ECHO, echoToBytes(features.echo || defaultEcho()));
      await this.setFeature(FEATURE_TYPE.STUTTER, stutterToBytes(features.stutter || defaultStutter()));
      await this.setFeature(FEATURE_TYPE.LOOPER, looperToBytes(features.looper || defaultLooper()));
    }
    await this.setGlobal(global);
    await this.commit();
  }
}
