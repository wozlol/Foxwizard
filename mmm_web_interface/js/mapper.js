// The mapper (firmware 1.40): one list of actions shared by Note to Feature, CC to Feature and the
// Launchpad buttons. An action is { type, a, b }, the same four bytes the firmware keeps.

export const ACT = {
  NONE: 0, NOTE: 1, CC_MOMENTARY: 2, CC_TOGGLE: 3, CC_VALUE: 4, CC_LEVEL: 5, TRANSPORT: 6,
  PROGRAM: 7, PROGRAM_STEP: 8, LIVE_CHANNEL: 9, LIVE_VELOCITY: 10, LOOPER: 11, TAP_TEMPO: 12,
  LIVE_CHANNEL_STEP: 13, ARP_DIV_STEP: 14, STUTTER_LENGTH: 15, QUAD_TOGGLE: 16, ECHO_TOGGLE: 17, // 1.48
  NOTE_TOGGLE: 18, PITCH_RAMP: 19, MOD_RAMP: 20, CC_RAMP: 21, // 1.48, button-mode parity
  PANIC: 22, OCTAVE: 23, // 1.49, button-mode parity
  PLOCK_CLEAR: 24 // 1.59
};

export const ACTION_TYPES = [
  { v: ACT.NONE, l: 'Off' },
  { v: ACT.NOTE, l: 'Note' },
  { v: ACT.CC_MOMENTARY, l: 'CC Momentary' },
  { v: ACT.CC_TOGGLE, l: 'CC Toggle' },
  { v: ACT.CC_VALUE, l: 'CC Value' },
  { v: ACT.CC_LEVEL, l: 'CC Level' },
  { v: ACT.TRANSPORT, l: 'Transport' },
  { v: ACT.PROGRAM, l: 'Program Change' },
  { v: ACT.PROGRAM_STEP, l: 'Program Up/Down' },
  { v: ACT.LIVE_CHANNEL, l: 'Live Ch' },
  { v: ACT.LIVE_VELOCITY, l: 'Live Velocity' },
  { v: ACT.LOOPER, l: 'Looper' },
  { v: ACT.TAP_TEMPO, l: 'Tap Tempo' },
  { v: ACT.LIVE_CHANNEL_STEP, l: 'Live Ch Up/Down' },
  { v: ACT.ARP_DIV_STEP, l: 'Arp Div Up/Down' },
  { v: ACT.STUTTER_LENGTH, l: 'Stutter Length' },
  { v: ACT.QUAD_TOGGLE, l: 'LP Quad' },
  { v: ACT.ECHO_TOGGLE, l: 'Echo On/Off' },
  { v: ACT.NOTE_TOGGLE, l: 'Note Toggle' },
  { v: ACT.PITCH_RAMP, l: 'Pitch Ramp' },
  { v: ACT.MOD_RAMP, l: 'Mod Ramp' },
  { v: ACT.CC_RAMP, l: 'CC Ramp' },
  { v: ACT.PANIC, l: 'Panic' },
  { v: ACT.OCTAVE, l: 'Octave Up/Down' },
  { v: ACT.PLOCK_CLEAR, l: 'Clear P-Locks' }
];

// Sent as MMC, and as MIDI Start, Stop or Continue where MIDI has one.
export const TRANSPORTS = ['Play', 'Stop', 'Continue', 'Pause', 'Record', 'Record Exit', 'Rewind', 'Fast Forward', 'Return to Start'];

// Undo here is Un-Clear: it brings back a cleared loop. Layer Undo and Layer Redo (1.60) step through
// overdub layers instead. Double and Half change the loop's length while it plays. Note Delete (1.66)
// is held: notes played meanwhile erase the same notes from the loop as it plays them.
export const LOOPER_ACTIONS = ['1-Button', 'Play/Arm/Rec', 'Stop/Clear/Undo', 'Play', 'Stop', 'Pause', 'Overdub', 'Clear', 'Un-Clear', 'Rec/Arm', 'Layer Undo', 'Layer Redo', 'Double', 'Half', 'Note Delete'];

// The mapper's Stutter Length action (1.48): bar fractions, measured against the actual time
// signature, rather than the arp-style musical divisions the route's own Stutter squares use.
export const STUTTER_LENGTHS = ['1/8 bar', '1/4 bar', '1/2 bar', '1 bar', '2 bars', '4 bars'];

export const MAP_ENTRIES = 16;

export function defaultAction(type = ACT.NONE) {
  switch (type) {
    case ACT.NOTE: return { type, a: 36, b: 0 };
    case ACT.CC_MOMENTARY: case ACT.CC_TOGGLE: case ACT.CC_VALUE: return { type, a: 20, b: 0 };
    case ACT.CC_LEVEL: return { type, a: 20, b: 64 };
    case ACT.LIVE_VELOCITY: return { type, a: 10, b: 0 }; // 100%
    case ACT.LIVE_CHANNEL_STEP: case ACT.ARP_DIV_STEP: return { type, a: 1, b: 0 }; // up
    case ACT.NOTE_TOGGLE: return { type, a: 36, b: 0 };
    case ACT.PITCH_RAMP: return { type, a: 1, b: 6 }; // up, a quarter note sweep
    case ACT.MOD_RAMP: return { type, a: 0, b: 6 };
    case ACT.CC_RAMP: return { type, a: 20, b: 6 };
    case ACT.OCTAVE: return { type, a: 1, b: 0 }; // up, momentary
    default: return { type, a: 0, b: 0 };
  }
}

