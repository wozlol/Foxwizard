import { ACTION_TYPES, MAP_ENTRIES, defaultAction, defaultLpActions } from './mapper.js';
// Human-readable XML preset format for FoxWizard Patchling.
// A preset captures mode, button config, LED brightness, and the full route list — everything
// the device's SysEx protocol can set. Numbers are kept human (channel lists as "1,2,4-6", device
// names as words) rather than raw bitmasks/enums, so the file is meant to be hand-editable.

import { DATA_TYPE_BITS, PORTS, PORT_TRS, PORT_USB, FEATURE, SCALES, LAYOUTS, defaultScale, ARP_SLOTS, ARP_MODES, ARP_SQUARES, DIVISIONS, defaultArp, defaultEcho, defaultStutter, defaultLooper, defaultLpQuad, noTrigger, STUTTER_RUNOUTS, snapRunout, STUTTER_DIVISIONS } from './midi.js';


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

// Division squares (arp and echo) as <division> children.
function squaresToXml(lines, squares, indent, divisions = DIVISIONS) {
  (squares || []).forEach((sq) => {
    if (!sq) return;
    lines.push(`${indent}<division value="${divisions[sq.division]}" trigger="${['none', 'note', 'cc'][sq.triggerType] || 'none'}" channel="${sq.triggerChannel > 15 ? 'any' : sq.triggerChannel + 1}" number="${sq.triggerNumber}"/>`);
  });
}

function squaresFromXml(el, divisions = DIVISIONS) {
  const found = [...el.querySelectorAll('division')];
  return Array.from({ length: ARP_SQUARES }, (_, k) => {
    const d = found[k];
    if (!d || !divisions.includes(d.getAttribute('value'))) return null;
    const c = d.getAttribute('channel') || 'any';
    return {
      division: divisions.indexOf(d.getAttribute('value')),
      triggerType: Math.max(0, ['none', 'note', 'cc'].indexOf(d.getAttribute('trigger'))),
      triggerChannel: c === 'any' ? 16 : Math.min(15, Math.max(0, parseInt(c, 10) - 1)),
      triggerNumber: Math.min(127, Math.max(0, parseInt(d.getAttribute('number') || '0', 10) || 0))
    };
  });
}

function echoFromXml(el) {
  const e = defaultEcho();
  if (!el) return e;
  if (DIVISIONS.includes(el.getAttribute('division'))) e.division = DIVISIONS.indexOf(el.getAttribute('division'));
  e.repeats = Math.min(32, Math.max(1, parseInt(el.getAttribute('repeats') || '4', 10) || 4));
  e.level = Math.min(100, Math.max(1, parseInt(el.getAttribute('level') || '70', 10) || 70));
  e.squares = squaresFromXml(el);
  e.defaultTrigger = triggerFromXml(el);
  e.afterRoute = el.getAttribute('afterRoute') !== 'false';
  e.latching = el.getAttribute('latching') !== 'false';
  e.global = el.getAttribute('global') === 'true';
  return e;
}

function stutterFromXml(el) {
  const st = defaultStutter();
  if (!el) return st;
  if (STUTTER_DIVISIONS.includes(el.getAttribute('division'))) st.division = STUTTER_DIVISIONS.indexOf(el.getAttribute('division'));
  const runout = el.getAttribute('runoutBars') || 'off';
  st.maxBars = runout === 'off' ? 0 : snapRunout(Math.min(16, Math.max(1, parseInt(runout, 10) || 16)));
  st.squares = squaresFromXml(el, STUTTER_DIVISIONS);
  st.defaultTrigger = triggerFromXml(el);
  st.afterRoute = el.getAttribute('afterRoute') !== 'false';
  st.latching = el.getAttribute('latching') === 'true';
  st.global = el.getAttribute('global') === 'true';
  return st;
}

