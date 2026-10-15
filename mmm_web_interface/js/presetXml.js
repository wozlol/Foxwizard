// Human-readable XML preset format for FoxWizard Patchling.
// A preset captures mode, button config, LED brightness, and the full route list — everything
// the device's SysEx protocol can set. Numbers are kept human (channel lists as "1,2,4-6", device
// names as words) rather than raw bitmasks/enums, so the file is meant to be hand-editable.

import { DATA_TYPE_BITS, PORTS, PORT_TRS, PORT_USB, FEATURE, NOTE_CC_ENTRIES, LAUNCHPAD_BUTTONS, defaultLaunchpadButtons, SCALES, LAYOUTS, defaultScale, ARP_SLOTS, ARP_MODES, ARP_SQUARES, DIVISIONS, defaultArp } from './midi.js';

const NOTE_CC_MODE_NAMES = ['momentary', 'velocity', 'toggle'];

const MODE_NAMES = ['usbAdaptor', 'router'];
const BUTTON_ACTION_NAMES = ['panic', 'pitchUp', 'pitchDown', 'mod', 'ccMomentary', 'ccToggle', 'octaveUp', 'octaveDown', 'noteMomentary', 'noteToggle', 'tapTempo'];

// buttonParamA means something different per action (ramp speed, cc number, or note number) — use
// a matching attribute name in the XML rather than a generic one so a hand-edited file reads right.
function buttonValueAttrName(action) {
  if ([1, 2, 3].includes(action)) return 'speed';
  if ([4, 5].includes(action)) return 'cc';
  if ([8, 9].includes(action)) return 'note';
  return null;
}
// Older presets (and older Patchlings) use device="trs|usb|both|none". New presets also write
// ports="trs,usb,host1,...", which wins when present.
const IN_DEVICE_NAMES = ['trs', 'usb', 'both'];
const OUT_DEVICE_NAMES = ['none', 'trs', 'usb', 'both'];
const LEGACY_DEVICE_PORTS = { none: 0, trs: PORT_TRS, usb: PORT_USB, both: PORT_TRS | PORT_USB };

function portsToString(mask) {
  return PORTS.filter((p) => mask & (1 << p.bit)).map((p) => p.key).join(',');
}

function stringToPorts(text) {
  let mask = 0;
  for (const key of text.split(',').map((k) => k.trim().toLowerCase())) {
    const port = PORTS.find((p) => p.key === key);
    if (port) mask |= 1 << port.bit;
  }
  return mask;
}

function legacyDeviceName(mask, names) {
  const both = mask & PORT_TRS && mask & PORT_USB;
  return both ? 'both' : mask & PORT_TRS ? 'trs' : mask & PORT_USB ? 'usb' : names.includes('none') ? 'none' : 'both';
}

function readPorts(el, fallbackDevice) {
  if (el?.hasAttribute('ports')) return stringToPorts(el.getAttribute('ports'));
  return LEGACY_DEVICE_PORTS[el?.getAttribute('device') || fallbackDevice] ?? LEGACY_DEVICE_PORTS[fallbackDevice];
}

// "1,3,5" (positions from 1) to a chord mask.
function parseChord(text) {
  let mask = 0;
  for (const p of (text || '').split(',')) {
    const k = parseInt(p, 10) - 1;
    if (k >= 0 && k < 12) mask |= 1 << k;
  }
  return mask;
}

export function channelsToRangeString(mask) {
  const set = [];
  for (let c = 0; c < 16; c++) if (mask & (1 << c)) set.push(c + 1);
  if (set.length === 0) return '';
  if (set.length === 16) return 'all';
  const parts = [];
  let start = set[0], prev = set[0];
  for (let i = 1; i <= set.length; i++) {
    const cur = set[i];
    if (cur === prev + 1) { prev = cur; continue; }
    parts.push(start === prev ? `${start}` : `${start}-${prev}`);
    start = prev = cur;
  }
  return parts.join(',');
}

