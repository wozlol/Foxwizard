// A preset slot on the unit is one file, the settings image the unit builds when it saves (serializeSettings in the firmware):
//   4 bytes magic (little-endian 0x464F5832, "FOX2"), the global block size, the route size,
//   the global block, a 2-byte route count, the routes, then "FX" and blocks of (type, size, data) ending with a 0 type byte.
// The route and block bytes are the same raw bytes the SysEx messages carry, so this file only has to cut the image apart and
// hand the pieces to the decoders midi.js already has. Firmware 2.7 (protocol 11) hands a slot's image over without switching to it.
import {
  FEATURE_TYPE, ARP_SLOTS, ROUTE_BYTES_V4, routeToBytes, bytesToRoute, rrOrderToBytes, applyRrOrder,
  defaultArp, arpToBytes, bytesToArp, defaultEcho, echoToBytes, bytesToEcho, defaultStutter, stutterToBytes, bytesToStutter,
  defaultLooper, looperToBytes, bytesToLooper, defaultPlock, plockToBytes, bytesToPlock, defaultScale, scaleToBytes, bytesToScale
} from './midi.js';
import { mapToBytes, bytesToMap, lpActionsToBytes, bytesToLpActions, defaultLpActions } from './mapper.js';

const MAGIC_V1 = 0x464F5831; // firmware 1.0, a 6-byte global block and 41-byte routes, no size bytes
const MAGIC_V2 = 0x464F5832;
export const SLOT_IMAGE_MAX = 4096; // the unit's settings image area, EEPROM_SIZE in the firmware
const GLOBAL_BYTES = 15;
const FEATURE_MARKER = [0x46, 0x58]; // "FX"

function padTo(bytes, length) {
  const out = Array.from(bytes);
  while (out.length < length) out.push(0);
  return out;
}

/** A slot's image to what Patchling edits: { global, routes, noteMap, ccMap, lpActions, scale, arps, echo, stutter, looper, plock }. Throws when it is not an image. */
export function imageToPreset(image) {
  const b = image;
  const u16 = (o) => (b[o] | (b[o + 1] << 8));
  const magic = ((b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)) >>> 0);
  let globalLen = 6, routeLen = 41, addr = 4;
  if (magic === MAGIC_V2) { globalLen = b[4]; routeLen = b[5]; addr = 6; }
  else if (magic !== MAGIC_V1) throw new Error('not-a-preset-image');
  if (!globalLen || !routeLen) throw new Error('not-a-preset-image');

  const g = padTo(b.slice(addr, addr + Math.min(globalLen, GLOBAL_BYTES)), GLOBAL_BYTES);
  addr += globalLen;
  const global = {
    mode: g[0], buttonAction: g[1], buttonParamA: g[2], buttonParamB: g[3], buttonToggleMomentary: g[4], ledBrightnessStep: g[5],
    clockSource: g[6], bpm: g[7] | (g[8] << 8), beatsPerBar: g[9], beatUnit: g[10], clockOut: g[11], tapType: g[12], tapChannel: g[13], tapNumber: g[14]
  };

  const stored = u16(addr);
  addr += 2;
  const count = Math.min(stored, Math.floor((b.length - addr) / routeLen));
  const routes = [];
  for (let i = 0; i < count; i++) {
    // A record from older firmware is shorter. The unit fills the rest with zeros, so this does too.
    routes.push(bytesToRoute(padTo(b.slice(addr, addr + Math.min(routeLen, ROUTE_BYTES_V4)), ROUTE_BYTES_V4)));
    addr += routeLen;
  }

  const blocks = {};
  if (addr + 3 <= b.length && b[addr] === FEATURE_MARKER[0] && b[addr + 1] === FEATURE_MARKER[1]) {
    addr += 2;
    while (addr + 2 <= b.length) {
      const type = b[addr], size = b[addr + 1];
      if (type === 0 || addr + 2 + size > b.length) break;
      blocks[type] = Array.from(b.slice(addr + 2, addr + 2 + size));
      addr += 2 + size;
    }
  }
  const block = (type, template) => (blocks[type] ? padTo(blocks[type], template.length) : null);

  const noteMap = block(FEATURE_TYPE.NOTE_MAP, mapToBytes([]));
  const ccMap = block(FEATURE_TYPE.CC_MAP, mapToBytes([]));
  const lp = block(FEATURE_TYPE.LP_ACTIONS, lpActionsToBytes(defaultLpActions()));
  const scale = block(FEATURE_TYPE.SCALE, scaleToBytes(defaultScale()));
  const echo = block(FEATURE_TYPE.ECHO, echoToBytes(defaultEcho()));
  const stutter = block(FEATURE_TYPE.STUTTER, stutterToBytes(defaultStutter()));
  const looper = block(FEATURE_TYPE.LOOPER, looperToBytes(defaultLooper()));
  const plock = block(FEATURE_TYPE.PLOCK, plockToBytes(defaultPlock()));
  const arps = [];
  for (let s = 0; s < ARP_SLOTS; s++) {
    const a = block(FEATURE_TYPE.ARP_1 + s, arpToBytes(defaultArp()));
    arps.push(a ? bytesToArp(a) : defaultArp());
  }
  if (blocks[FEATURE_TYPE.RR_ORDER]) applyRrOrder(routes, blocks[FEATURE_TYPE.RR_ORDER]);

  return {
    global, routes,
    noteMap: noteMap ? bytesToMap(noteMap) : [],
    ccMap: ccMap ? bytesToMap(ccMap) : [],
    lpActions: lp ? bytesToLpActions(lp) : defaultLpActions(),
    scale: scale ? bytesToScale(scale) : defaultScale(),
    arps,
    echo: echo ? bytesToEcho(echo) : defaultEcho(),
    stutter: stutter ? bytesToStutter(stutter) : defaultStutter(),
    looper: looper ? bytesToLooper(looper) : defaultLooper(),
    plock: plock ? bytesToPlock(plock) : defaultPlock()
  };
}