function looperFromXml(el) {
  const lp = defaultLooper();
  if (!el) return lp;
  const len = el.getAttribute('length') || 'free';
  lp.bars = len === 'free' ? 0 : Math.min(16, Math.max(1, parseInt(len, 10) || 1));
  lp.quantize = DIVISIONS.indexOf(el.getAttribute('quantize'));
  lp.autoOverdub = el.getAttribute('autoOverdub') === 'true';
  lp.recordInputs = el.getAttribute('records') === 'inputs';
  lp.holdClears = el.getAttribute('holdClears') !== 'false';
  lp.loopUnrouted = el.getAttribute('loopUnrouted') !== 'false';
  lp.trigger = triggerFromXml(el);
  lp.recordCcs = el.getAttribute('recordCcs') === 'true';
  return lp;
}

// A default square's trigger as attributes: trigger="note|cc" triggerChannel="1-16|any" triggerNumber.
function triggerAttrs(t) {
  if (!t || !t.triggerType) return '';
  return ` trigger="${t.triggerType === 1 ? 'note' : 'cc'}" triggerChannel="${t.triggerChannel > 15 ? 'any' : t.triggerChannel + 1}" triggerNumber="${t.triggerNumber}"`;
}

function triggerFromXml(el) {
  const type = ['none', 'note', 'cc'].indexOf(el.getAttribute('trigger') || 'none');
  if (type <= 0) return noTrigger();
  const c = el.getAttribute('triggerChannel') || 'any';
  return {
    triggerType: type,
    triggerChannel: c === 'any' ? 16 : Math.min(15, Math.max(0, parseInt(c, 10) - 1)),
    triggerNumber: Math.min(127, Math.max(0, parseInt(el.getAttribute('triggerNumber') || '0', 10) || 0))
  };
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
    lines.push(`      <output ports="${portsToString(r.outPorts)}" device="${legacyDeviceName(r.outPorts, OUT_DEVICE_NAMES)}" channels="${channelsToRangeString(r.outputChannels)}" transpose="${r.transpose}" velocityScale="${r.velocityScale}" ccMapStart="${r.ccMapStart}" monoRetrig="${!!r.monoRetrig}" roundRobin="${!!r.roundRobin}" roundRobinRandom="${!!r.roundRobinRandom}" noteToCc="${!!(r.features & FEATURE.NOTE_TO_CC)}" launchpad="${!!(r.features & FEATURE.LAUNCHPAD)}" launchpadOctave="${r.lpOctave || 0}" quadEnabled="${!!(r.lpQuad || defaultLpQuad()).enabled}" quadBlock="${(r.lpQuad || defaultLpQuad()).block}" quadChannel="${(r.lpQuad || defaultLpQuad()).channel}" quadStartNote="${(r.lpQuad || defaultLpQuad()).startNote}" scale="${!!(r.features & FEATURE.SCALE)}" layout="${esc(LAYOUTS[r.scaleLayout || 0])}" autoChord="${!!(r.features & FEATURE.AUTO_CHORD)}" chord="${Array.from({ length: 12 }, (_, k) => k).filter((k) => (r.chordMask || 0) & (1 << k)).map((k) => k + 1).join(',')}" arp="${r.arpSlot || 0}" echo="${!!(r.features & FEATURE.ECHO)}" stutter="${!!(r.features & FEATURE.STUTTER)}" ccToFeature="${!!(r.features & FEATURE.CC_MAP)}" looperEnabled="${r.looperEnabled !== false}"/>`);
    lines.push('    </route>');
  }
  lines.push('  </routes>');
  lines.push('  <noteToFeature>');
  for (const e of state.noteMap || []) lines.push(`    <map note="${e.source}"${actionAttrs(e.action)}/>`);
  lines.push('  </noteToFeature>');
  lines.push('  <ccToFeature>');
  for (const e of state.ccMap || []) lines.push(`    <map cc="${e.source}"${actionAttrs(e.action)}/>`);
  lines.push('  </ccToFeature>');
  const sc = state.scale || defaultScale();
  lines.push(`  <scale root="${sc.root}" scale="${esc(SCALES[sc.scaleType]?.name || 'Major')}"/>`);
  lines.push('  <arps>');
  (state.arps || []).forEach((ar, s) => {
    lines.push(`    <arp slot="${s + 1}" mode="${esc(ARP_MODES[ar.mode] || 'Up')}" division="${DIVISIONS[ar.division] || '1/16'}" octaves="${ar.octaves}" gate="${ar.gate}" velocity="${ar.aftertouch ? 'aftertouch' : (ar.velocity ? ar.velocity : 'played')}" hold="${ar.holdType ? 'cc' : 'none'}" holdChannel="${ar.holdChannel > 15 ? 'any' : ar.holdChannel + 1}" holdNumber="${ar.holdNumber}" bypass="${ar.bypassChannel || 'off'}" bass="${ar.bassChannel || 'off'}" followsLiveCh="${!!ar.followsLiveChannel}"${triggerAttrs(ar.defaultTrigger)}>`);
    squaresToXml(lines, ar.squares, '      ');
    lines.push('    </arp>');
  });
  lines.push('  </arps>');
  const ec = state.echo || defaultEcho();
  lines.push(`  <echo division="${DIVISIONS[ec.division]}" repeats="${ec.repeats}" level="${ec.level}" afterRoute="${ec.afterRoute !== false}" latching="${ec.latching !== false}" global="${!!ec.global}"${triggerAttrs(ec.defaultTrigger)}>`);
  squaresToXml(lines, ec.squares, '    ');
  lines.push('  </echo>');
  const stt = state.stutter || defaultStutter();
  lines.push(`  <stutter division="${STUTTER_DIVISIONS[stt.division]}" runoutBars="${stt.maxBars === 0 ? 'off' : stt.maxBars}" afterRoute="${stt.afterRoute !== false}" latching="${!!stt.latching}" global="${!!stt.global}"${triggerAttrs(stt.defaultTrigger)}>`);
  squaresToXml(lines, stt.squares, '    ', STUTTER_DIVISIONS);
  lines.push('  </stutter>');
  const lp = state.looper || defaultLooper();
  lines.push(`  <looper length="${lp.bars ? lp.bars : 'free'}" quantize="${lp.quantize >= 0 ? DIVISIONS[lp.quantize] : 'off'}" autoOverdub="${lp.autoOverdub}" records="${lp.recordInputs ? 'inputs' : 'outputs'}" holdClears="${lp.holdClears}" loopUnrouted="${lp.loopUnrouted !== false}" recordCcs="${!!lp.recordCcs}"${triggerAttrs(lp.trigger)}>`);
  lines.push('  </looper>');
  lines.push('  <launchpad>');
  (state.lpActions || []).forEach((act, i) => {
    if (act.type) lines.push(`    <button index="${i}"${actionAttrs(act)}/>`);
  });
  lines.push('  </launchpad>');
  lines.push('</foxwizardPatchlingPreset>');
  return lines.join('\n');
}

