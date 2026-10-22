// The Host Edition's report from its last recovery restart (firmware 1.63, host brain 3): what each
// USB Host port was doing, and the last things that happened. Layout in foxwizard_link.h.

const STAGES = ['', 'USB host driver', 'reading the main brain', 'reading DIN', 'reading USB devices',
  'sorting output', 'writing to USB devices', 'writing DIN', 'status to the main brain', 'writing to the main brain', 'lights'];

// Vendor IDs from the USB-IF list, only the ones common in MIDI gear.
const VENDORS = { 0x1235: 'Novation/Focusrite', 0x0582: 'Roland', 0x0499: 'Yamaha', 0x0944: 'Korg', 0x09E8: 'Akai', 0x1C75: 'Arturia', 0x17CC: 'Native Instruments' };

const hex4 = (v) => v.toString(16).toUpperCase().padStart(4, '0');
const u16 = (b, i) => b[i] | (b[i + 1] << 8);
const u32 = (b, i) => (b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | (b[i + 3] << 24)) >>> 0;
const secs = (ms) => `${(ms / 1000).toFixed(1)} s`;

export function deviceName(vid, pid) {
  if (!vid && !pid) return 'unknown device';
  return `${VENDORS[vid] ? VENDORS[vid] + ' ' : ''}${hex4(vid)}:${hex4(pid)}`;
}

/** A USB MIDI packet in words, e.g. "Note On ch 1, 60, 127". */
export function packetText(p) {
  const cin = p[0] & 0x0F;
  if (!p[0] && !p[1] && !p[2] && !p[3]) return 'nothing';
  const raw = p.map(b => b.toString(16).toUpperCase().padStart(2, '0')).join(' ');
  if (cin >= 0x4 && cin <= 0x7) return `SysEx piece (${raw})`;
  const st = p[1] & 0xF0, ch = (p[1] & 0x0F) + 1;
  const names = { 0x80: 'Note Off', 0x90: 'Note On', 0xA0: 'Poly AT', 0xB0: 'CC', 0xC0: 'Program', 0xD0: 'Ch Pressure', 0xE0: 'Pitch Bend' };
  if (names[st]) return `${names[st]} ch ${ch}, ${p[2]}${st === 0xC0 || st === 0xD0 ? '' : ', ' + p[3]} (${raw})`;
  return raw;
}

function eventText(type, port, a) {
  const host = `USB Host ${port}`;
  switch (type) {
    case 1: return a ? `started after a recovery restart (reason ${a})` : 'started';
    case 2: return `${host} connected`;
    case 3: return `${host} disconnected`;
    case 4: return `${host} took nothing for 1 s, its output is being dropped`;
    case 5: return `recovery restart, reason ${a}${port ? ', ' + host : ''}`;
    case 6: return `USB driver fault ${a}`;
    case 7: return 'a device came with all four USB Host ports taken, and waits for one to free up';
    default: return `event ${type}`;
  }
}

/** Parses the report. Returns null when it isn't a report this Patchling understands. */
export function parseHostReport(b) {
  if (!b || b.length < 240 || b[0] !== 1) return null;
  const ports = [];
  for (let p = 0; p < 4; p++) {
    const o = 16 + p * 24;
    ports.push({
      port: p + 1, mounted: !!(b[o] & 1), writable: !!(b[o] & 2), hubPort: (b[o] >> 2) & 7, tier: (b[o] >> 5) & 3,
      vid: u16(b, o + 1), pid: u16(b, o + 3),
      sent: u32(b, o + 5), received: u32(b, o + 9), last: b.slice(o + 13, o + 17), stuck: b.slice(o + 17, o + 21),
      queued: u16(b, o + 21), idleMs: b[o + 23] * 100
    });
  }
  const events = [];
  for (let i = 0; i < 16; i++) {
    const o = 112 + i * 8;
    if (!b[o + 4]) continue;
    events.push({ ms: u32(b, o), type: b[o + 4], port: b[o + 5], a: b[o + 6], b: b[o + 7] });
  }
  return {
    reason: b[1], port: b[2], stage: b[3], uptimeMs: u32(b, 4), maxLoopUs: u16(b, 8), stallDrops: u16(b, 10),
    devices: b[12], restarts: b[13], hostVersion: b[14], jacks: b[15], ports, events
  };
}

/** One line for a message: the stuck port's device and what it wouldn't take. */
export function hostReportSummary(r) {
  if (!r) return '';
  const p = r.ports[r.port - 1];
  if (r.reason === 2 && p) return `${deviceName(p.vid, p.pid)}, stuck on ${packetText(p.stuck)} with ${p.queued} waiting.`;
  return `The loop was ${STAGES[r.stage] || 'somewhere unknown'}.`;
}

/** The whole report as plain text, to read or copy. */
export function hostReportText(r, headline) {
  if (!r) return 'No report from the Host Edition yet. It needs host brain firmware 3 or later.';
  const lines = [headline, ''];
  lines.push(`Up ${secs(r.uptimeMs)} before restarting. The loop was ${STAGES[r.stage] || 'somewhere unknown'}.`);
  lines.push(`Longest loop pass: ${r.maxLoopUs} µs. Packets dropped to stuck ports: ${r.stallDrops}. USB devices: ${r.devices}. Hub jacks: ${r.jacks || 'no hub'}. Recovery restarts: ${r.restarts}. Host brain firmware ${r.hostVersion}.`);
  lines.push('');
  for (const p of r.ports) {
    if (!p.vid && !p.pid && !p.sent && !p.received) { lines.push(`USB Host ${p.port}: empty`); continue; }
    const where = p.tier === 0 ? 'plugged straight in' : (p.tier === 2 ? `behind a second hub, its port ${p.hubPort}` : (p.hubPort === p.port ? '' : `hub port ${p.hubPort}`));
    lines.push(`USB Host ${p.port}: ${deviceName(p.vid, p.pid)}${where ? ` (${where})` : ''}${p.mounted ? '' : ' (disconnected)'}${p.writable ? '' : ', no MIDI out'}`);
    lines.push(`  Took ${p.sent} packets, sent ${p.received}. Last taken: ${packetText(p.last)}.`);
    if (p.queued || p.idleMs) lines.push(`  At restart: ${p.queued} waiting, front one ${packetText(p.stuck)}, nothing taken for ${secs(p.idleMs)}.`);
  }
  if (r.events.length) {
    lines.push('', 'Recent events (time since it started):');
    for (const e of r.events) lines.push(`  ${secs(e.ms).padStart(9)}  ${eventText(e.type, e.port, e.a)}`);
  }
  return lines.join('\n');
}