/** What Patchling edits to a slot's image, the layout the firmware writes. Throws when it does not fit the unit's 4,096 bytes. */
export function presetToImage(p) {
  const g = p.global;
  const out = [0x32, 0x58, 0x4F, 0x46, GLOBAL_BYTES, ROUTE_BYTES_V4];
  const bpm = g.bpm ?? 0;
  out.push(g.mode ?? 0, g.buttonAction ?? 0, g.buttonParamA ?? 0, g.buttonParamB ?? 0, g.buttonToggleMomentary ?? 0, g.ledBrightnessStep ?? 10,
    g.clockSource ?? 0, bpm & 0xFF, (bpm >> 8) & 0xFF, g.beatsPerBar ?? 0, g.beatUnit ?? 0, g.clockOut ?? 0, g.tapType ?? 0, g.tapChannel ?? 0, g.tapNumber ?? 0);
  out.push(p.routes.length & 0xFF, (p.routes.length >> 8) & 0xFF);
  for (const r of p.routes) out.push(...routeToBytes(r, 4));
  out.push(...FEATURE_MARKER);
  const add = (type, bytes) => {
    if (bytes.length > 255) throw new Error('block-too-big');
    out.push(type, bytes.length, ...bytes);
  };
  // The firmware's own order, by type number. Types 1 and 2 are not stored, and the order block is left out when no route has a custom order.
  add(FEATURE_TYPE.SCALE, scaleToBytes(p.scale || defaultScale()));
  for (let s = 0; s < ARP_SLOTS; s++) add(FEATURE_TYPE.ARP_1 + s, arpToBytes(p.arps?.[s] || defaultArp()));
  add(FEATURE_TYPE.ECHO, echoToBytes(p.echo || defaultEcho()));
  add(FEATURE_TYPE.STUTTER, stutterToBytes(p.stutter || defaultStutter()));
  add(FEATURE_TYPE.LOOPER, looperToBytes(p.looper || defaultLooper()));
  add(FEATURE_TYPE.NOTE_MAP, mapToBytes(p.noteMap || []));
  add(FEATURE_TYPE.CC_MAP, mapToBytes(p.ccMap || []));
  add(FEATURE_TYPE.LP_ACTIONS, lpActionsToBytes(p.lpActions || defaultLpActions()));
  add(FEATURE_TYPE.PLOCK, plockToBytes(p.plock || defaultPlock()));
  const order = rrOrderToBytes(p.routes);
  if (order.bytes.length) add(FEATURE_TYPE.RR_ORDER, order.bytes);
  out.push(0);
  if (out.length > SLOT_IMAGE_MAX) throw new Error('image-too-big');
  return Uint8Array.from(out);
}