export function actionToBytes(act) {
  const a = act || defaultAction();
  return [a.type & 0x7F, a.a & 0x7F, a.b & 0x7F, (a.c || 0) & 0x7F];
}

export function bytesToAction(b, o) {
  const type = b[o] <= ACT.PLOCK_CLEAR ? b[o] : ACT.NONE;
  return { type, a: (b[o + 1] || 0) & 0x7F, b: (b[o + 2] || 0) & 0x7F, c: (b[o + 3] || 0) & 0x7F };
}

// Note to Feature and CC to Feature: 16 entries of source + 1 (0 unused) and an action, 80 bytes.
export function mapToBytes(list) {
  const bytes = [];
  for (let i = 0; i < MAP_ENTRIES; i++) {
    const e = list[i];
    bytes.push(e ? (e.source & 0x7F) + 1 : 0, ...actionToBytes(e?.action));
  }
  return bytes;
}

export function bytesToMap(b) {
  const list = [];
  for (let i = 0; i < MAP_ENTRIES; i++) {
    const o = i * 5;
    if (!b[o]) continue;
    list.push({ source: b[o] - 1, action: bytesToAction(b, o + 1) });
  }
  return list;
}

export function lpActionsToBytes(actions) {
  const bytes = [];
  for (let i = 0; i < 16; i++) bytes.push(...actionToBytes(actions[i]));
  return bytes;
}

export function bytesToLpActions(b) {
  return Array.from({ length: 16 }, (_, i) => bytesToAction(b, i * 4));
}

// 1.48 defaults, left to right: Top 1-8 then Side 1-8 (indices 0-7, 8-15).
export function defaultLpActions() {
  return [
    { type: ACT.PROGRAM_STEP, a: 1, b: 0 },       // Top 1: Program Up
    { type: ACT.PROGRAM_STEP, a: 0, b: 0 },       // Top 2: Program Down
    { type: ACT.ARP_DIV_STEP, a: 0, b: 0 },       // Top 3: Arp Division Down
    { type: ACT.ARP_DIV_STEP, a: 1, b: 0 },       // Top 4: Arp Division Up
    { type: ACT.LIVE_CHANNEL, a: 0, b: 0 },        // Top 5: Live Ch 1
    { type: ACT.LIVE_CHANNEL, a: 9, b: 0 },        // Top 6: Live Ch 10
    { type: ACT.LIVE_CHANNEL, a: 1, b: 0 },        // Top 7: Live Ch 2
    { type: ACT.LIVE_CHANNEL, a: 2, b: 0 },        // Top 8: Live Ch 3
    { type: ACT.STUTTER_LENGTH, a: 4, b: 0 },      // Side 1: Stutter 2 bars
    { type: ACT.STUTTER_LENGTH, a: 3, b: 0 },      // Side 2: Stutter 1 bar
    { type: ACT.STUTTER_LENGTH, a: 2, b: 0 },      // Side 3: Stutter 1/2 bar
    { type: ACT.STUTTER_LENGTH, a: 1, b: 0 },      // Side 4: Stutter 1/4 bar
    { type: ACT.STUTTER_LENGTH, a: 0, b: 0 },      // Side 5: Stutter 1/8 bar
    { type: ACT.LOOPER, a: 14, b: 0 },             // Side 6: Looper Note Delete
    { type: ACT.LOOPER, a: 1, b: 0 },              // Side 7: Looper Play/Arm/Rec
    { type: ACT.LOOPER, a: 2, b: 0 }               // Side 8: Looper Stop/Clear/Undo
  ];
}

// The ramp's full sweep length: Instant (0), or a musical division (1 = divisions[0], matching the
// firmware's divisionPlus1 convention, same as Echo and Stutter).
function rampLengthSelect(cur, divisions) {
  const opt = (v, l) => `<option value="${v}" ${cur === v ? 'selected' : ''}>${l}</option>`;
  return `<select title="Ramp length">${opt(0, 'Instant')}${(divisions || []).map((d, i) => opt(i + 1, d)).join('')}</select>`;
}

export function usesLooper(act) { return act?.type === ACT.LOOPER; }
export function usesLiveChannel(act) { return act?.type === ACT.LIVE_CHANNEL; }
export function usesCc(act) { return [ACT.CC_MOMENTARY, ACT.CC_TOGGLE, ACT.CC_VALUE, ACT.CC_LEVEL].includes(act?.type); }