export function rangeStringToChannels(str) {
  if (!str) return 0;
  if (str.trim().toLowerCase() === 'all') return 0xFFFF;
  let mask = 0;
  for (const part of str.split(',')) {
    const p = part.trim();
    if (!p) continue;
    if (p.includes('-')) {
      const [a, b] = p.split('-').map((n) => parseInt(n, 10));
      for (let c = a; c <= b; c++) mask |= (1 << (c - 1));
    } else {
      mask |= (1 << (parseInt(p, 10) - 1));
    }
  }
  return mask;
}

function esc(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

export function serializePreset(state) {
  const g = state.global;
  const lines = [];
  lines.push('<?xml version="1.0" encoding="UTF-8"?>');
  lines.push(`<foxwizardPatchlingPreset version="1" device="FoxWizard Mini MIDI Multitool">`);
  lines.push(`  <name>${esc(state.name || 'Untitled Preset')}</name>`);
  lines.push(`  <mode>${MODE_NAMES[g.mode] || 'usbAdaptor'}</mode>`);
  lines.push(`  <ledBrightness>${g.ledBrightnessStep * 10}</ledBrightness>`);
  const clockPort = PORTS.find((p) => p.bit + 1 === g.clockSource);
  lines.push(`  <clock source="${clockPort ? clockPort.key : 'master'}" bpm="${g.bpm}" timeSignature="${g.beatsPerBar}/${g.beatUnit}" sendClock="${!!g.clockOut}" tap="${['none', 'note', 'cc'][g.tapType] || 'none'}" tapChannel="${g.tapChannel > 15 ? 'any' : g.tapChannel + 1}" tapNumber="${g.tapNumber}"/>`);
  const valueAttr = buttonValueAttrName(g.buttonAction);
  lines.push(`  <button action="${BUTTON_ACTION_NAMES[g.buttonAction] || 'panic'}"${valueAttr ? ` ${valueAttr}="${g.buttonParamA}"` : ''} channel="${g.buttonParamB === 16 ? 'all' : g.buttonParamB + 1}" toggle="${!!g.buttonToggleMomentary}"/>`);
  lines.push('  <routes>');
  for (const r of state.routes) {
    lines.push(`    <route name="${esc(r.name || '')}">`);
    lines.push(`      <input ports="${portsToString(r.inPorts)}" device="${legacyDeviceName(r.inPorts, IN_DEVICE_NAMES)}" channels="${channelsToRangeString(r.inputChannels)}"/>`);
    lines.push(`      <passthrough programChange="${!!(r.typeFlags & DATA_TYPE_BITS.programChange)}" pitchBend="${!!(r.typeFlags & DATA_TYPE_BITS.pitchBend)}" aftertouch="${!!(r.typeFlags & DATA_TYPE_BITS.aftertouch)}" aftertouchMapEnabled="${!!(r.typeFlags & DATA_TYPE_BITS.aftertouchMap)}" aftertouchMapCC="${r.atMapCC}" pressure="${!!(r.typeFlags & DATA_TYPE_BITS.pressure)}" pressureMapEnabled="${!!r.cpMapEnabled}" pressureMapCC="${r.cpMapCC}" sysex="${!!(r.typeFlags & DATA_TYPE_BITS.sysex)}" common="${!!r.commonEnabled}" realtime="${!!r.realtimeEnabled}" clock="${!!r.clockEnabled}"/>`);
    lines.push(`      <noteRange enabled="${!!(r.typeFlags & DATA_TYPE_BITS.note)}" start="${r.noteStart}" end="${r.noteEnd}"/>`);
    lines.push(`      <ccRange enabled="${!!(r.typeFlags & DATA_TYPE_BITS.cc)}" start="${r.ccStart}" end="${r.ccEnd}"/>`);
    lines.push(`      <output ports="${portsToString(r.outPorts)}" device="${legacyDeviceName(r.outPorts, OUT_DEVICE_NAMES)}" channels="${channelsToRangeString(r.outputChannels)}" transpose="${r.transpose}" velocityScale="${r.velocityScale}" ccMapStart="${r.ccMapStart}" monoRetrig="${!!r.monoRetrig}" roundRobin="${!!r.roundRobin}" roundRobinRandom="${!!r.roundRobinRandom}" noteToCc="${!!(r.features & FEATURE.NOTE_TO_CC)}" launchpad="${!!(r.features & FEATURE.LAUNCHPAD)}" scale="${!!(r.features & FEATURE.SCALE)}" layout="${esc(LAYOUTS[r.scaleLayout || 0])}" autoChord="${!!(r.features & FEATURE.AUTO_CHORD)}" chord="${Array.from({ length: 12 }, (_, k) => k).filter((k) => (r.chordMask || 0) & (1 << k)).map((k) => k + 1).join(',')}" arp="${r.arpSlot || 0}"/>`);
    lines.push('    </route>');
  }
  lines.push('  </routes>');
  lines.push('  <noteToCc>');
  for (const e of state.noteCc || []) lines.push(`    <map note="${e.note}" cc="${e.cc}" value="${NOTE_CC_MODE_NAMES[e.mode] || 'momentary'}"/>`);
  lines.push('  </noteToCc>');
  const sc = state.scale || defaultScale();
  lines.push(`  <scale root="${sc.root}" scale="${esc(SCALES[sc.scaleType]?.name || 'Major')}"/>`);
  lines.push('  <arps>');
  (state.arps || []).forEach((ar, s) => {
    lines.push(`    <arp slot="${s + 1}" mode="${esc(ARP_MODES[ar.mode] || 'Up')}" division="${DIVISIONS[ar.division] || '1/16'}" octaves="${ar.octaves}" gate="${ar.gate}" velocity="${ar.aftertouch ? 'aftertouch' : (ar.velocity ? ar.velocity : 'played')}" hold="${ar.holdType ? 'cc' : 'none'}" holdChannel="${ar.holdChannel > 15 ? 'any' : ar.holdChannel + 1}" holdNumber="${ar.holdNumber}" bypass="${ar.bypassChannel || 'off'}" bass="${ar.bassChannel || 'off'}">`);
    (ar.squares || []).forEach((sq) => {
      if (!sq) return;
      lines.push(`      <division value="${DIVISIONS[sq.division]}" trigger="${['none', 'note', 'cc'][sq.triggerType] || 'none'}" channel="${sq.triggerChannel > 15 ? 'any' : sq.triggerChannel + 1}" number="${sq.triggerNumber}"/>`);
    });
    lines.push('    </arp>');
  });
  lines.push('  </arps>');
  lines.push('  <launchpad>');
  (state.launchpad || []).forEach((b, i) => {
    if (b.type) lines.push(`    <button index="${i}" sends="${b.type === 1 ? 'note' : 'cc'}" number="${b.number}"/>`);
  });
  lines.push('  </launchpad>');
  lines.push('</foxwizardPatchlingPreset>');
  return lines.join('\n');
}

function boolAttr(el, name) {
  return el.getAttribute(name) === 'true';
}

export function parsePreset(xmlText) {
  const doc = new DOMParser().parseFromString(xmlText, 'text/xml');
  const errorNode = doc.querySelector('parsererror');
  if (errorNode) throw new Error('Could not parse preset XML');

  const root = doc.querySelector('foxwizardPatchlingPreset');
  if (!root) throw new Error('Not a FoxWizard Patchling preset file');

  const name = root.querySelector('name')?.textContent?.trim() || 'Untitled Preset';
  const modeText = root.querySelector('mode')?.textContent?.trim() || 'usbAdaptor';
  const mode = Math.max(0, MODE_NAMES.indexOf(modeText));
  const ledPct = parseInt(root.querySelector('ledBrightness')?.textContent || '100', 10);
  const ledBrightnessStep = Math.min(10, Math.max(0, Math.round(ledPct / 10)));

  const buttonEl = root.querySelector('button');
  const buttonAction = Math.max(0, BUTTON_ACTION_NAMES.indexOf(buttonEl?.getAttribute('action') || 'panic'));
  const buttonParamA = parseInt(
    buttonEl?.getAttribute('speed') || buttonEl?.getAttribute('cc') || buttonEl?.getAttribute('note') || '0', 10
  );
  const chAttr = buttonEl?.getAttribute('channel') || '1';
  const buttonParamB = chAttr === 'all' ? 16 : (parseInt(chAttr, 10) - 1);
  const buttonToggleMomentary = boolAttr(buttonEl || doc.createElement('x'), 'toggle') ? 1 : 0;

  // Global clock, missing in presets from before firmware 1.4: Patchling's defaults stand.
  const clock = {};
  const clockEl = root.querySelector('clock');
  if (clockEl) {
    const src = clockEl.getAttribute('source') || 'master';
    const port = PORTS.find((p) => p.key === src && p.input);
    const [beats, unit] = (clockEl.getAttribute('timeSignature') || '4/4').split('/').map((n) => parseInt(n, 10));
    const tapCh = clockEl.getAttribute('tapChannel') || 'any';
    Object.assign(clock, {
      clockSource: port ? port.bit + 1 : 0,
      bpm: Math.min(300, Math.max(20, parseInt(clockEl.getAttribute('bpm') || '120', 10) || 120)),
      beatsPerBar: Math.min(16, Math.max(1, beats || 4)),
      beatUnit: [2, 4, 8, 16].includes(unit) ? unit : 4,
      clockOut: boolAttr(clockEl, 'sendClock') ? 1 : 0,
      tapType: Math.max(0, ['none', 'note', 'cc'].indexOf(clockEl.getAttribute('tap') || 'none')),
      tapChannel: tapCh === 'any' ? 16 : Math.min(15, Math.max(0, parseInt(tapCh, 10) - 1)),
      tapNumber: Math.min(127, Math.max(0, parseInt(clockEl.getAttribute('tapNumber') || '0', 10) || 0))
    });
  }

  const routes = [];
  for (const routeEl of root.querySelectorAll('routes > route')) {
    const inputEl = routeEl.querySelector('input');
    const passEl = routeEl.querySelector('passthrough');
    const noteEl = routeEl.querySelector('noteRange');
    const ccEl = routeEl.querySelector('ccRange');
    const outputEl = routeEl.querySelector('output');

    let typeFlags = 0;
    if (boolAttr(noteEl || document.createElement('x'), 'enabled')) typeFlags |= DATA_TYPE_BITS.note;
    if (boolAttr(ccEl || document.createElement('x'), 'enabled')) typeFlags |= DATA_TYPE_BITS.cc;
    if (boolAttr(passEl || document.createElement('x'), 'programChange')) typeFlags |= DATA_TYPE_BITS.programChange;
    if (boolAttr(passEl || document.createElement('x'), 'pitchBend')) typeFlags |= DATA_TYPE_BITS.pitchBend;
    if (boolAttr(passEl || document.createElement('x'), 'aftertouch')) typeFlags |= DATA_TYPE_BITS.aftertouch;
    if (boolAttr(passEl || document.createElement('x'), 'aftertouchMapEnabled')) typeFlags |= DATA_TYPE_BITS.aftertouchMap;
    if (boolAttr(passEl || document.createElement('x'), 'pressure')) typeFlags |= DATA_TYPE_BITS.pressure;
    if (boolAttr(passEl || document.createElement('x'), 'sysex')) typeFlags |= DATA_TYPE_BITS.sysex;

    routes.push({
      name: routeEl.getAttribute('name') || '',
      enabled: true, // no disabled-but-kept-around concept — a route that exists is active; delete it otherwise
      inPorts: readPorts(inputEl, 'both'),
      inputChannels: rangeStringToChannels(inputEl?.getAttribute('channels') || 'all'),
      typeFlags,
      noteStart: parseInt(noteEl?.getAttribute('start') || '0', 10),
      noteEnd: parseInt(noteEl?.getAttribute('end') || '127', 10),
      ccStart: parseInt(ccEl?.getAttribute('start') || '0', 10),
      ccEnd: parseInt(ccEl?.getAttribute('end') || '127', 10),
      outPorts: readPorts(outputEl, 'none'),
      outputChannels: rangeStringToChannels(outputEl?.getAttribute('channels') || 'all'),
      transpose: parseInt(outputEl?.getAttribute('transpose') || '0', 10),
      velocityScale: parseInt(outputEl?.getAttribute('velocityScale') || '100', 10),
      ccMapStart: parseInt(outputEl?.getAttribute('ccMapStart') || '0', 10),
      atMapCC: parseInt(passEl?.getAttribute('aftertouchMapCC') || '0', 10),
      cpMapEnabled: boolAttr(passEl || document.createElement('x'), 'pressureMapEnabled'),
      cpMapCC: parseInt(passEl?.getAttribute('pressureMapCC') || '0', 10),
      // Accept the former autoLegato attribute so existing preset files remain importable.
      monoRetrig: boolAttr(outputEl || document.createElement('x'), 'monoRetrig') ||
        boolAttr(outputEl || document.createElement('x'), 'autoLegato'),
      roundRobin: boolAttr(outputEl || document.createElement('x'), 'roundRobin'),
      roundRobinRandom: boolAttr(outputEl || document.createElement('x'), 'roundRobinRandom'),
      // Presets saved before Common/Realtime existed have no opinion on these attributes at all —
      // default to on (matching a freshly-added route) rather than off, only respecting an explicit
      // false from a preset that was actually saved with this feature.
      commonEnabled: passEl && passEl.hasAttribute('common') ? boolAttr(passEl, 'common') : true,
      realtimeEnabled: passEl && passEl.hasAttribute('realtime') ? boolAttr(passEl, 'realtime') : true,
      clockEnabled: passEl && passEl.hasAttribute('clock') ? boolAttr(passEl, 'clock') : true,
      chordMask: outputEl?.hasAttribute('chord') ? parseChord(outputEl.getAttribute('chord')) : undefined,
      scaleLayout: outputEl?.hasAttribute('layout') ? Math.max(0, LAYOUTS.indexOf(outputEl.getAttribute('layout'))) : undefined,
      arpSlot: Math.min(ARP_SLOTS, Math.max(0, parseInt(outputEl?.getAttribute('arp') || '0', 10) || 0)),
      features: (boolAttr(outputEl || document.createElement('x'), 'noteToCc') ? FEATURE.NOTE_TO_CC : 0) |
        (boolAttr(outputEl || document.createElement('x'), 'launchpad') ? FEATURE.LAUNCHPAD : 0) |
        (boolAttr(outputEl || document.createElement('x'), 'scale') ? FEATURE.SCALE : 0) |
        (boolAttr(outputEl || document.createElement('x'), 'autoChord') ? FEATURE.AUTO_CHORD : 0)
    });
  }

  const arps = Array.from({ length: ARP_SLOTS }, defaultArp);
  for (const el of root.querySelectorAll('arps > arp')) {
    const s = parseInt(el.getAttribute('slot') || '0', 10) - 1;
    if (s < 0 || s >= ARP_SLOTS) continue;
    const vel = el.getAttribute('velocity') || 'played';
    const ch = el.getAttribute('holdChannel') || 'any';
    arps[s] = {
      mode: Math.max(0, ARP_MODES.indexOf(el.getAttribute('mode') === 'Up 1-Oct' ? 'Up 4-Step' : el.getAttribute('mode'))), // renamed in 1.20
      division: DIVISIONS.includes(el.getAttribute('division')) ? DIVISIONS.indexOf(el.getAttribute('division')) : arps[s].division,
      octaves: Math.min(4, Math.max(1, parseInt(el.getAttribute('octaves') || '1', 10) || 1)),
      gate: Math.min(100, Math.max(5, parseInt(el.getAttribute('gate') || '50', 10) || 50)),
      velocity: vel === 'played' || vel === 'aftertouch' ? 0 : Math.min(127, Math.max(1, parseInt(vel, 10) || 100)),
      aftertouch: vel === 'aftertouch',
      bypassChannel: Math.min(16, Math.max(0, parseInt(el.getAttribute('bypass') || '0', 10) || 0)),
      bassChannel: Math.min(16, Math.max(0, parseInt(el.getAttribute('bass') || '0', 10) || 0)),
      squares: Array.from({ length: ARP_SQUARES }, (_, k) => {
        const d = el.querySelectorAll('division')[k];
        if (!d || !DIVISIONS.includes(d.getAttribute('value'))) return null;
        const c = d.getAttribute('channel') || 'any';
        return {
          division: DIVISIONS.indexOf(d.getAttribute('value')),
          triggerType: Math.max(0, ['none', 'note', 'cc'].indexOf(d.getAttribute('trigger'))),
          triggerChannel: c === 'any' ? 16 : Math.min(15, Math.max(0, parseInt(c, 10) - 1)),
          triggerNumber: Math.min(127, Math.max(0, parseInt(d.getAttribute('number') || '0', 10) || 0))
        };
      }),
      holdType: el.getAttribute('hold') === 'cc' ? 2 : 0,
      holdChannel: ch === 'any' ? 16 : Math.min(15, Math.max(0, parseInt(ch, 10) - 1)),
      holdNumber: Math.min(127, Math.max(0, parseInt(el.getAttribute('holdNumber') || '64', 10) || 0))
    };
  }

  const scale = defaultScale();
  const scaleEl = root.querySelector('scale');
  if (scaleEl) {
    scale.root = Math.min(11, Math.max(0, parseInt(scaleEl.getAttribute('root') || '0', 10) || 0));
    scale.scaleType = Math.max(0, SCALES.findIndex((s) => s.name === scaleEl.getAttribute('scale')));
    scale.layout = Math.max(0, LAYOUTS.indexOf(scaleEl.getAttribute('layout')));
    scale.chordMask = parseChord(scaleEl.getAttribute('chord'));
  }

  // Presets from 1.11-1.13 kept one Layout on <scale>, it goes to each Scale route without its own.
  for (const r of routes) {
    if (r.scaleLayout === undefined) r.scaleLayout = (r.features & FEATURE.SCALE) ? scale.layout : 0;
    if (r.chordMask === undefined) r.chordMask = (r.features & FEATURE.AUTO_CHORD) ? scale.chordMask : 0; // presets from 1.12-1.14
  }
  scale.layout = 0;
  scale.chordMask = 0;

  const launchpad = defaultLaunchpadButtons();
  for (const b of root.querySelectorAll('launchpad > button')) {
    const i = parseInt(b.getAttribute('index') || '-1', 10);
    if (i < 0 || i >= LAUNCHPAD_BUTTONS) continue;
    launchpad[i] = { type: b.getAttribute('sends') === 'cc' ? 2 : 1, number: Math.min(127, Math.max(0, parseInt(b.getAttribute('number') || '0', 10) || 0)) };
  }

  const noteCc = [...root.querySelectorAll('noteToCc > map')].slice(0, NOTE_CC_ENTRIES).map((m) => ({
    note: Math.min(127, Math.max(0, parseInt(m.getAttribute('note') || '0', 10) || 0)),
    cc: Math.min(127, Math.max(0, parseInt(m.getAttribute('cc') || '0', 10) || 0)),
    mode: Math.max(0, NOTE_CC_MODE_NAMES.indexOf(m.getAttribute('value') || 'momentary'))
  }));

  return {
    name,
    global: { mode, buttonAction, buttonParamA, buttonParamB, buttonToggleMomentary, ledBrightnessStep, ...clock },
    routes,
    noteCc,
    launchpad,
    scale,
    arps
  };
}

export function downloadPreset(state) {
  const xml = serializePreset(state);
  const blob = new Blob([xml], { type: 'application/xml' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  const safeName = (state.name || 'foxwizard-preset').replace(/[^a-z0-9\-_]+/gi, '_');
  a.download = `${safeName}.xml`;
  document.body.appendChild(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(url);
}

export function readPresetFile(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => {
      try { resolve(parsePreset(reader.result)); }
      catch (err) { reject(err); }
    };
    reader.onerror = () => reject(reader.error);
    reader.readAsText(file);
  });
}