// An action as attributes: action="CC Level" a="20" b="64".
function actionAttrs(act) {
  const t = ACTION_TYPES.find((x) => x.v === act?.type) || ACTION_TYPES[0];
  return ` action="${t.l}" a="${act?.a || 0}" b="${act?.b || 0}"`;
}

function actionFromXml(el) {
  const t = ACTION_TYPES.find((x) => x.l === el.getAttribute('action'));
  if (!t) return defaultAction();
  const clamp = (name) => Math.min(127, Math.max(0, parseInt(el.getAttribute(name) || '0', 10) || 0));
  return { type: t.v, a: clamp('a'), b: clamp('b') };
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
      chordMask: parseChord(outputEl?.getAttribute('chord')),
      scaleLayout: Math.max(0, LAYOUTS.indexOf(outputEl?.getAttribute('layout'))),
      lpOctave: Math.min(2, Math.max(0, parseInt(outputEl?.getAttribute('launchpadOctave') || '0', 10) || 0)),
      looperEnabled: outputEl?.getAttribute('looperEnabled') !== 'false',
      lpQuad: {
        enabled: outputEl?.getAttribute('quadEnabled') === 'true',
        block: Math.min(3, Math.max(0, parseInt(outputEl?.getAttribute('quadBlock') || '3', 10) || 0)),
        channel: Math.min(15, Math.max(0, parseInt(outputEl?.getAttribute('quadChannel') || '9', 10) || 0)),
        startNote: Math.min(127, Math.max(0, parseInt(outputEl?.getAttribute('quadStartNote') || '36', 10) || 0))
      },
      arpSlot: Math.min(ARP_SLOTS, Math.max(0, parseInt(outputEl?.getAttribute('arp') || '0', 10) || 0)),
      features: (boolAttr(outputEl || document.createElement('x'), 'noteToCc') ? FEATURE.NOTE_TO_CC : 0) |
        (boolAttr(outputEl || document.createElement('x'), 'launchpad') ? FEATURE.LAUNCHPAD : 0) |
        (boolAttr(outputEl || document.createElement('x'), 'scale') ? FEATURE.SCALE : 0) |
        (boolAttr(outputEl || document.createElement('x'), 'autoChord') ? FEATURE.AUTO_CHORD : 0) |
        (boolAttr(outputEl || document.createElement('x'), 'echo') ? FEATURE.ECHO : 0) |
        (boolAttr(outputEl || document.createElement('x'), 'stutter') ? FEATURE.STUTTER : 0) |
        (boolAttr(outputEl || document.createElement('x'), 'ccToFeature') ? FEATURE.CC_MAP : 0)
    });
  }

  const arps = Array.from({ length: ARP_SLOTS }, defaultArp);
  for (const el of root.querySelectorAll('arps > arp')) {
    const s = parseInt(el.getAttribute('slot') || '0', 10) - 1;
    if (s < 0 || s >= ARP_SLOTS) continue;
    const vel = el.getAttribute('velocity') || 'played';
    const ch = el.getAttribute('holdChannel') || 'any';
    arps[s] = {
      mode: Math.max(0, ARP_MODES.indexOf(el.getAttribute('mode'))),
      division: DIVISIONS.includes(el.getAttribute('division')) ? DIVISIONS.indexOf(el.getAttribute('division')) : arps[s].division,
      octaves: Math.min(4, Math.max(1, parseInt(el.getAttribute('octaves') || '1', 10) || 1)),
      gate: Math.min(100, Math.max(5, parseInt(el.getAttribute('gate') || '50', 10) || 50)),
      velocity: vel === 'played' || vel === 'aftertouch' ? 0 : Math.min(127, Math.max(1, parseInt(vel, 10) || 100)),
      aftertouch: vel === 'aftertouch',
      bypassChannel: Math.min(16, Math.max(0, parseInt(el.getAttribute('bypass') || '0', 10) || 0)),
      bassChannel: Math.min(16, Math.max(0, parseInt(el.getAttribute('bass') || '0', 10) || 0)),
      squares: squaresFromXml(el),
      defaultTrigger: triggerFromXml(el),
      followsLiveChannel: el.getAttribute('followsLiveCh') === 'true',
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
  }

  const num = (el, name) => Math.min(127, Math.max(0, parseInt(el.getAttribute(name) || '0', 10) || 0));
  const lpActions = defaultLpActions();
  for (const b of root.querySelectorAll('launchpad > button')) {
    const i = parseInt(b.getAttribute('index') || '-1', 10);
    if (i >= 0 && i < 16) lpActions[i] = actionFromXml(b);
  }

  const noteMap = [...root.querySelectorAll('noteToFeature > map')].slice(0, MAP_ENTRIES).map((m) => ({ source: num(m, 'note'), action: actionFromXml(m) }));
  const ccMap = [...root.querySelectorAll('ccToFeature > map')].slice(0, MAP_ENTRIES).map((m) => ({ source: num(m, 'cc'), action: actionFromXml(m) }));

  return {
    name,
    global: { mode, buttonAction, buttonParamA, buttonParamB, buttonToggleMomentary, ledBrightnessStep, ...clock },
    routes,
    noteMap,
    ccMap,
    lpActions,
    scale,
    arps,
    echo: echoFromXml(root.querySelector('echo')),
    stutter: stutterFromXml(root.querySelector('stutter')),
    looper: looperFromXml(root.querySelector('looper'))
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