// The editor for one action. attrs identifies it: data-act-owner, data-act-index, data-route-index.
// showColor is true only for Launchpad buttons: a's type byte c is a raw Launchpad velocity color
// (0 = the usual channel-color indicator), meaningless for Note/CC to Feature entries. divisions is
// midi.js's DIVISIONS list, passed in rather than imported to avoid a circular import (midi.js
// already imports this module for the route/lpActions byte packing).
export function actionEditorHtml(act, attrs, noteSelectOptions, showColor, divisions) {
  const opt = (v, cur, l) => `<option value="${v}" ${cur === v ? 'selected' : ''}>${l}</option>`;
  const field = (f, html) => html.replace('<select', `<select data-act-field="${f}" ${attrs}`).replace('<input', `<input data-act-field="${f}" ${attrs}`);
  // A short visible label next to the number (CC, Val, Color...) since these fields otherwise look
  // like bare unlabeled numbers, especially on a Launchpad button where several sit side by side.
  const num = (f, v, min, max, label, title) => `<span class="field-mini"><span class="field-mini-label">${label}</span>${field(f, `<input type="number" min="${min}" max="${max}" value="${v}" title="${title || label}">`)}</span>`;
  let fields = '';
  switch (act.type) {
    case ACT.NOTE:
      fields = field('a', `<select title="Note">${noteSelectOptions(act.a)}</select>`);
      break;
    case ACT.CC_MOMENTARY: case ACT.CC_TOGGLE: case ACT.CC_VALUE:
      fields = num('a', act.a, 0, 127, 'CC', 'CC number');
      break;
    case ACT.CC_LEVEL:
      fields = num('a', act.a, 0, 127, 'CC', 'CC number') + num('b', act.b, 0, 127, 'Val', 'Level');
      break;
    case ACT.TRANSPORT:
      fields = field('a', `<select>${TRANSPORTS.map((t, i) => opt(i, act.a, t)).join('')}</select>`);
      break;
    case ACT.PROGRAM:
      fields = num('a', act.a, 0, 127, 'Pgm', 'Program');
      break;
    case ACT.PROGRAM_STEP:
      fields = field('a', `<select>${opt(1, act.a, 'Up')}${opt(0, act.a, 'Down')}</select>`);
      break;
    case ACT.LIVE_CHANNEL:
      fields = field('a', `<select title="Channel">${Array.from({ length: 16 }, (_, c) => opt(c, act.a, `Ch ${c + 1}`)).join('')}</select>`);
      break;
    case ACT.LIVE_VELOCITY:
      fields = field('a', `<select>${opt(0, act.a, 'Follows Value')}${Array.from({ length: 20 }, (_, k) => opt(k + 1, act.a, `${(k + 1) * 10}%`)).join('')}</select>`);
      break;
    case ACT.LOOPER:
      fields = field('a', `<select>${LOOPER_ACTIONS.map((l, i) => opt(i, act.a, l)).join('')}</select>`);
      break;
    case ACT.LIVE_CHANNEL_STEP:
    case ACT.ARP_DIV_STEP:
      fields = field('a', `<select>${opt(1, act.a, 'Up')}${opt(0, act.a, 'Down')}</select>`);
      break;
    case ACT.STUTTER_LENGTH:
      fields = field('a', `<select>${STUTTER_LENGTHS.map((l, i) => opt(i, act.a, l)).join('')}</select>`);
      break;
    case ACT.NOTE_TOGGLE:
      fields = field('a', `<select title="Note">${noteSelectOptions(act.a)}</select>`);
      break;
    case ACT.PITCH_RAMP:
      fields = field('a', `<select>${opt(1, act.a, 'Up')}${opt(0, act.a, 'Down')}</select>`) + field('b', rampLengthSelect(act.b, divisions));
      break;
    case ACT.MOD_RAMP:
      fields = field('b', rampLengthSelect(act.b, divisions));
      break;
    case ACT.CC_RAMP:
      fields = num('a', act.a, 0, 127, 'CC', 'CC number') + field('b', rampLengthSelect(act.b, divisions));
      break;
    case ACT.OCTAVE:
      fields = field('a', `<select>${opt(1, act.a, 'Up')}${opt(0, act.a, 'Down')}</select>`) +
        field('b', `<select>${opt(0, act.b, 'Momentary')}${opt(1, act.b, 'Toggle')}</select>`);
      break;
    default:
      break;
  }
  const learnOut = usesCc(act) ? `<button type="button" class="btn btn-ghost btn-tiny" data-action="learnActOut" ${attrs} title="Learn the CC number">Learn</button>` : '';
  const color = showColor && act.type !== ACT.NONE
    ? num('c', act.c || 0, 0, 127, 'Color', 'Button light color: a raw Launchpad velocity color, 0 = the usual channel-color indicator')
    : '';
  return `<span class="action-editor">${field('type', `<select title="Action">${ACTION_TYPES.map((t) => opt(t.v, act.type, t.l)).join('')}</select>`)}${fields}${color}${learnOut}</span>`;
}

// Clamps and stores a changed action field. Returns true when the editor needs redrawing.
export function applyActionField(act, field, raw) {
  let v = parseInt(raw, 10);
  if (Number.isNaN(v)) return false;
  if (field === 'type') {
    Object.assign(act, defaultAction(v));
    return true;
  }
  act[field] = Math.min(127, Math.max(0, v));
  return false;
}

