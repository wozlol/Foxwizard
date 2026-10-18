// The mapper (firmware 1.40): one list of actions shared by Note to Feature, CC to Feature and the
// Launchpad buttons. An action is { type, a, b }, the same four bytes the firmware keeps.

export const ACT = {
  NONE: 0, NOTE: 1, CC_MOMENTARY: 2, CC_TOGGLE: 3, CC_VALUE: 4, CC_LEVEL: 5, TRANSPORT: 6,
  PROGRAM: 7, PROGRAM_STEP: 8, LIVE_CHANNEL: 9, LIVE_VELOCITY: 10, LOOPER: 11, TAP_TEMPO: 12
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
  { v: ACT.LIVE_CHANNEL, l: 'Live Out Channel' },
  { v: ACT.LIVE_VELOCITY, l: 'Live Velocity' },
  { v: ACT.LOOPER, l: 'Looper' },
  { v: ACT.TAP_TEMPO, l: 'Tap Tempo' }
];

// Sent as MMC, and as MIDI Start, Stop or Continue where MIDI has one.
export const TRANSPORTS = ['Play', 'Stop', 'Continue', 'Pause', 'Record', 'Record Exit', 'Rewind', 'Fast Forward', 'Return to Start'];

export const LOOPER_ACTIONS = ['Button', 'Play/Arm/Rec', 'Stop/Clear/Undo', 'Play', 'Stop', 'Pause', 'Overdub', 'Clear', 'Undo', 'Rec/Arm'];

export const MAP_ENTRIES = 16;

export function defaultAction(type = ACT.NONE) {
  switch (type) {
    case ACT.NOTE: return { type, a: 36, b: 0 };
    case ACT.CC_MOMENTARY: case ACT.CC_TOGGLE: case ACT.CC_VALUE: return { type, a: 20, b: 0 };
    case ACT.CC_LEVEL: return { type, a: 20, b: 64 };
    case ACT.LIVE_VELOCITY: return { type, a: 10, b: 0 }; // 100%
    default: return { type, a: 0, b: 0 };
  }
}

export function actionToBytes(act) {
  const a = act || defaultAction();
  return [a.type & 0x7F, a.a & 0x7F, a.b & 0x7F, 0];
}

export function bytesToAction(b, o) {
  const type = b[o] <= ACT.TAP_TEMPO ? b[o] : ACT.NONE;
  return { type, a: (b[o + 1] || 0) & 0x7F, b: (b[o + 2] || 0) & 0x7F };
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

export function defaultLpActions() {
  return Array.from({ length: 16 }, () => defaultAction());
}

export function usesLooper(act) { return act?.type === ACT.LOOPER; }
export function usesLiveChannel(act) { return act?.type === ACT.LIVE_CHANNEL; }
export function usesCc(act) { return [ACT.CC_MOMENTARY, ACT.CC_TOGGLE, ACT.CC_VALUE, ACT.CC_LEVEL].includes(act?.type); }

// The editor for one action. attrs identifies it: data-act-owner, data-act-index, data-route-index.
export function actionEditorHtml(act, attrs, noteSelectOptions) {
  const opt = (v, cur, l) => `<option value="${v}" ${cur === v ? 'selected' : ''}>${l}</option>`;
  const field = (f, html) => html.replace('<select', `<select data-act-field="${f}" ${attrs}`).replace('<input', `<input data-act-field="${f}" ${attrs}`);
  const num = (f, v, min, max, title) => field(f, `<input type="number" min="${min}" max="${max}" value="${v}" title="${title}">`);
  let fields = '';
  switch (act.type) {
    case ACT.NOTE:
      fields = field('a', `<select title="Note">${noteSelectOptions(act.a)}</select>`);
      break;
    case ACT.CC_MOMENTARY: case ACT.CC_TOGGLE: case ACT.CC_VALUE:
      fields = num('a', act.a, 0, 127, 'CC number');
      break;
    case ACT.CC_LEVEL:
      fields = num('a', act.a, 0, 127, 'CC number') + num('b', act.b, 0, 127, 'Level');
      break;
    case ACT.TRANSPORT:
      fields = field('a', `<select>${TRANSPORTS.map((t, i) => opt(i, act.a, t)).join('')}</select>`);
      break;
    case ACT.PROGRAM:
      fields = num('a', act.a, 0, 127, 'Program');
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
    default:
      break;
  }
  const learnOut = usesCc(act) ? `<button type="button" class="btn btn-ghost btn-tiny" data-action="learnActOut" ${attrs} title="Learn the CC number">Learn</button>` : '';
  return `<span class="action-editor">${field('type', `<select title="Action">${ACTION_TYPES.map((t) => opt(t.v, act.type, t.l)).join('')}</select>`)}${fields}${learnOut}</span>`;
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

