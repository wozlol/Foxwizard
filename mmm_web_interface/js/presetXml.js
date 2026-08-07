// Human-readable XML preset format for Foxwizard Patchling.
// A preset captures mode, button config, LED brightness, and the full route list — everything
// the device's SysEx protocol can set. Numbers are kept human (channel lists as "1,2,4-6", device
// names as words) rather than raw bitmasks/enums, so the file is meant to be hand-editable.

import { DATA_TYPE_BITS } from './midi.js';

const MODE_NAMES = ['usbAdaptor', 'router'];
const BUTTON_ACTION_NAMES = ['panic', 'pitchUp', 'pitchDown', 'mod', 'ccMomentary', 'ccToggle', 'octaveUp', 'octaveDown', 'noteMomentary', 'noteToggle'];

// buttonParamA means something different per action (ramp speed, cc number, or note number) — use
// a matching attribute name in the XML rather than a generic one so a hand-edited file reads right.
function buttonValueAttrName(action) {
  if ([1, 2, 3].includes(action)) return 'speed';
  if ([4, 5].includes(action)) return 'cc';
  if ([8, 9].includes(action)) return 'note';
  return null;
}
const IN_DEVICE_NAMES = ['trs', 'usb', 'both'];
const OUT_DEVICE_NAMES = ['none', 'trs', 'usb', 'both'];

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
  lines.push(`<foxwizardPatchlingPreset version="1" device="Foxwizard Mini MIDI Multitool">`);
  lines.push(`  <name>${esc(state.name || 'Untitled Preset')}</name>`);
  lines.push(`  <mode>${MODE_NAMES[g.mode] || 'usbAdaptor'}</mode>`);
  lines.push(`  <ledBrightness>${g.ledBrightnessStep * 10}</ledBrightness>`);
  const valueAttr = buttonValueAttrName(g.buttonAction);
  lines.push(`  <button action="${BUTTON_ACTION_NAMES[g.buttonAction] || 'panic'}"${valueAttr ? ` ${valueAttr}="${g.buttonParamA}"` : ''} channel="${g.buttonParamB === 16 ? 'all' : g.buttonParamB + 1}" toggle="${!!g.buttonToggleMomentary}"/>`);
  lines.push('  <routes>');
  for (const r of state.routes) {
    lines.push(`    <route name="${esc(r.name || '')}">`);
    lines.push(`      <input device="${IN_DEVICE_NAMES[r.inputDevice]}" channels="${channelsToRangeString(r.inputChannels)}"/>`);
    lines.push(`      <passthrough programChange="${!!(r.typeFlags & DATA_TYPE_BITS.programChange)}" pitchBend="${!!(r.typeFlags & DATA_TYPE_BITS.pitchBend)}" aftertouch="${!!(r.typeFlags & DATA_TYPE_BITS.aftertouch)}" pressure="${!!(r.typeFlags & DATA_TYPE_BITS.pressure)}" sysex="${!!(r.typeFlags & DATA_TYPE_BITS.sysex)}"/>`);
    lines.push(`      <noteRange enabled="${!!(r.typeFlags & DATA_TYPE_BITS.note)}" start="${r.noteStart}" end="${r.noteEnd}"/>`);
    lines.push(`      <ccRange enabled="${!!(r.typeFlags & DATA_TYPE_BITS.cc)}" start="${r.ccStart}" end="${r.ccEnd}"/>`);
    lines.push(`      <output device="${OUT_DEVICE_NAMES[r.outputDevice]}" channels="${channelsToRangeString(r.outputChannels)}" transpose="${r.transpose}" ccMapStart="${r.ccMapStart}"/>`);
    lines.push('    </route>');
  }
  lines.push('  </routes>');
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
  if (!root) throw new Error('Not a Foxwizard Patchling preset file');

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
    if (boolAttr(passEl || document.createElement('x'), 'pressure')) typeFlags |= DATA_TYPE_BITS.pressure;
    if (boolAttr(passEl || document.createElement('x'), 'sysex')) typeFlags |= DATA_TYPE_BITS.sysex;

    routes.push({
      name: routeEl.getAttribute('name') || '',
      enabled: true, // no disabled-but-kept-around concept — a route that exists is active; delete it otherwise
      inputDevice: Math.max(0, IN_DEVICE_NAMES.indexOf(inputEl?.getAttribute('device') || 'both')),
      inputChannels: rangeStringToChannels(inputEl?.getAttribute('channels') || 'all'),
      typeFlags,
      noteStart: parseInt(noteEl?.getAttribute('start') || '0', 10),
      noteEnd: parseInt(noteEl?.getAttribute('end') || '127', 10),
      ccStart: parseInt(ccEl?.getAttribute('start') || '0', 10),
      ccEnd: parseInt(ccEl?.getAttribute('end') || '127', 10),
      outputDevice: Math.max(0, OUT_DEVICE_NAMES.indexOf(outputEl?.getAttribute('device') || 'none')),
      outputChannels: rangeStringToChannels(outputEl?.getAttribute('channels') || 'all'),
      transpose: parseInt(outputEl?.getAttribute('transpose') || '0', 10),
      ccMapStart: parseInt(outputEl?.getAttribute('ccMapStart') || '0', 10)
    });
  }

  return {
    name,
    global: { mode, buttonAction, buttonParamA, buttonParamB, buttonToggleMomentary, ledBrightnessStep },
    routes
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
