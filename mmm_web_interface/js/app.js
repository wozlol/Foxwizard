import { ACT, MAP_ENTRIES, defaultAction, defaultLpActions, actionEditorHtml, applyActionField, usesLooper, usesLiveChannel } from './mapper.js';
import { DeviceLink, DATA_TYPE_BITS, ROUTE_NAME_MAX_LEN, PORTS, PORT_TRS, PORT_USB, SIDECAR_INPUT_PORTS, SIDECAR_OUTPUT_PORTS, SIDECAR_TRS_PORTS, FEATURE, SCALES, LAYOUTS, defaultScale, ARP_SLOTS, ARP_MODES, ARP_MODE_ORDER, ARP_SQUARES, DIVISIONS, defaultArp, defaultEcho, defaultStutter, defaultLooper, defaultLpQuad, noTrigger, STUTTER_RUNOUTS, snapRunout, defaultPlock } from './midi.js';
import { serializePreset, parsePreset, downloadPreset, readPresetFile, channelsToRangeString } from './presetXml.js';
import { attachDragReorder } from './dragReorder.js';
import { parseHostReport, hostReportSummary, hostReportText } from './hostReport.js';

// ============================================================================================
// Icons
// Shark/octopus art are Regime Radar's "level" mascot characters (icons/mode-shark.png,
// icons/mode-octopus.png), shrunk down to fit the mode toggle's knob circle.
// ============================================================================================
const SHARK_SVG = `<img src="icons/mode-shark.png" alt="Shark — USB Adaptor Mode">`;
const OCTOPUS_SVG = `<img src="icons/mode-octopus.png" alt="Octopus — Router Mode">`;

const GRIP_SVG = `<svg viewBox="0 0 16 24" fill="currentColor" width="14" height="20"><circle cx="4" cy="4" r="1.6"/><circle cx="12" cy="4" r="1.6"/><circle cx="4" cy="12" r="1.6"/><circle cx="12" cy="12" r="1.6"/><circle cx="4" cy="20" r="1.6"/><circle cx="12" cy="20" r="1.6"/></svg>`;

const TRASH_SVG = `<svg viewBox="0 0 20 20" fill="none" width="16" height="16"><path d="M4 6h12M8 6V4h4v2M6 6l.6 10a1 1 0 001 1h4.8a1 1 0 001-1L14 6" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/></svg>`;

// Ported from Regime Radar's staple toggle: 9 hand-tuned keyframes per leg/bar so the bend reads
// as a real staple closing/opening rather than a linear morph. Each array runs straight->bent
// (i.e. "turning on"); turning off just plays the same array in reverse.
const STAPLE_LEFT_STEPS = [
  'M 60 30 L 60 80',
  'M 60 31 Q 61 54 63 66 Q 65 78 68 80',
  'M 60 32 Q 62 47 65 62 Q 68 77 70 80',
  'M 60 38 Q 63 53 69 67 Q 75 81 78 85',
  'M 60 44 Q 65 59 72 72 Q 78 84 82 87',
  'M 60 46 Q 65 61 75 71 Q 85 81 90 82',
  'M 60 48 Q 60 63 71 68 Q 83 73 96 66',
  'M 60 49 Q 60 64 71.5 68 Q 84 71 98 64',
  'M 60 50 Q 60 65 72 68 Q 85 70 100 62'
];
const STAPLE_TOP_STEPS = [
  'M 60 30 Q 120 28 180 30',
  'M 60 31 Q 120 29 180 31',
  'M 60 32 Q 120 30 180 32',
  'M 60 38 Q 120 36 180 38',
  'M 60 44 Q 120 42 180 44',
  'M 60 46 Q 120 44 180 46',
  'M 60 48 Q 120 46 180 48',
  'M 60 49 Q 120 47 180 49',
  'M 60 50 Q 120 48 180 50'
];
const STAPLE_RIGHT_STEPS = [
  'M 180 30 L 180 80',
  'M 180 31 Q 179 54 177 66 Q 175 78 172 80',
  'M 180 32 Q 178 47 175 62 Q 172 77 170 80',
  'M 180 38 Q 177 53 171 67 Q 165 81 162 85',
  'M 180 44 Q 175 59 168 72 Q 162 84 158 87',
  'M 180 46 Q 175 61 165 71 Q 155 81 150 82',
  'M 180 48 Q 180 63 169 68 Q 157 73 144 66',
  'M 180 49 Q 180 64 168.5 68 Q 156 71 142 64',
  'M 180 50 Q 180 65 168 68 Q 155 70 140 62'
];

function stapleSvg(enabled, animate) {
  const color = enabled ? 'var(--amber)' : 'var(--text-faint)';
  // enabled=true (turning on): steps run as defined, straight -> bent.
  // enabled=false (turning off): reversed, bent -> straight. Either way steps[0] is the "from"
  // state and steps[steps.length-1] is always the target (matches `enabled`).
  const left = enabled ? STAPLE_LEFT_STEPS : [...STAPLE_LEFT_STEPS].reverse();
  const top = enabled ? STAPLE_TOP_STEPS : [...STAPLE_TOP_STEPS].reverse();
  const right = enabled ? STAPLE_RIGHT_STEPS : [...STAPLE_RIGHT_STEPS].reverse();

  const part = (steps) => {
    const target = steps[steps.length - 1];
    const d = animate ? steps[0] : target;
    const animateTag = animate
      ? `<animate attributeName="d" values="${steps.join(';')}" dur="0.6s" begin="0.05s" fill="freeze"/>`
      : '';
    return `<path d="${d}" stroke="${color}" stroke-width="10" fill="none" stroke-linecap="round">${animateTag}</path>`;
  };

  return `<svg viewBox="0 -5 240 120" xmlns="http://www.w3.org/2000/svg">
    ${part(left)}
    ${part(top)}
    ${part(right)}
  </svg>`;
}

// ============================================================================================
// State
// ============================================================================================
let nextUid = 1;

function defaultGlobal() {
  return {
    mode: 0, buttonAction: 0, buttonParamA: 64, buttonParamB: 0, buttonToggleMomentary: 0, ledBrightnessStep: 10,
    // Global clock, firmware 1.4 and later (protocol 3)
    clockSource: 0, bpm: 120, beatsPerBar: 4, beatUnit: 4, clockOut: 0, tapType: 0, tapChannel: 16, tapNumber: 0
  };
}

function defaultRoute() {
  return {
    _uid: nextUid++,
    _collapsed: false, // UI-only, never sent to the device or saved in presets
    _ccMapEnabled: false, // UI-only — off means the CC Map Start field just mirrors ccStart (no remap)
    name: '',
    enabled: true,
    // Every port this FoxWizard has right now, so a fresh route is a full passthrough.
    inPorts: PORT_TRS | PORT_USB | (sidecarPresent ? sidecarInputPorts() : 0),
    inputChannels: 0x0001, // ch1 only
    // All top-level input data types on by default (every bit except bit7/aftertouchMap, which is
    // a sub-switch under Poly Aftertouch, not one of the top-level switches the master toggle
    // controls) — a fresh route works as a full passthrough immediately, narrow down from there.
    typeFlags: 0x7F,
    noteStart: 0, noteEnd: 127,
    ccStart: 0, ccEnd: 127,
    outPorts: PORT_TRS | PORT_USB | (sidecarPresent ? sidecarOutputPorts() : 0),
    outputChannels: 0x0001, // ch1 only
    transpose: 0,
    velocityScale: 100,
    ccMapStart: 0,
    atMapCC: 0,
    monoRetrig: false,
    roundRobin: false,
    roundRobinRandom: false,
    commonEnabled: true,
    realtimeEnabled: true,
    clockEnabled: true,
    cpMapEnabled: false,
    cpMapCC: 0,
    features: 0, // FEATURE_* switches, firmware 1.6 and later
    arpSlot: 0
  };
}

const state = {
  presetName: 'Untitled Preset',
  global: defaultGlobal(),
  routes: [],
  // Feature blocks, shared by the route that switches the feature on (firmware 1.6 and later).
  noteMap: [], // Note to Feature: [{ source note, action }]
  ccMap: [], // CC to Feature: [{ source CC, action }]
  lpActions: defaultLpActions(), // the Launchpad's 16 round buttons, an action each
  scale: defaultScale(), // { root 0-11, scaleType }, each route has its own scaleLayout and chordMask
  arps: Array.from({ length: ARP_SLOTS }, defaultArp), // slot settings, route.arpSlot 1-4 points at one
  echo: defaultEcho(),
  stutter: defaultStutter(),
  looper: defaultLooper(),
  plock: defaultPlock() // Per-Note Parameter Lock's Delay and Glide, the locks themselves live on the device
};

const device = new DeviceLink();
let dirty = false;

// ============================================================================================
// Overlay / toast helpers
// ============================================================================================
const overlay = document.getElementById('radarOverlay');
const overlayText = document.getElementById('radarOverlayText');
const overlayProgress = document.getElementById('radarOverlayProgress');

function showOverlay(text) {
  overlayText.textContent = text;
  overlayProgress.textContent = '';
  if (overlay.hidden) {
    overlay.hidden = false;
    radarBlip.start();
  }
}
function setOverlayProgress(done, total) {
  overlayProgress.textContent = total ? `${done} / ${total}` : '';
}
function hideOverlay() {
  overlay.hidden = true;
  radarBlip.stop();
}

// The radar's blip wanders and flashes as the sweep passes over it. Ported from Regime Radar's info
// modal (RegimeReferenceModal.js initRadarAnimation), without its crown.
const radarBlip = (() => {
  const CYCLE = 3000;    // ms per sweep turn, matches .mini-sweep
  const FLASH_MS = 4500; // ping length
  const STEP_MAX = 8;    // px per hop
  const MOVE_MIN = 2000; // ms between hops
  const MOVE_MAX = 3500;
  const R = 32.5, rDot = 2; // radar and dot radii
  const sweep = overlay.querySelector('.mini-sweep');
  const blip = overlay.querySelector('.mini-blip');
  let x, y, flashTimer, flashLoop, wanderTimer;

  // The sweep's current angle, degrees clockwise from the right axis.
  function sweepAngle() {
    const m = new DOMMatrixReadOnly(getComputedStyle(sweep).transform);
    return (Math.atan2(m.b, m.a) * 180 / Math.PI + 360) % 360;
  }
  function pulse() {
    blip.style.animation = 'none';
    void blip.offsetWidth; // restarts the animation
    blip.style.animation = `blipFlash ${FLASH_MS}ms ease-out`;
  }
  // Flashes when the bright edge of the sweep reaches the dot.
  function syncFlash() {
    clearTimeout(flashTimer);
    clearInterval(flashLoop);
    const dotAngle = (Math.atan2(y - R, x - R) * 180 / Math.PI + 360) % 360;
    const wait = ((dotAngle - sweepAngle() + 360) % 360) / 360 * CYCLE;
    flashTimer = setTimeout(() => { pulse(); flashLoop = setInterval(pulse, CYCLE); }, wait);
  }
  function wander() {
    let nx, ny;
    do { // a short random hop that stays inside the radar
      const t = Math.random() * 2 * Math.PI, d = Math.random() * STEP_MAX;
      nx = x + d * Math.cos(t);
      ny = y + d * Math.sin(t);
    } while (Math.hypot(nx - R, ny - R) > R - rDot);
    x = nx; y = ny;
    blip.style.left = `${x - rDot}px`;
    blip.style.top = `${y - rDot}px`;
    blip.addEventListener('transitionend', syncFlash, { once: true });
    wanderTimer = setTimeout(wander, MOVE_MIN + Math.random() * (MOVE_MAX - MOVE_MIN));
  }
  return {
    start() {
      x = 0.71 * 65 + rDot;
      y = 0.47 * 65 + rDot;
      blip.style.left = `${x - rDot}px`;
      blip.style.top = `${y - rDot}px`;
      syncFlash();
      wanderTimer = setTimeout(wander, 1500);
    },
    stop() {
      clearTimeout(flashTimer);
      clearInterval(flashLoop);
      clearTimeout(wanderTimer);
    }
  };
})();

let toastStack = document.querySelector('.toast-stack');
if (!toastStack) {
  toastStack = document.createElement('div');
  toastStack.className = 'toast-stack';
  document.body.appendChild(toastStack);
}
// What a pasted report should say about the software that made it.
const APP_VERSION = document.querySelector('.app-footer')?.textContent.match(/v\d+\.\d+/)?.[0] || '';
function versionLine() {
  const v = deviceVersion();
  return `Patchling ${APP_VERSION}, firmware ${v ? versionText(v) : 'unknown'}`;
}

// Puts text on the clipboard, the older way too where the page isn't a secure context.
async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch (err) {
    const box = document.createElement('textarea');
    box.value = text;
    box.style.position = 'fixed';
    box.style.opacity = '0';
    document.body.appendChild(box);
    box.select();
    let ok = false;
    try { ok = document.execCommand('copy'); } catch (e2) { /* no clipboard */ }
    box.remove();
    return ok;
  }
}

// sticky: stays until dismissed, with a Copy button (copying copyWith when given, else the message),
// an OK button and an x in the corner, for things worth reading and passing on after the fact.
function toast(message, kind, sticky, copyWith) {
  const el = document.createElement('div');
  el.className = 'toast' + (kind ? ` toast-${kind}` : '') + (sticky ? ' toast-sticky' : '');
  if (!sticky) {
    el.textContent = message;
    toastStack.appendChild(el);
    setTimeout(() => el.remove(), 3200);
    return;
  }
  const text = document.createElement('div');
  text.className = 'toast-text';
  text.textContent = message;
  const close = document.createElement('button');
  close.type = 'button';
  close.className = 'toast-close';
  close.title = 'Dismiss';
  close.setAttribute('aria-label', 'Dismiss');
  close.textContent = '\u00D7';
  close.addEventListener('click', () => el.remove());
  const copy = document.createElement('button');
  copy.type = 'button';
  copy.className = 'btn btn-ghost btn-tiny';
  copy.textContent = 'Copy';
  copy.addEventListener('click', async () => {
    copy.textContent = (await copyText(copyWith || message)) ? 'Copied' : 'Copy failed';
    setTimeout(() => { copy.textContent = 'Copy'; }, 1600);
  });
  const ok = document.createElement('button');
  ok.type = 'button';
  ok.className = 'btn btn-primary btn-tiny';
  ok.textContent = 'OK';
  ok.addEventListener('click', () => el.remove());
  const actions = document.createElement('div');
  actions.className = 'toast-actions';
  actions.append(copy, ok);
  el.append(close, text, actions);
  toastStack.appendChild(el);
}

const saveToDeviceBtn = document.getElementById('saveToDeviceBtn');
function markDirty() {
  dirty = true;
  saveToDeviceBtn.classList.add('has-unsaved');
  scheduleAutosave();
}

// Autosave: a change goes to the device a moment after it's made, without the overlay. Changes made
// while a save is going out are sent once it finishes.
const AUTOSAVE_DELAY_MS = 700;
const autosaveSwitch = document.getElementById('autosaveSwitch');
let autosaveTimer = null;
let autosaving = false;
let autosaveAgain = false;
try { autosaveSwitch.checked = localStorage.getItem('patchling.autosave') === '1'; } catch (err) { /* no storage */ }
autosaveSwitch.addEventListener('change', () => {
  try { localStorage.setItem('patchling.autosave', autosaveSwitch.checked ? '1' : '0'); } catch (err) { /* no storage */ }
  if (autosaveSwitch.checked && dirty) scheduleAutosave();
});

function scheduleAutosave() {
  if (!autosaveSwitch.checked || !device.connected) return;
  clearTimeout(autosaveTimer);
  autosaveTimer = setTimeout(autosave, AUTOSAVE_DELAY_MS);
}

async function autosave() {
  if (!autosaveSwitch.checked || !device.connected || !dirty) return;
  if (autosaving || updateInProgress || !overlay.hidden) { autosaveAgain = true; return; }
  if (state.routes.length > routeLimit()) return; // the Save button explains this one
  autosaving = true;
  autosaveAgain = false;
  dirty = false; // a change made during the save marks it dirty again
  try {
    await device.saveAll(state.global, state.routes, { noteMap: state.noteMap, ccMap: state.ccMap, lpActions: state.lpActions, scale: state.scale, arps: state.arps, echo: state.echo, stutter: state.stutter, looper: state.looper, plock: state.plock });
    if (!dirty) clearDirty();
  } catch (err) {
    dirty = true;
    toast('Autosave failed, use Save to Device', 'error');
  } finally {
    autosaving = false;
    if (autosaveAgain || dirty) scheduleAutosave();
  }
}
function clearDirty() {
  dirty = false;
  saveToDeviceBtn.classList.remove('has-unsaved');
}

// ============================================================================================
// Dialog
// ============================================================================================
const dialogEl = document.getElementById('dialog');
const dialogTitle = document.getElementById('dialogTitle');
const dialogBody = document.getElementById('dialogBody');
const dialogActions = document.getElementById('dialogActions');

function closeDialog() { dialogEl.hidden = true; }

/** actions: [{ label, kind: 'primary'|'accent'|'ghost', href?, onClick? }]. Every action closes the dialog. */
function openDialog(title, bodyHtml, actions) {
  dialogTitle.textContent = title;
  dialogBody.innerHTML = bodyHtml;
  dialogActions.innerHTML = '';
  for (const a of actions) {
    const el = document.createElement(a.href ? 'a' : 'button');
    el.className = `btn btn-${a.kind || 'ghost'}`;
    el.textContent = a.label;
    if (a.href) { el.href = a.href; el.target = '_blank'; el.rel = 'noopener'; } else { el.type = 'button'; }
    el.addEventListener('click', () => { closeDialog(); if (a.onClick) a.onClick(); });
    dialogActions.appendChild(el);
  }
  dialogEl.hidden = false;
  dialogActions.lastElementChild?.focus();
}

dialogEl.addEventListener('click', (e) => { if (e.target === dialogEl) closeDialog(); });
document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && !dialogEl.hidden) closeDialog(); });

// ============================================================================================
// Firmware check
// firmware/firmware.json describes the newest firmware: { major, minor, bin, size, md5 }.
// Firmware older than FW_UPDATE_MIN can't update over MIDI, so it's updated once by hand with the
// FIRMWARE pads. Everything from FW_UPDATE_MIN on updates from the yellow pill.
// ============================================================================================
const FW_UPDATE_MIN = { major: 1, minor: 2 };
const FIRMWARE_DOWNLOAD_URL = 'https://woz.lol/foxwizard-midi-multitool-firmware/';
const FIRMWARE_DOWNLOAD_LABEL = 'woz.lol/foxwizard-midi-multitool-firmware';
const RECONNECT_WAIT_MS = 30000;

// Test channel: Patchling served from this computer (serve.sh) or opened with ?test offers the test
// builds in firmware/test/ (build_firmware.sh --test-channel), falling back to the live firmware if
// there are none. The live site never offers test builds.
const TEST_CHANNEL = ['localhost', '127.0.0.1', '[::1]'].includes(location.hostname) || new URLSearchParams(location.search).has('test');
let firmwareDir = 'firmware/';

let latestFirmware = null; // false if firmware.json couldn't be loaded
function fetchFirmwareJson(dir) {
  return fetch(`${dir}firmware.json`, { cache: 'no-store' })
    .then((r) => (r.ok ? r.json() : Promise.reject(new Error('firmware.json'))))
    .then((j) => { latestFirmware = j; firmwareDir = dir; });
}
const latestFirmwareReady = (TEST_CHANNEL ? fetchFirmwareJson('firmware/test/').catch(() => fetchFirmwareJson('firmware/')) : fetchFirmwareJson('firmware/'))
  .catch(() => { latestFirmware = false; });
if (TEST_CHANNEL) document.getElementById('testTag').hidden = false;

function compareVersion(a, b) { return a.major !== b.major ? a.major - b.major : a.minor - b.minor; }
function versionText(v) { return `v${v.major}.${v.minor}`; }
function deviceVersion() {
  const d = device.deviceInfo;
  return d ? { major: d.fwMajor, minor: d.fwMinor } : null;
}

/** 'current', 'updatable' (over MIDI), 'manual' (FIRMWARE pads), or 'unknown' (couldn't check). */
function firmwareStatus() {
  const v = deviceVersion();
  if (!v || !latestFirmware) return 'unknown';
  if (compareVersion(v, latestFirmware) >= 0) return 'current';
  return compareVersion(v, FW_UPDATE_MIN) >= 0 ? 'updatable' : 'manual';
}

function openUpdateDialog() {
  const unsavedWarning = dirty
    ? `<p class="dialog-warn">You have changes that aren't saved to the FoxWizard. When it reconnects after the update, Patchling reloads its saved settings, which replaces them. Click Cancel, then Save to Device, to keep them.</p>`
    : '';
  openDialog('Firmware update', `
    <p>This FoxWizard has firmware ${versionText(deviceVersion())}. Firmware ${versionText(latestFirmware)} is available.</p>
    <p>The update takes a few seconds. Keep the FoxWizard plugged in until it restarts and reconnects. Saved settings and routes are kept.</p>
    ${unsavedWarning}`,
  [
    { label: 'Cancel', kind: 'ghost' },
    { label: 'Update Now', kind: 'accent', onClick: runFirmwareUpdate }
  ]);
}

function openManualUpdateDialog() {
  openDialog('Firmware update', `
    <p>This FoxWizard has firmware ${versionText(deviceVersion())}, which did not yet have the easy-update ability. Firmware ${versionText(latestFirmware)} is available. If you would please update it manually this one time, it will update easily right here next time with just one click.</p>
    <ol>
      <li>Download the latest <code>.uf2</code> firmware file from <a href="${FIRMWARE_DOWNLOAD_URL}" target="_blank" rel="noopener">${FIRMWARE_DOWNLOAD_LABEL}</a>.</li>
      <li>Unplug the FoxWizard.</li>
      <li>Short the two pads labeled <strong>FIRMWARE</strong> on the circuit board together, with tweezers or a paperclip, and keep them shorted while you plug it into your computer.</li>
      <li>A drive named <strong>RPI-RP2</strong> appears. Release the short.</li>
      <li>Drag the <code>.uf2</code> file onto the RPI-RP2 drive. The FoxWizard restarts by itself when the copy finishes.</li>
    </ol>
    <p>Saved settings and routes are kept.</p>`,
  [
    { label: 'Close, Do This Later', kind: 'ghost' },
    { label: 'Get Firmware', kind: 'accent', href: FIRMWARE_DOWNLOAD_URL }
  ]);
}

function promptForFirmwareIfNeeded() {
  const status = firmwareStatus();
  if (status === 'updatable') openUpdateDialog();
  else if (status === 'manual') openManualUpdateDialog();
}

// Counts connections, so the update can wait for the device to come back after it restarts.
let connectionCount = 0;
let updateInProgress = false;

function waitForReconnect(fromCount, timeoutMs) {
  return new Promise((resolve) => {
    const started = Date.now();
    const timer = setInterval(() => {
      if (connectionCount > fromCount && device.connected) { clearInterval(timer); resolve(true); }
      else if (Date.now() - started > timeoutMs) { clearInterval(timer); resolve(false); }
    }, 200);
  });
}

async function runFirmwareUpdate() {
  if (!device.connected || !latestFirmware) return;
  updateInProgress = true;
  const countBefore = connectionCount;
  showOverlay('Updating firmware. Keep the FoxWizard plugged in.');
  try {
    const res = await fetch(`${firmwareDir}${latestFirmware.bin}`, { cache: 'no-store' });
    if (!res.ok) throw new Error('download');
    const bin = new Uint8Array(await res.arrayBuffer());
    if (bin.length !== latestFirmware.size) throw new Error('size');
    await device.updateFirmware(bin, latestFirmware.md5, (sent, total) => {
      overlayProgress.textContent = `${Math.floor((sent * 100) / total)}%`;
    });
  } catch (err) {
    if (!err.fwEndUnconfirmed) {
      updateInProgress = false;
      hideOverlay();
      toast('Firmware update failed. The FoxWizard kept its current firmware.', 'error');
      return;
    }
    // The last step went unconfirmed, but the device may still be installing. Wait and see.
  }

  overlayText.textContent = 'Installing. The FoxWizard restarts by itself.';
  overlayProgress.textContent = '';
  const back = await waitForReconnect(countBefore, RECONNECT_WAIT_MS);
  updateInProgress = false;
  hideOverlay();
  if (!back) {
    toast("The FoxWizard didn't reconnect. Unplug it and plug it back in.", 'error');
  } else if (firmwareStatus() === 'current') {
    toast(`Firmware updated to ${versionText(deviceVersion())}`, 'success');
  } else {
    toast(`Update didn't install, the FoxWizard still has ${versionText(deviceVersion())}`, 'error');
  }
}

// ============================================================================================
// Connection handling
// Green: connected, firmware up to date. Yellow: connected, firmware update available.
// Red: not connected, click to try again.
// ============================================================================================
const connectionPill = document.getElementById('connectionPill');
const connectionLabel = document.getElementById('connectionLabel');
let connectionState = 'disconnected';

// Sidecar state, from GET_STATUS (firmware 1.4 and later), checked every 2 s while connected.
// Customers buy a FoxWizard or a FoxWizard Host Edition, so anything they see says "Host" or
// "Host Edition", never "sidecar".
const SIDECAR_POLL_MS = 2000;
let sidecarPresent = false;
let sidecarHasTrs = true; // true once a TRS board is added on top of the bare sidecar module
let hostPortMask = 0; // bit 0 = a device on USB Host 1, up to bit 6 for USB Host 7
let hostSlots = 4;    // the USB Host slots the host brain has: 7 from host firmware 6, 4 before
// What the host brain says about its hubs (1.71, host firmware 4): jacks is how many downstream ports
// the attached hubs add up to, 0 with no hub. Until it says, all 4 USB Host ports show.
let hostHub = { known: false, jacks: 0, hubs: 0, waiting: 0 };
let hostPortsSeen = 0; // the most USB Host ports needed since connecting, so a chip doesn't vanish when a device is unplugged
let hostPortsShown = 4;
let hostRestartCount = null; // null until the first status after connecting, so only new restarts are announced
let lastHostStatus = null;   // newest GET_STATUS reply, for the badge click

function restartMessage(status) {
  switch (status.restartReason) {
    case 2: return `Host Edition restarted to recover: USB Host ${status.restartPort} stopped accepting data`;
    case 3: return 'Host Edition restarted to recover: USB data sequence error';
    case 4: return 'Host Edition reset its USB ports after a device change';
    case 5: return 'Host Edition restarted to recover: a USB device stalled';
    default: return 'Host Edition restarted to recover: its USB host stopped responding';
  }
}

// 1.63: the Host Edition's own report of its last restart, null when the firmware can't give one.
async function fetchHostReport() {
  const v = deviceVersion();
  if (!v || v.major < 1 || (v.major === 1 && v.minor < 63)) return null;
  try {
    const { bytes } = await device.getHostReport();
    return parseHostReport(bytes);
  } catch (err) { return null; }
}

async function showHostReport() {
  const report = await fetchHostReport();
  const text = hostReportText(report, `${restartMessage(lastHostStatus)}.\n${versionLine()}`);
  const pre = text.replace(/[&<>]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
  openDialog('Host Edition Report', `<pre class="host-report">${pre}</pre>`, [
    { label: 'Copy', kind: 'ghost', onClick: async () => { if (await copyText(text)) toast('Report copied', 'success'); } },
    { label: 'Close', kind: 'primary' }
  ]);
}

// Polls in a row that found no Host Edition. A host brain restarting is gone for a moment, so a
// couple of misses keep the ports showing instead of redrawing every route and back.
let sidecarMisses = 0;

async function refreshSidecarStatus() {
  if (!device.connected || updateInProgress || !overlay.hidden) return;
  let status;
  try { status = await device.getStatus(); } catch (err) { return; }
  if (status.sidecar) sidecarMisses = 0;
  else if (sidecarPresent && ++sidecarMisses < 3) return;
  const sidecarChanged = status.sidecar !== sidecarPresent;
  const trsChanged = status.hasTrs !== sidecarHasTrs;
  const portsChanged = status.hostPortMask !== hostPortMask;
  const waitingBefore = hostHub.waiting;
  sidecarPresent = status.sidecar;
  sidecarHasTrs = status.hasTrs;
  hostPortMask = status.hostPortMask;
  hostSlots = status.hostFwVersion >= 6 ? 7 : 4;
  hostHub = { known: status.sidecar && status.hubKnown, jacks: status.hubJacks, hubs: status.hubCount, waiting: status.hostWaiting };
  const hostShownChanged = updateHostPortsShown();
  if (hostHub.known && hostHub.waiting > waitingBefore) {
    toast(`${hostHub.waiting} USB device${hostHub.waiting > 1 ? 's' : ''} plugged in with no free USB Host port. ${hostHub.waiting > 1 ? 'They start' : 'It starts'} working when a port frees up.`, 'warn');
  }
  lastHostStatus = status;
  if (status.sidecar) {
    if (hostRestartCount !== null && status.restartCount > hostRestartCount) {
      // USB driver resets (data sequence 3, device change 4) recover on their own: a brief yellow note.
      // Every other reason stays up in red until clicked.
      // The report arrives over the next second or two, after the restart itself.
      const time = new Date().toLocaleTimeString();
      setTimeout(async () => {
        const report = await fetchHostReport();
        const summary = hostReportSummary(report);
        const more = summary ? ` ${summary} Click the Connected badge for the full report.` : '';
        if (status.restartReason === 3 || status.restartReason === 4) {
          toast(`${restartMessage(status)}.${more}`, 'warn');
        } else {
          // Copy takes the whole report, with the versions, so a paste says everything.
          const headline = `${time} ${restartMessage(status)}.\n${versionLine()}`;
          toast(`${time} ${restartMessage(status)}.${more}`, 'error', true, report ? hostReportText(report, headline) : headline);
        }
      }, 2500);
    }
    hostRestartCount = status.restartCount;
  }
  updateClockReadout(status);
  if (sidecarChanged || trsChanged || hostShownChanged) {
    renderRoutes();
    renderClockRow();
    renderConnectionPill();
    if (sidecarChanged) toast(sidecarPresent ? 'Host Edition ports connected' : 'Host Edition ports disconnected', sidecarPresent ? 'success' : 'error');
  } else if (portsChanged) {
    updatePortChipsLive();
  }
}
setInterval(refreshSidecarStatus, SIDECAR_POLL_MS);

function renderConnectionPill() {
  connectionPill.className = 'connection-pill';
  const s = connectionState;
  if (s === 'connected') {
    const status = firmwareStatus();
    const version = versionText(deviceVersion());
    if (status === 'updatable' || status === 'manual') {
      connectionPill.classList.add('outdated');
      connectionLabel.textContent = `Connected · Firmware ${version}${sidecarPresent ? ' · Host' : ''} · Update`;
      connectionPill.title = `Click to update to firmware ${versionText(latestFirmware)}`;
    } else {
      connectionPill.classList.add('connected');
      connectionLabel.textContent = `Connected · Firmware ${version}${sidecarPresent ? ' · Host' : ''}`;
      connectionPill.title = status === 'current' ? 'Firmware is up to date' : "Couldn't check for firmware updates";
    }
  } else if (s === 'connecting') {
    connectionPill.classList.add('connecting');
    connectionLabel.textContent = 'Connecting…';
    connectionPill.title = '';
  } else if (s === 'unsupported') {
    connectionPill.classList.add('disconnected');
    connectionLabel.textContent = 'Web MIDI not supported — use Chrome';
    connectionPill.title = '';
  } else if (s === 'denied') {
    connectionPill.classList.add('disconnected');
    connectionLabel.textContent = 'MIDI access denied';
    connectionPill.title = 'Click to try again';
  } else {
    connectionPill.classList.add('disconnected');
    connectionLabel.textContent = 'Not connected';
    connectionPill.title = 'Click to try connecting again';
  }
}

device.addEventListener('status', async (e) => {
  connectionState = e.detail.state;
  if (connectionState !== 'connected') {
    hostRestartCount = null;
    lastHostStatus = null;
    hostHub = { known: false, jacks: 0, hubs: 0, waiting: 0 };
    sidecarMisses = 0;
    updateHostPortsShown();
  }
  if (connectionState !== 'connected' && sidecarPresent) {
    sidecarPresent = false;
    hostPortMask = 0;
    renderRoutes();
  }
  renderConnectionPill();
  if (connectionState !== 'connected') return;

  connectionCount++;
  if (!updateInProgress) toast('Device connected', 'success');
  await refreshSidecarStatus();
  await loadFromDevice(true);
  await latestFirmwareReady;
  renderConnectionPill();
  if (!updateInProgress) promptForFirmwareIfNeeded();
});

connectionPill.addEventListener('click', async () => {
  if (updateInProgress || connectionState === 'connecting') return;
  if (connectionState === 'unsupported') {
    toast('Web MIDI needs Chrome or another Chromium-based browser', 'error');
    return;
  }
  if (connectionState !== 'connected') {
    await device.retryConnect();
    if (!device.connected) toast('No FoxWizard found. Check the USB cable.', 'error');
    return;
  }
  await latestFirmwareReady;
  if (sidecarPresent && lastHostStatus?.restartCount > 0) {
    await showHostReport();
    return;
  }
  const status = firmwareStatus();
  if (status === 'current') toast(`Firmware ${versionText(deviceVersion())} is up to date`, 'success');
  else if (status === 'unknown') toast("Couldn't check for firmware updates", 'error');
  else promptForFirmwareIfNeeded();
});

// ============================================================================================
// Global settings card
// ============================================================================================
const globalCard = document.getElementById('globalSettingsCard');

const BUTTON_ACTIONS = [
  { value: 0, label: 'Panic (default)' },
  { value: 11, label: 'Looper', protocol: 4 },
  { value: 1, label: 'Pitch Up' },
  { value: 2, label: 'Pitch Down' },
  { value: 3, label: 'Mod' },
  { value: 4, label: 'CC Momentary' },
  { value: 5, label: 'CC Toggle' },
  { value: 6, label: 'Octave Up' },
  { value: 7, label: 'Octave Down' },
  { value: 8, label: 'Note Momentary' },
  { value: 9, label: 'Note Toggle' },
  { value: 10, label: 'Tap Tempo', protocol: 3 }
];

// Actions a connected device's firmware supports. Not connected, everything shows.
function availableButtonActions() {
  return BUTTON_ACTIONS.filter((a) => !a.protocol || !device.connected || device.protocolVersion >= a.protocol);
}

// C3 = 60 (Ableton/Logic/FL convention) — there's no universal standard for which octave number
// note 60 gets, so flag this if a different convention (e.g. Cubase-style C4=60) is expected.
const NOTE_LETTER_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
function noteName(n) {
  const octave = Math.floor(n / 12) - 2;
  return `${n} ${NOTE_LETTER_NAMES[n % 12]}${octave}`;
}
function noteSelectOptions(selected) {
  let opts = '';
  for (let n = 0; n <= 127; n++) opts += `<option value="${n}" ${selected === n ? 'selected' : ''}>${noteName(n)}</option>`;
  return opts;
}

function channelSelectOptions(selected) {
  let opts = '';
  for (let c = 1; c <= 16; c++) opts += `<option value="${c - 1}" ${selected === c - 1 ? 'selected' : ''}>${c}</option>`;
  opts += `<option value="16" ${selected === 16 ? 'selected' : ''}>All</option>`;
  return opts;
}

// Wraps a label+control pair so it can be centered as a single unit within a taller .mini-box —
// see the .mini-box-main CSS for how the centering (and, for boxes with a trailing .note-hint,
// bottom-pinning that hint) actually works.
function miniBoxMain(label, controlHtml) {
  return `<div class="mini-box-main"><p class="mini-box-label" title="${label}">${label}</p>${controlHtml}</div>`;
}

function renderGlobalCard() {
  const g = state.global;
  const isUsbAdaptor = g.mode === 0;

  let buttonExtraFields = '';
  if ([1, 2, 3].includes(g.buttonAction)) {
    // Pitch/Mod ramp length (buttonParamA, 1.58): 0 Instant, else a musical division + 1, the same
    // list the mapped ramps use, up to a whole bar.
    const lenOpt = (v, label) => `<option value="${v}" ${g.buttonParamA === v ? 'selected' : ''}>${label}</option>`;
    buttonExtraFields = `
      <div class="mini-box">${miniBoxMain('Length', `<select data-global-field="buttonParamA">${lenOpt(0, 'Instant')}${DIVISIONS.map((d, i) => lenOpt(i + 1, d)).join('')}</select>`)}</div>
      <div class="mini-box">${miniBoxMain('Channel', `<select data-global-field="buttonParamB">${channelSelectOptions(g.buttonParamB)}</select>`)}</div>`;
  } else if ([4, 5].includes(g.buttonAction)) {
    buttonExtraFields = `
      <div class="mini-box">${miniBoxMain('CC Number', `<input type="number" min="0" max="127" value="${g.buttonParamA}" data-global-field="buttonParamA">`)}</div>
      <div class="mini-box">${miniBoxMain('Channel', `<select data-global-field="buttonParamB">${channelSelectOptions(g.buttonParamB)}</select>`)}</div>`;
  } else if ([6, 7].includes(g.buttonAction)) {
    buttonExtraFields = `
      <div class="mini-box">${miniBoxMain('Behavior', `<select data-global-field="buttonToggleMomentary">
          <option value="0" ${g.buttonToggleMomentary == 0 ? 'selected' : ''}>Momentary</option>
          <option value="1" ${g.buttonToggleMomentary == 1 ? 'selected' : ''}>Toggle</option>
        </select>`)}</div>
      <div class="mini-box">${miniBoxMain('Channel', `<select data-global-field="buttonParamB">${channelSelectOptions(g.buttonParamB)}</select>`)}</div>`;
  } else if ([8, 9].includes(g.buttonAction)) {
    buttonExtraFields = `
      <div class="mini-box">${miniBoxMain('Note', `<select data-global-field="buttonParamA">${noteSelectOptions(g.buttonParamA)}</select>`)}</div>
      <div class="mini-box">${miniBoxMain('Channel', `<select data-global-field="buttonParamB">${channelSelectOptions(g.buttonParamB)}</select>`)}</div>`;
  } else if (g.buttonAction === 11) {
    buttonExtraFields = '';
  }

  const brightnessPct = g.ledBrightnessStep * 10;
  const knobAngle = -150 + (g.ledBrightnessStep / 10) * 300;
  const allCollapsed = state.routes.length > 0 && state.routes.every((r) => r._collapsed);

  renderClockRow();
  renderLooperRow();
  globalCard.innerHTML = `
    <div class="global-card-header">
      <h2 class="main-mode-heading">MAIN MODE</h2>
      <span class="route-header-spacer"></span>
      <span class="staple-toggle" id="masterStaple" data-action="toggleAllCollapsed" title="${allCollapsed ? 'All routes collapsed — click to open all' : 'Click to collapse all routes'}">${stapleSvg(allCollapsed)}</span>
      <button class="btn btn-icon btn-danger" id="resetAllBtn" title="Reset everything — clears all routes and restores default settings">${TRASH_SVG}</button>
    </div>
    <div class="mode-toggle-wrap">
        <div class="mode-toggle-labels">
          <div class="mode-toggle-side left ${isUsbAdaptor ? 'active' : ''}">
            <span class="mode-name">ADAPTOR</span>
            <span class="mode-desc">Factory default USB→DIN / DIN→USB Adaptor.</span>
          </div>
          <div class="mode-toggle-side right ${isUsbAdaptor ? '' : 'active'}">
            <span class="mode-name">ROUTER</span>
            <span class="mode-desc">DIN &amp; USB merged and broadcast to both ports.</span>
          </div>
        </div>
        <div class="mode-toggle" data-mode="${isUsbAdaptor ? 'usbAdaptor' : 'router'}" id="modeToggle" role="button" tabindex="0" aria-label="Toggle mode">
          <div class="mode-toggle-knob">${isUsbAdaptor ? SHARK_SVG : OCTOPUS_SVG}</div>
        </div>
        <p class="note-hint">Custom routes below override the main mode setting above.</p>
      </div>

    <div class="settings-grid">
      <div class="mini-box">
        ${miniBoxMain('Button Action', `<select data-global-field="buttonAction">
          ${availableButtonActions().map((a) => `<option value="${a.value}" ${g.buttonAction === a.value ? 'selected' : ''}>${a.label}</option>`).join('')}
        </select>`)}
      </div>
      ${buttonExtraFields}
      <div class="mini-box lights-box">
        <div class="lights-info">
          <p class="mini-box-label">Lights</p>
          <div class="knob-readout" id="knobReadout">${brightnessPct}%</div>
        </div>
        <div class="knob" id="brightnessKnob">
          <div class="knob-indicator" style="transform: rotate(${knobAngle}deg)"></div>
        </div>
      </div>
    </div>
  `;

  wireModeToggle();
  wireKnob();
  wireGlobalHeader();
}

// ============================================================================================
// Looper row (green). Always shown: it records unrouted channels by default, so it works without
// changing Button Action. The single-button Looper gesture still needs Button Action set to
// Looper; everything else here is reachable from any route's Note/CC to Feature mapping too.
// ============================================================================================
const looperRow = document.getElementById('looperRow');

// Whether the looper is reachable at all right now: Button Action set to it, or mapped on any
// Note/CC to Feature entry or Launchpad button (all three lists are global, shared by the whole
// device, like the firmware's own tables). Until one of these is true there's nothing to record
// with, so the row stays collapsed to just its title and hint instead of showing dead settings.
function looperMapped() {
  // A mapping only counts while some route has that feature switched on, so the default
  // Launchpad table's two Looper buttons don't count with no Launchpad route.
  const featureOn = (bit) => state.routes.some((r) => r.features & bit);
  return state.global.buttonAction === 11 || (state.looper.trigger?.triggerType || 0) !== 0 ||
    (featureOn(FEATURE.LAUNCHPAD) && state.lpActions.some(usesLooper)) ||
    (featureOn(FEATURE.NOTE_TO_CC) && state.noteMap.some((e) => usesLooper(e.action))) ||
    (featureOn(FEATURE.CC_MAP) && state.ccMap.some((e) => usesLooper(e.action)));
}

// The looper's own one-button trigger: a note or CC that works like the front button in Looper
// mode. Shown even while the row is collapsed, since setting it is itself a way in.
function looperTriggerHtml() {
  const t = state.looper.trigger || noTrigger();
  const opt = (v, cur, label) => `<option value="${v}" ${cur === v ? 'selected' : ''}>${label}</option>`;
  let channels = opt(16, t.triggerChannel, 'Any');
  for (let c = 0; c < 16; c++) channels += opt(c, t.triggerChannel, c + 1);
  const number = t.triggerType === 1
    ? `<select data-looper-trigger="triggerNumber">${noteSelectOptions(t.triggerNumber)}</select>`
    : `<input type="number" min="0" max="127" value="${t.triggerNumber}" data-looper-trigger="triggerNumber">`;
  return `<div class="looper-trigger">
      <span class="looper-trigger-label">Looper Button</span>
      <select data-looper-trigger="triggerType">${opt(0, t.triggerType, 'None')}${opt(1, t.triggerType, 'Note')}${opt(2, t.triggerType, 'CC')}</select>
      ${t.triggerType ? `<select data-looper-trigger="triggerChannel" title="Channel">${channels}</select>${number}` : ''}
      <button type="button" class="btn btn-ghost btn-tiny" data-action="learnLooperTrigger">Learn</button>
    </div>`;
}

looperRow.addEventListener('change', (e) => {
  const field = e.target.dataset.looperTrigger;
  if (!field) return;
  const t = (state.looper.trigger = state.looper.trigger || noTrigger());
  let v = parseInt(e.target.value, 10);
  if (Number.isNaN(v)) return;
  t[field] = field === 'triggerNumber' ? Math.min(127, Math.max(0, v)) : v;
  markDirty();
  renderRoutes(); // the trigger can make the looper reachable, which the route Looper switches follow
});

looperRow.addEventListener('click', async (e) => {
  const btn = e.target.closest('[data-action="learnLooperTrigger"]');
  if (!btn) return;
  if (!device.connected) { toast('Not connected to a device', 'error'); return; }
  btn.disabled = true;
  btn.textContent = 'Play a note or move a control…';
  try {
    const r = await device.learn();
    state.looper.trigger = { triggerType: r.type, triggerChannel: r.channel, triggerNumber: r.number };
    markDirty();
    toast(`Learned ${r.type === 1 ? `note ${noteName(r.number)}` : `CC ${r.number}`} on channel ${r.channel + 1}`, 'success');
  } catch (err) {
    toast('Nothing heard in 10 seconds', 'error');
  }
  renderRoutes();
});

function renderLooperRow() {
  looperRow.hidden = false;
  const l = state.looper;
  const mapped = looperMapped();
  const opt = (v, cur, label) => `<option value="${v}" ${cur === v ? 'selected' : ''}>${label}</option>`;
  // Title at top, control under — same shape as the Length/Quantize boxes beside them, instead of
  // the switch's own inline label making these four look like a different kind of control.
  const sw = (field, on, off = false) => `<label class="switch switch-green looper-switch">
      <input type="checkbox" data-looper-field="${field}" ${on ? 'checked' : ''} ${off ? 'disabled' : ''}>
      <span class="switch-track"><span class="switch-thumb"></span></span>
    </label>`;
  looperRow.innerHTML = `
    <div class="looper-row-header">
      <h2 class="looper-row-heading">LOOPER</h2>
      <p class="note-hint">Set Button Action to Looper, or use 'Note/CC to Feature' and select Looper.</p>
      ${looperTriggerHtml()}
    </div>
    ${mapped ? `<div class="looper-settings">${[
      ['Length', `<select data-looper-field="bars">${opt(0, l.bars, 'Free')}${Array.from({ length: 16 }, (_, k) => opt(k + 1, l.bars, `${k + 1} bar${k ? 's' : ''}`)).join('')}</select>`],
      ['Quantize', `<select data-looper-field="quantize">${opt(-1, l.quantize, 'Off')}${DIVISIONS.map((d, i) => opt(i, l.quantize, d)).join('')}</select>`],
      // Record After Routes and Global Looper sit side by side. With Record After Routes on, the loop already records
      // everything the FoxWizard sends, so Global Looper has nothing left to add and is greyed instead of hidden.
      ['Record After Routes', sw('recordAfter', !l.recordInputs)],
      ['Global Looper', sw('loopUnrouted', l.loopUnrouted, !l.recordInputs), !l.recordInputs ? 'Not used while Record After Routes is on, the loop records everything the FoxWizard sends.' : ''],
      ['Auto Overdub', sw('autoOverdub', l.autoOverdub)],
      ['Hold Clears', sw('holdClears', l.holdClears)],
      ['Record CCs', sw('recordCcs', !!l.recordCcs)],
      ['Restart on Clock Start', sw('restartOnStart', !!l.restartOnStart)]
    ].map(([title, control, offReason]) => `<div class="looper-cell${offReason ? ' looper-cell-off' : ''}" ${offReason ? `title="${offReason}"` : ''}><p class="looper-cell-title">${title}</p><div class="looper-cell-control">${control}</div></div>`).join('')}</div>` : ''}`;
}

looperRow.addEventListener('change', (e) => {
  const field = e.target.dataset.looperField;
  if (!field) return;
  const l = state.looper;
  if (field === 'recordAfter') l.recordInputs = !e.target.checked; // on records what the routes send
  else if (e.target.type === 'checkbox') l[field] = e.target.checked;
  else l[field] = parseInt(e.target.value, 10);
  markDirty();
  renderLooperRow();
  renderClockRow();
});

// ============================================================================================
// Global clock row (red). Shows while something uses the clock: for now the Tap Tempo button,
// later the looper and timed route features.
// ============================================================================================
const clockRow = document.getElementById('clockRow');
let clockReadout = { bpm: null, status: 0, perf: null };

function clockRowActive() {
  const featureOn = (bit) => state.routes.some((r) => r.features & bit);
  const isRamp = (a) => a && [ACT.PITCH_RAMP, ACT.MOD_RAMP, ACT.CC_RAMP].includes(a.type) && a.b > 0;
  const rampMapped = (featureOn(FEATURE.LAUNCHPAD) && state.lpActions.some(isRamp)) ||
    (featureOn(FEATURE.NOTE_TO_CC) && state.noteMap.some((e) => isRamp(e.action))) ||
    (featureOn(FEATURE.CC_MAP) && state.ccMap.some((e) => isRamp(e.action)));
  return [1, 2, 3].includes(state.global.buttonAction) || rampMapped ||
    state.global.buttonAction === 10 || state.global.buttonAction === 11 ||
    state.looper.quantize >= 0 || state.looper.bars > 0 || // the looper needs a clock either way it's triggered now
    state.routes.some((r) => r.arpSlot || (r.features & (FEATURE.ECHO | FEATURE.STUTTER)));
}

const CLOCK_SOURCE_AUTO = 15; // Clock: follow the clock of any port while one arrives, lead with its own while none does (1.92)

function clockSourceOptions(g) {
  const ports = PORTS.filter((p) => p.input && (!p.sidecar || sidecarPortShown(p, false) || g.clockSource === p.bit + 1));
  return [`<option value="0" ${g.clockSource === 0 ? 'selected' : ''}>FoxWizard is master</option>`,
    `<option value="${CLOCK_SOURCE_AUTO}" ${g.clockSource === CLOCK_SOURCE_AUTO ? 'selected' : ''}>Auto</option>`]
    .concat(ports.map((p) => `<option value="${p.bit + 1}" ${g.clockSource === p.bit + 1 ? 'selected' : ''}>Follow ${p.label}</option>`))
    .join('');
}

// How hard the main loop worked in the last couple of seconds (1.83): the longest pass round it, then
// anything worth knowing, like input held back because an output queue was filling.
function perfText() {
  const p = clockReadout.perf;
  if (!p) return '';
  const parts = [`Loop ${(p.loopUs / 1000).toFixed(1)} ms`];
  if (p.linkLimit) parts.push(`input limit hit ${p.linkLimit}`);
  if (p.heldBack) parts.push(`input held back ${p.heldBack}`);
  if (p.trsPct >= 25) parts.push(`TRS queue ${p.trsPct}%`);
  if (p.usbPct >= 25) parts.push(`USB queue ${p.usbPct}%`);
  if (p.linkPct >= 25) parts.push(`link ${p.linkPct}%`);
  if (p.repeatOn) parts.push(`repeated Note On ${p.repeatOn}`);
  if (p.strayOff) parts.push(`stray Note Off ${p.strayOff}`);
  if (p.loopRepeatOn) parts.push(`loop overlapping On ${p.loopRepeatOn}`);
  if (p.loopOrphanOff) parts.push(`loop orphan Off ${p.loopOrphanOff}`);
  return ` · ${parts.join(', ')}`;
}

function clockReadoutText() {
  if (!device.connected) return '';
  const { bpm, status } = clockReadout;
  const port = PORTS.find((p) => p.bit + 1 === state.global.clockSource);
  if (state.global.clockSource === CLOCK_SOURCE_AUTO && status === 4) return `${bpm} BPM · Auto, leading with its own clock${perfText()}`;
  if (state.global.clockSource === CLOCK_SOURCE_AUTO && status === 2) return `${bpm} BPM · Auto, following the clock that arrives${perfText()}`;
  if (status === 1) return `${bpm} BPM · Master${perfText()}`;
  if (status === 2) return `${bpm} BPM · Following ${port ? port.label : ''}${perfText()}`;
  if (status === 3) return `No clock from ${port ? port.label : 'that port'}${perfText()}`;
  return 'Save to Device to start the clock';
}

function renderClockRow() {
  clockRow.hidden = false;
  const relevant = clockRowActive();
  clockRow.classList.toggle('clock-row-dim', !relevant);
  const g = state.global;
  if (!relevant) {
    clockRow.innerHTML = `
      <div class="clock-row-header">
        <h2 class="clock-row-heading">GLOBAL CLOCK</h2>
        <span class="clock-readout">Nothing using the clock yet &middot; BPM ${g.bpm}</span>
      </div>`;
    return;
  }
  const auto = g.clockSource === CLOCK_SOURCE_AUTO;
  const master = g.clockSource === 0 || auto; // the FoxWizard's own tempo and clock out matter here
  const opt = (v, cur, label) => `<option value="${v}" ${cur === v ? 'selected' : ''}>${label}</option>`;
  let beats = '';
  for (let n = 1; n <= 16; n++) beats += opt(n, g.beatsPerBar, n);
  const units = [2, 4, 8, 16].map((u) => opt(u, g.beatUnit, u)).join('');
  let tapChannels = opt(16, g.tapChannel, 'Any');
  for (let c = 0; c < 16; c++) tapChannels += opt(c, g.tapChannel, c + 1);
  const tapNumberField = g.tapType === 1
    ? `<select data-clock-field="tapNumber">${noteSelectOptions(g.tapNumber)}</select>`
    : `<input type="number" min="0" max="127" value="${g.tapNumber}" data-clock-field="tapNumber">`;

  clockRow.innerHTML = `
    <div class="clock-row-header">
      <h2 class="clock-row-heading">GLOBAL CLOCK</h2>
      <span class="clock-readout" id="clockReadout">${clockReadoutText()}</span>
    </div>
    <div class="settings-grid">
      <div class="mini-box">${miniBoxMain('BPM', `<input type="number" min="20" max="300" value="${g.bpm}" data-clock-field="bpm" ${master ? '' : 'disabled'}>`)}
        ${auto ? '<p class="note-hint">Used while no clock arrives. A clock that arrives sets it.</p>' : (master ? '' : '<p class="note-hint">Set by the clock it follows.</p>')}</div>
      <div class="mini-box">${miniBoxMain('Clock', `<select data-clock-field="clockSource">${clockSourceOptions(g)}</select>`)}
        ${master
          ? `<label class="switch switch-red clock-send"><input type="checkbox" data-clock-field="clockOut" ${g.clockOut ? 'checked' : ''}><span class="switch-track"><span class="switch-thumb"></span></span><span class="switch-label">Send Clock to every port</span></label>`
          : '<p class="note-hint">Clock from every other port is ignored.</p>'}
        ${auto ? '<p class="note-hint">Follows the first port that sends a clock. Runs its own clock while none does.</p>' : ''}</div>
      <div class="mini-box">${miniBoxMain('Time Signature', `<div class="time-sig">
          <select data-clock-field="beatsPerBar">${beats}</select><span>/</span><select data-clock-field="beatUnit">${units}</select>
        </div>`)}</div>
      <div class="mini-box">${miniBoxMain('Tap Tempo Trigger', `<div class="tap-trigger">
          <select data-clock-field="tapType">${opt(0, g.tapType, 'None')}${opt(1, g.tapType, 'Note')}${opt(2, g.tapType, 'CC')}</select>
          ${g.tapType ? `<select data-clock-field="tapChannel" title="Channel">${tapChannels}</select>${tapNumberField}` : ''}
          <button type="button" class="btn btn-ghost btn-tiny" data-action="learnTap">Learn</button>
        </div>`)}</div>
    </div>`;
}

// A tempo set on the FoxWizard itself (tap tempo, a Free loop) shows here too. Only a change on the
// device is taken, so a BPM typed here and not saved yet isn't overwritten.
let lastDeviceBpm = null;
function updateClockReadout(status) {
  if ((status.clockStatus === 1 || status.clockStatus === 4) && status.bpm) {
    if (lastDeviceBpm !== null && status.bpm !== lastDeviceBpm && status.bpm !== state.global.bpm) {
      state.global.bpm = status.bpm;
      const input = clockRow.querySelector('[data-clock-field="bpm"]');
      if (input && document.activeElement !== input) input.value = status.bpm;
      markDirty();
    }
    lastDeviceBpm = status.bpm;
  }
  clockReadout = { bpm: status.bpm, status: status.clockStatus, perf: status.perf };
  const el = document.getElementById('clockReadout');
  if (el) el.textContent = clockReadoutText();
}

// ============================================================================================
// Scale row (yellow). Shows while a route has Scale on: the key, and the scale on a piano and a
// guitar neck side by side.
// ============================================================================================
const scaleRow = document.getElementById('scaleRow');
const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

// The row always shows. Root and scale only apply, and only color the graphics, while a route has
// Scale on. Notes the FoxWizard sends to the computer show live either way.
function scaleInUse() {
  return state.routes.some((r) => r.features & FEATURE.SCALE);
}

function scalePitchClasses() {
  if (!scaleInUse()) return new Set();
  const steps = SCALES[state.scale.scaleType]?.steps || SCALES[0].steps;
  return new Set(steps.map((s) => (s + state.scale.root) % 12));
}

// Held notes from the FoxWizard's USB output, note number -> count.
const liveNotes = new Map();

// Both graphics share one viewBox height and one top line (GRAPHIC_TOP), so at the same rendered
// height the piano's top lines up with the guitar nut.
const GRAPHIC_TOP = 10;
const GRAPHIC_HEIGHT = 91;
const PRESSED_STROKE = '#0f172a';

// Two octaves from C. Scale notes yellow, the root orange. A held note (shown by note % 24) gets a dark
// inner outline, so the key looks pressed.
// The piano's keys play C3 to B4 (48-71, middle C and the octave below) on every port and channel.
const pianoPointers = new Map(); // pointerId -> note held down
scaleRow.addEventListener('pointerdown', (e) => {
  const key = e.target.closest('.scale-piano [data-note]');
  if (!key || !device.connected) return;
  e.preventDefault();
  const note = parseInt(key.dataset.note, 10);
  pianoPointers.set(e.pointerId, note);
  device.playNote(true, note);
});
function pianoRelease(e) {
  const note = pianoPointers.get(e.pointerId);
  if (note === undefined) return;
  pianoPointers.delete(e.pointerId);
  device.playNote(false, note);
}
document.addEventListener('pointerup', pianoRelease);
document.addEventListener('pointercancel', pianoRelease);

function pianoSvg(pcs, root, held) {
  const whitePcs = [0, 2, 4, 5, 7, 9, 11];
  const blackPcs = { 1: 0, 3: 1, 6: 3, 8: 4, 10: 5 }; // black key pc: the white key index it sits after
  const W = 18, H = 70, BW = 11, BH = 44, Y = GRAPHIC_TOP;
  const fill = (pc, black) => pc === root ? '#f97316' : pcs.has(pc) ? (black ? '#ca8a04' : '#fde047') : (black ? '#1f2937' : '#f8fafc');
  const pressed = (x, w, h) => `<rect x="${x + 1.5}" y="${Y + 1.5}" width="${w - 3}" height="${h - 3}" rx="1.5" fill="none" stroke="${PRESSED_STROKE}" stroke-width="3"/>`;
  let keys = '';
  for (let o = 0; o < 2; o++) {
    whitePcs.forEach((pc, i) => {
      const x = (o * 7 + i) * W;
      keys += `<rect x="${x}" y="${Y}" width="${W}" height="${H}" rx="2" fill="${fill(pc, false)}" stroke="#334155" stroke-width="1" data-note="${48 + o * 12 + pc}"/>`;
      if (held.has(o * 12 + pc)) keys += pressed(x, W, H);
    });
  }
  for (let o = 0; o < 2; o++) {
    for (const [pcText, after] of Object.entries(blackPcs)) {
      const pc = Number(pcText);
      const x = (o * 7 + after + 1) * W - BW / 2;
      keys += `<rect x="${x}" y="${Y}" width="${BW}" height="${BH}" rx="2" fill="${fill(pc, true)}" stroke="#0f172a" stroke-width="1" data-note="${48 + o * 12 + pc}"/>`;
      // Dark outline on a highlighted black key, light on a plain one where dark wouldn't show.
      const stroke = (pc === root || pcs.has(pc)) ? PRESSED_STROKE : '#e2e8f0';
      if (held.has(o * 12 + pc)) keys += `<rect x="${x + 1.5}" y="${Y + 1.5}" width="${BW - 3}" height="${BH - 3}" rx="1.5" fill="none" stroke="${stroke}" stroke-width="2.5"/>`;
    }
  }
  return `<svg class="scale-piano" viewBox="-1 0 ${14 * W + 2} ${GRAPHIC_HEIGHT}" role="img" aria-label="Scale on a piano">${keys}</svg>`;
}

// Standard tuning, high E on top like a tab. Frets 0-12, spaced like a real neck: fret n sits at
// 1 - 2^(-n/12) of the scale length, so the 12th is halfway. Scale notes yellow, the root orange. A
// held note marks every octave of it on the neck: an inner dark outline on its dot, or an outlined
// empty dot where it isn't in the scale. The outline is drawn inside the dot, which keeps its size.
const GUITAR_TUNING = [64, 59, 55, 50, 45, 40];
const GUITAR_FRETS = 12;
const DOT_R = 5;
const HELD_STROKE = 3;

function guitarSvg(pcs, root, held, viewWidth) {
  const SH = 13, X0 = 22, Y0 = GRAPHIC_TOP, RIGHT_PAD = 8;
  const neck = viewWidth - X0 - RIGHT_PAD;
  const fretX = (n) => X0 + neck * (1 - Math.pow(2, -n / 12)) / (1 - Math.pow(2, -GUITAR_FRETS / 12));
  const dotX = (f) => (f === 0 ? X0 - 11 : (fretX(f - 1) + fretX(f)) / 2);
  const heldPcs = new Set([...held].map((n) => n % 12));
  const markerY = Y0 + 5 * SH + 9;
  let g = '';
  for (let f = 0; f <= GUITAR_FRETS; f++) g += `<line x1="${fretX(f)}" y1="${Y0}" x2="${fretX(f)}" y2="${Y0 + 5 * SH}" stroke="${f === 0 ? '#e2e8f0' : '#475569'}" stroke-width="${f === 0 ? 3 : 1}"/>`;
  GUITAR_TUNING.forEach((_, s) => { g += `<line x1="${X0}" y1="${Y0 + s * SH}" x2="${fretX(GUITAR_FRETS)}" y2="${Y0 + s * SH}" stroke="#94a3b8" stroke-width="${0.6 + s * 0.25}"/>`; });
  for (const f of [3, 5, 7, 9]) g += `<circle cx="${dotX(f)}" cy="${markerY}" r="2.5" fill="#475569"/>`;
  g += `<circle cx="${dotX(12) - 5}" cy="${markerY}" r="2.5" fill="#475569"/><circle cx="${dotX(12) + 5}" cy="${markerY}" r="2.5" fill="#475569"/>`;
  GUITAR_TUNING.forEach((open, s) => {
    for (let f = 0; f <= GUITAR_FRETS; f++) {
      const note = open + f, pc = note % 12;
      const inScale = pcs.has(pc), isHeld = heldPcs.has(pc);
      if (!inScale && !isHeld) continue;
      const fill = inScale ? (pc === root ? '#f97316' : '#fde047') : 'none';
      const cx = dotX(f), cy = Y0 + s * SH;
      if (!isHeld) {
        g += `<circle cx="${cx}" cy="${cy}" r="${DOT_R}" fill="${fill}" stroke="${PRESSED_STROKE}" stroke-width="1"/>`;
      } else {
        // Shrunk by half the stroke, so the thick outline sits inside the dot's usual edge.
        g += `<circle cx="${cx}" cy="${cy}" r="${DOT_R - HELD_STROKE / 2}" fill="${fill}" stroke="${inScale ? PRESSED_STROKE : '#e2e8f0'}" stroke-width="${HELD_STROKE}"/>`;
      }
    }
  });
  return `<svg class="scale-guitar" viewBox="0 0 ${viewWidth} ${GRAPHIC_HEIGHT}" role="img" aria-label="Scale on a guitar neck">${g}</svg>`;
}

// On desktop the neck fills the space right of the piano, at the shared 130px height. Its viewBox
// width is worked out from that space, so dots stay round. On a phone it has a fixed shape and
// scales to the full width.
const GRAPHIC_PX_HEIGHT = 130;
const PIANO_VIEW_WIDTH = 14 * 18 + 2;
const GUITAR_MOBILE_VIEW_WIDTH = 420;
const GRAPHICS_GAP_PX = 18;

function guitarViewWidth() {
  const el = document.getElementById('scaleGraphics');
  if (!el || window.matchMedia('(max-width: 640px)').matches) return GUITAR_MOBILE_VIEW_WIDTH;
  const pianoPx = PIANO_VIEW_WIDTH * GRAPHIC_PX_HEIGHT / GRAPHIC_HEIGHT;
  const spacePx = el.clientWidth - pianoPx - GRAPHICS_GAP_PX;
  if (spacePx < 200) return GUITAR_MOBILE_VIEW_WIDTH; // wraps under the piano at this width anyway
  return Math.floor(spacePx * GRAPHIC_HEIGHT / GRAPHIC_PX_HEIGHT);
}

let guitarResizeTimer = null;
window.addEventListener('resize', () => {
  clearTimeout(guitarResizeTimer);
  guitarResizeTimer = setTimeout(queueScaleGraphics, 100);
});

function scaleGraphicsHtml() {
  const pcs = scalePitchClasses();
  const root = scaleInUse() ? state.scale.root : -1;
  const held = new Set(liveNotes.keys());
  const byTwoOctaves = new Set([...liveNotes.keys()].map((n) => n % 24));
  return pianoSvg(pcs, root, byTwoOctaves) + guitarSvg(pcs, root, held, guitarViewWidth());
}

function renderScaleRow() {
  scaleRow.hidden = false;
  const sc = state.scale;
  const active = scaleInUse();
  scaleRow.classList.toggle('scale-row-idle', !active);
  scaleRow.innerHTML = `
    <div class="scale-row-header">
      <h2 class="scale-row-heading">SCALE</h2>
      <select data-scale-field="root" aria-label="Root" ${active ? '' : 'disabled'}>${NOTE_NAMES.map((n, i) => `<option value="${i}" ${sc.root === i ? 'selected' : ''}>${n}</option>`).join('')}</select>
      <select data-scale-field="scaleType" aria-label="Scale" ${active ? '' : 'disabled'}>${SCALES.map((s, i) => `<option value="${i}" ${sc.scaleType === i ? 'selected' : ''}>${s.name}</option>`).join('')}</select>
    </div>
    <div class="scale-graphics" id="scaleGraphics"></div>`;
  document.getElementById('scaleGraphics').innerHTML = scaleGraphicsHtml(); // measured once it's in the page
}

// Live notes redraw only the graphics, at most once per frame.
let scaleGraphicsQueued = false;
function queueScaleGraphics() {
  if (scaleGraphicsQueued) return;
  scaleGraphicsQueued = true;
  requestAnimationFrame(() => {
    scaleGraphicsQueued = false;
    const el = document.getElementById('scaleGraphics');
    if (el) el.innerHTML = scaleGraphicsHtml();
  });
}

device.addEventListener('note', (e) => {
  const { note, on } = e.detail;
  const count = liveNotes.get(note) || 0;
  if (on) liveNotes.set(note, count + 1);
  else if (count > 1) liveNotes.set(note, count - 1);
  else liveNotes.delete(note);
  queueScaleGraphics();
});

device.addEventListener('status', (e) => {
  if (e.detail.state !== 'connected' && liveNotes.size) {
    liveNotes.clear();
    queueScaleGraphics();
  }
});

scaleRow.addEventListener('change', (e) => {
  const field = e.target.dataset.scaleField;
  if (!field) return;
  state.scale[field] = parseInt(e.target.value, 10);
  trimChordMasks(); // a smaller scale drops its leftover chord positions
  markDirty();
  renderScaleRow();
  state.routes.forEach((r, i) => { if (r.features & FEATURE.AUTO_CHORD) refreshRouteCard(i); });
});

const CLOCK_STRUCTURAL = new Set(['clockSource', 'tapType']);

clockRow.addEventListener('change', (e) => {
  const field = e.target.dataset.clockField;
  if (!field) return;
  const g = state.global;
  if (field === 'clockOut') {
    g.clockOut = e.target.checked ? 1 : 0;
  } else {
    let v = parseInt(e.target.value, 10);
    if (Number.isNaN(v)) return;
    if (field === 'bpm') v = Math.min(300, Math.max(20, v));
    if (field === 'tapNumber') v = Math.min(127, Math.max(0, v));
    g[field] = v;
    if (String(v) !== e.target.value) e.target.value = v;
  }
  if (CLOCK_STRUCTURAL.has(field)) renderClockRow();
  markDirty();
});

clockRow.addEventListener('click', async (e) => {
  const btn = e.target.closest('[data-action="learnTap"]');
  if (!btn) return;
  if (!device.connected) { toast('Not connected to a device', 'error'); return; }
  if (device.protocolVersion < 3) { toast('Learn needs FoxWizard firmware 1.4 or later', 'error'); return; }
  btn.disabled = true;
  btn.textContent = 'Play a note or move a control…';
  try {
    const r = await device.learn();
    Object.assign(state.global, { tapType: r.type, tapChannel: r.channel, tapNumber: r.number });
    markDirty();
    toast(`Learned ${r.type === 1 ? `note ${noteName(r.number)}` : `CC ${r.number}`} on channel ${r.channel + 1}`, 'success');
  } catch (err) {
    toast('Nothing heard in 10 seconds', 'error');
  }
  renderClockRow();
});

// Mutates the toggle in place rather than re-rendering the whole card: a fresh element already
// sitting at its final position never actually transitions (there's no "before" state to animate
// from), which is what was silently skipping the slide. Keeping the same knob element alive lets
// the left/right CSS transition play, then a jiggle settles it once the slide finishes.
function applyModeVisual(animate) {
  const isUsbAdaptor = state.global.mode === 0;
  const toggleEl = document.getElementById('modeToggle');
  const knob = toggleEl.querySelector('.mode-toggle-knob');
  toggleEl.dataset.mode = isUsbAdaptor ? 'usbAdaptor' : 'router';
  knob.innerHTML = isUsbAdaptor ? SHARK_SVG : OCTOPUS_SVG;
  document.querySelector('.mode-toggle-side.left')?.classList.toggle('active', isUsbAdaptor);
  document.querySelector('.mode-toggle-side.right')?.classList.toggle('active', !isUsbAdaptor);

  if (animate) {
    // Jiggle (transform: scale/rotate) and the slide (left, a different property) run concurrently
    // from the moment of the click — they don't fight each other since they're separate properties,
    // so there's no need to wait for the slide to land before the wobble starts.
    knob.classList.remove('jiggle');
    void knob.offsetWidth; // restart the animation cleanly if clicked again mid-jiggle
    knob.classList.add('jiggle');
    knob.addEventListener('animationend', () => knob.classList.remove('jiggle'), { once: true });
  }
}

function wireGlobalHeader() {
  document.getElementById('resetAllBtn').addEventListener('click', resetEverything);
  document.getElementById('masterStaple').addEventListener('click', toggleAllRoutesCollapsed);
}

// Local-only, same as every other edit — nothing reaches the device until Save to Device. Still
// asks first since it's a total wipe of whatever's currently configured, not a small edit.
function resetEverything() {
  if (!confirm("Clear all settings? Remember to click 'Save to Device' next.")) return;
  state.global = defaultGlobal();
  state.routes = [];
  renderGlobalCard();
  renderRoutes();
  markDirty();
  toast('Routes cleared and settings reset to default', 'success');
}

// Smart toggle: if anything is currently expanded, collapse everything; only once every route is
// already collapsed does clicking again expand everything back out.
function toggleAllRoutesCollapsed() {
  const allCollapsed = state.routes.length > 0 && state.routes.every((r) => r._collapsed);
  const target = !allCollapsed;
  state.routes.forEach((r) => { r._collapsed = target; });
  renderRoutes();
  refreshMasterStaple(true);
}

// The master staple lives in the global card, but its correct open/closed state depends on
// state.routes, which changes from a bunch of other places (individual route staples, add,
// delete, load, preset upload) — so it's resynced explicitly rather than only on its own click.
function refreshMasterStaple(animate) {
  const el = document.getElementById('masterStaple');
  if (!el) return;
  const allCollapsed = state.routes.length > 0 && state.routes.every((r) => r._collapsed);
  el.title = allCollapsed ? 'All routes collapsed — click to open all' : 'Click to collapse all routes';
  el.innerHTML = stapleSvg(allCollapsed, animate);
}

function wireModeToggle() {
  const el = document.getElementById('modeToggle');
  const toggle = () => {
    state.global.mode = state.global.mode === 0 ? 1 : 0;
    applyModeVisual(true);
    syncGlobalLive();
  };
  el.addEventListener('click', toggle);
  el.addEventListener('keydown', (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); toggle(); } });
}

function wireKnob() {
  const knob = document.getElementById('brightnessKnob');
  const indicator = knob.querySelector('.knob-indicator');
  const readout = document.getElementById('knobReadout');
  let lastStep = state.global.ledBrightnessStep;

  function angleToStep(angle) {
    const clamped = Math.max(-150, Math.min(150, angle));
    return Math.round(((clamped + 150) / 300) * 10);
  }

  function pointerToAngle(clientX, clientY) {
    const rect = knob.getBoundingClientRect();
    const cx = rect.left + rect.width / 2;
    const cy = rect.top + rect.height / 2;
    const deg = Math.atan2(clientX - cx, -(clientY - cy)) * (180 / Math.PI);
    return Math.max(-150, Math.min(150, deg));
  }

  function applyStep(step, live) {
    if (step === lastStep) return;
    lastStep = step;
    state.global.ledBrightnessStep = step;
    const angle = -150 + (step / 10) * 300;
    indicator.style.transform = `rotate(${angle}deg)`;
    readout.textContent = `${step * 10}%`;
    if (live && device.connected) {
      // Fire-and-forget: the device's own LEDs are the confirmation here. Dragging the knob fires
      // this rapidly and the device briefly blocks while it flashes each preview, so an ACK can
      // legitimately arrive late or get superseded by the next step — that's not a real failure,
      // just this control being live/best-effort by nature. Only the final synced value (on
      // pointer-up, via syncGlobalLive) needs to be reliably confirmed.
      device.previewBrightness(step).catch(() => {});
    }
  }

  function onMove(e) {
    const point = e.touches ? e.touches[0] : e;
    const angle = pointerToAngle(point.clientX, point.clientY);
    applyStep(angleToStep(angle), true);
  }
  function onUp() {
    document.removeEventListener('pointermove', onMove);
    document.removeEventListener('pointerup', onUp);
    knob.classList.remove('active');
    syncGlobalLive();
  }
  knob.addEventListener('pointerdown', (e) => {
    knob.setPointerCapture?.(e.pointerId);
    knob.classList.add('active');
    onMove(e);
    document.addEventListener('pointermove', onMove);
    document.addEventListener('pointerup', onUp);
  });
}

globalCard.addEventListener('change', (e) => {
  const field = e.target.dataset.globalField;
  if (!field) return;
  state.global[field] = parseInt(e.target.value, 10);
  // Pitch/Mod read buttonParamA as a ramp length: a CC or note number left from another mode isn't one.
  if (field === 'buttonAction' && [1, 2, 3].includes(state.global.buttonAction) && state.global.buttonParamA > DIVISIONS.length) state.global.buttonParamA = 6;
  if (field === 'buttonAction' || field === 'buttonToggleMomentary') renderGlobalCard();
  syncGlobalLive();
});
globalCard.addEventListener('input', (e) => {
  const field = e.target.dataset.globalField;
  if (!field || e.target.type !== 'range') return;
  state.global[field] = parseInt(e.target.value, 10);
  syncGlobalLive();
});

// Local-only: edits no longer push to the device automatically. They used to (an earlier design
// for "live audition"), but that meant routine edits — including add/delete/reorder — could race
// with each other and with an explicit Load/Save click (DeviceLink only allows one in-flight
// SysEx exchange at a time), causing confusing "could not sync" errors and even making a
// subsequent Load from Device fail outright if it landed while an edit's auto-push was still in
// flight. Now nothing touches the device until you explicitly click "Save to Device", which is
// unambiguous and can't race with anything else you're doing in the UI.
function syncGlobalLive() {
  markDirty();
}

// ============================================================================================
// Route cards
// ============================================================================================
const routesContainer = document.getElementById('routesContainer');
const emptyHint = document.getElementById('emptyRoutesHint');

// A TRS port is a hardware fact about the attached sidecar, not a transient "unplugged" state: it
// shows only while a sidecar that says it has a TRS board is connected (1.76). Not for one without,
// and not while none is there at all, even for a route already using it (every route defaults to
// every port on, so that fallback would otherwise defeat this entirely, and a hub being unplugged
// would bring all of them back).
function trsUnavailable(p) {
  return p.trs && !(sidecarPresent && sidecarHasTrs);
}

// How many USB Host ports there is room for: one with no hub (a device plugged straight in), else the
// hubs' jacks, up to the 4 the FoxWizard has.
function hostCapacity() { return hostHub.jacks ? Math.min(hostSlots, hostHub.jacks) : (hostHub.hubs ? hostSlots : 1); }

// Works out how many USB Host chips show: the highest port with a device plus one to plug into next, at
// least one, at most the room the hubs have. Only grows while connected. True if it changed.
function updateHostPortsShown() {
  const before = hostPortsShown;
  if (!sidecarPresent || !hostHub.known) {
    hostPortsSeen = 0;
    hostPortsShown = 4;
  } else {
    let highest = 0;
    for (let i = 0; i < hostSlots; i++) if (hostPortMask & (1 << i)) highest = i + 1;
    hostPortsSeen = Math.max(hostPortsSeen, highest + 1, 1);
    // A device on a port means there's room for it, whatever the hub report says.
    hostPortsShown = Math.min(Math.max(hostCapacity(), highest), hostPortsSeen);
  }
  return hostPortsShown !== before;
}

// Whether a sidecar port's chip shows. USB Host chips follow the room the hubs have once the host
// brain says. The rest show while a sidecar is detected, or when something already uses them.
function sidecarPortShown(p, used) {
  if (trsUnavailable(p)) return false;
  if (p.slot && sidecarPresent && p.slot > hostSlots) return false; // a host brain with fewer slots than this port
  if (p.slot && sidecarPresent && hostHub.known) return p.slot <= hostPortsShown;
  return sidecarPresent || used;
}

// Port switches. Each switch and its label sit inside one chip, so it's always clear which label
// goes with which switch. Sidecar ports (USB Host 1-4, TRS 2-6) only show while a sidecar is
// detected, or when the route already uses them (TRS 2-6 excepted, see trsUnavailable above).
function visiblePorts(route, field) {
  return PORTS.filter((p) => (field === 'outPorts' || p.input) && (!p.sidecar || sidecarPortShown(p, !!(route[field] & (1 << p.bit)))));
}

// Sidecar port masks this board actually has: Host 1-4 always, TRS 2-6 only when a TRS board is
// added on top of the bare sidecar module.
function sidecarInputPorts() { return sidecarHasTrs ? SIDECAR_INPUT_PORTS : (SIDECAR_INPUT_PORTS & ~SIDECAR_TRS_PORTS); }
function sidecarOutputPorts() { return sidecarHasTrs ? SIDECAR_OUTPUT_PORTS : (SIDECAR_OUTPUT_PORTS & ~SIDECAR_TRS_PORTS); }

function visiblePortMask(route, field) {
  return visiblePorts(route, field).reduce((mask, p) => mask | (1 << p.bit), 0);
}

function allPortsOn(route, field) {
  const mask = visiblePortMask(route, field);
  return (route[field] & mask) === mask;
}

// The Input/Output title leads the row of chips, followed by the purple All Ins/All Outs chip.
function portSwitchesHtml(route, index, field, title) {
  const allField = field === 'inPorts' ? 'allInPorts' : 'allOutPorts';
  const allLabel = field === 'inPorts' ? 'All Ins' : 'All Outs';
  const allChip = `<label class="switch switch-purple port-chip" title="${allLabel}">
      <input type="checkbox" data-route-index="${index}" data-field="${allField}" ${allPortsOn(route, field) ? 'checked' : ''}>
      <span class="switch-track"><span class="switch-thumb"></span></span>
      <span class="switch-label">${allLabel}</span>
    </label>`;
  const chips = visiblePorts(route, field).map((p) => {
    const live = p.slot && sidecarPresent && (hostPortMask & (1 << (p.slot - 1)));
    return `<label class="switch switch-port port-chip${live ? ' port-chip-live' : ''}" data-port-bit="${p.bit}" title="${p.label}${live ? ', device connected' : ''}">
      <input type="checkbox" data-route-index="${index}" data-field="${field}" data-bit="${p.bit}" ${route[field] & (1 << p.bit) ? 'checked' : ''}>
      <span class="switch-track"><span class="switch-thumb"></span></span>
      <span class="switch-label">${p.label}</span>
    </label>`;
  }).join('');
  return `<div class="port-switches"><h3>${title}</h3>${allChip}${chips}</div>`;
}

// Marks which USB Host chips have a device plugged in, without re-rendering the route list.
function updatePortChipsLive() {
  for (const p of PORTS.filter((x) => x.slot)) {
    const i = p.slot - 1;
    const live = sidecarPresent && !!(hostPortMask & (1 << i));
    routesContainer.querySelectorAll(`.port-chip[data-port-bit="${p.bit}"]`).forEach((el) => {
      el.classList.toggle('port-chip-live', live);
      el.title = `USB Host ${i + 1}${live ? ', device connected' : ''}`;
    });
  }
}

function selectOptions(options, selected) {
  return options.map((o) => `<option value="${o.v}" ${o.v === selected ? 'selected' : ''}>${o.l}</option>`).join('');
}

function channelGrid(index, field, mask) {
  let html = '<div class="channel-grid">';
  for (let c = 0; c < 16; c++) {
    const checked = mask & (1 << c) ? 'checked' : '';
    html += `<div class="channel-chip">
      <input type="checkbox" id="ch-${field}-${index}-${c}" data-route-index="${index}" data-field="${field}" data-bit="${c}" ${checked}>
      <label for="ch-${field}-${index}-${c}">${c + 1}</label>
    </div>`;
  }
  html += '</div>';
  return html;
}

function switchHtml(index, field, checked, label, extraClass) {
  const id = `sw-${field}-${index}`;
  return `<label class="switch${extraClass ? ` ${extraClass}` : ''}">
    <input type="checkbox" id="${id}" data-route-index="${index}" data-field="${field}" ${checked ? 'checked' : ''}>
    <span class="switch-track"><span class="switch-thumb"></span></span>
    <span class="switch-label">${label}</span>
  </label>`;
}

function portsShort(mask) {
  const names = PORTS.filter((p) => mask & (1 << p.bit)).map((p) => p.short);
  return names.length ? names.join('+') : 'None';
}
const TYPE_SHORT_LABELS = [
  [DATA_TYPE_BITS.note, 'Note'],
  [DATA_TYPE_BITS.cc, 'CC'],
  [DATA_TYPE_BITS.programChange, 'PC'],
  [DATA_TYPE_BITS.pitchBend, 'Bend'],
  [DATA_TYPE_BITS.pressure, 'Ch AT'],
  [DATA_TYPE_BITS.aftertouch, 'Poly AT'],
  [DATA_TYPE_BITS.sysex, 'Sysex']
];

// System Common/Realtime/Clock aren't typeFlags bits (typeFlags is full — they live in flags2
// instead, as plain boolean route fields), so they're not part of TYPE_SHORT_LABELS' bit-filter
// scheme and need their own check here and in routeSummary below.
function allInputTypesOn(route) {
  return (route.typeFlags & 0x7F) === 0x7F && !!route.commonEnabled && !!route.realtimeEnabled && !!route.clockEnabled;
}

function allInputChannelsOn(route) {
  return route.inputChannels === 0xFFFF;
}

// Plain-language one-liner shown in place of the full settings when a route is collapsed — the
// collapsed card is only as tall as the header, so this is the only thing conveying what it does.
function routeSummary(route) {
  const inCh = route.inputChannels === 0xFFFF ? 'All Ch' : `Ch ${channelsToRangeString(route.inputChannels) || 'None'}`;
  const outCh = route.outputChannels === 0xFFFF ? 'All Ch' : `Ch ${channelsToRangeString(route.outputChannels) || 'None'}`;
  const types = TYPE_SHORT_LABELS.filter(([bit]) => route.typeFlags & bit).map(([bit, label]) => {
    if (bit === DATA_TYPE_BITS.aftertouch && (route.typeFlags & DATA_TYPE_BITS.aftertouchMap)) return `Poly AT → CC ${route.atMapCC}`;
    if (bit === DATA_TYPE_BITS.pressure && route.cpMapEnabled) return `Ch AT → CC ${route.cpMapCC}`;
    return label;
  });
  if (route.commonEnabled) types.push('Common');
  if (route.realtimeEnabled) types.push('Transport');
  if (route.clockEnabled) types.push('Clock');
  const typesText = types.length ? types.join(', ') : 'Nothing Selected';
  const transposeText = (route.typeFlags & DATA_TYPE_BITS.note) && route.transpose !== 0
    ? `, Transpose ${route.transpose > 0 ? '+' : ''}${route.transpose}` : '';
  const velocityText = (route.typeFlags & DATA_TYPE_BITS.note) && route.velocityScale !== 100
    ? `, Velocity ${route.velocityScale}%` : '';
  const monoRobin = [];
  if (route.monoRetrig) monoRobin.push('Mono');
  if (route.roundRobin) monoRobin.push(route.roundRobinRandom ? 'Rand Robin' : 'Round Robin');
  const monoRobinText = monoRobin.length ? `, ${monoRobin.join(', ')}` : '';
  return `${portsShort(route.inPorts)} ${inCh} → ${portsShort(route.outPorts)} ${outCh} · ${typesText}${transposeText}${velocityText}${monoRobinText}`;
}

function renderRouteCard(route, index, animateStaple) {
  const noteOn = !!(route.typeFlags & DATA_TYPE_BITS.note);
  const ccOn = !!(route.typeFlags & DATA_TYPE_BITS.cc);
  const ccSpan = route.ccEnd - route.ccStart;
  const ccMapEnabled = !!route._ccMapEnabled;
  const ccMapDisplayValue = ccMapEnabled ? route.ccMapStart : route.ccStart;
  const ccMapInvalid = ccMapEnabled && (route.ccMapStart + ccSpan > 127);
  const aftertouchOn = !!(route.typeFlags & DATA_TYPE_BITS.aftertouch);
  const aftertouchMapOn = !!(route.typeFlags & DATA_TYPE_BITS.aftertouchMap);
  const pressureOn = !!(route.typeFlags & DATA_TYPE_BITS.pressure);
  const pressureMapOn = !!route.cpMapEnabled;
  const collapsed = !!route._collapsed;

  const headerCenter = collapsed
    ? `<div class="route-collapsed-info" data-route-index="${index}" data-action="expand" title="Click to expand">
        <span class="route-collapsed-name">${(route.name || `Route ${index + 1}`).replace(/</g, '&lt;')}</span>
        <span class="route-collapsed-summary">${routeSummary(route)}</span>
      </div>`
    : `<input class="route-title-input" type="text" maxlength="${ROUTE_NAME_MAX_LEN}" placeholder="Route ${index + 1}" value="${route.name.replace(/"/g, '&quot;')}" data-route-index="${index}" data-field="name">`;

  return `
  <div class="card card-full route-card ${collapsed ? 'collapsed' : ''}" data-route-index="${index}">
    <div class="route-card-header">
      <span class="route-drag-handle" draggable="true" title="Drag to reorder">${GRIP_SVG}</span>
      <span class="route-order-badge">${index + 1}</span>
      ${headerCenter}
      <span class="staple-toggle" data-route-index="${index}" data-action="toggleCollapsed" title="${collapsed ? 'Collapsed — click to open and show settings' : 'Open — click to close and hide settings'}">${stapleSvg(collapsed, animateStaple)}</span>
      <button class="btn btn-icon btn-danger" data-route-index="${index}" data-action="delete" title="Delete route">${TRASH_SVG}</button>
    </div>

    ${collapsed ? '' : `<div class="route-body">
      <div class="route-io-columns">
        <div class="route-input-col">
          <div class="col-header-row">${portSwitchesHtml(route, index, 'inPorts', 'Input')}</div>
          <div class="field-row">
            <label>Input Channels</label>
            ${channelGrid(index, 'inputChannels', route.inputChannels)}
          </div>
          <div class="switches-master-row">
            ${switchHtml(index, 'allInputChannels', allInputChannelsOn(route), 'All Channels', 'switch-purple')}
            ${switchHtml(index, 'allInputTypes', allInputTypesOn(route), 'All Data', 'switch-purple')}
          </div>
          <div class="switches-cols">
            <div class="switches-col">
              <div class="field-row">
                ${switchHtml(index, 'note', noteOn, 'Note Range')}
                ${noteOn ? `<div class="range-row">
                  <input type="number" min="0" max="127" value="${route.noteStart}" data-route-index="${index}" data-field="noteStart">
                  <span>to</span>
                  <input type="number" min="0" max="127" value="${route.noteEnd}" data-route-index="${index}" data-field="noteEnd">
                  <button type="button" class="btn btn-ghost btn-tiny" data-route-index="${index}" data-action="setNoteRangeAll">All</button>
                </div>` : ''}
              </div>
              <div class="field-row">
                ${switchHtml(index, 'cc', ccOn, 'CC Range')}
                ${ccOn ? `<div class="range-row">
                  <input type="number" min="0" max="127" value="${route.ccStart}" data-route-index="${index}" data-field="ccStart">
                  <span>to</span>
                  <input type="number" min="0" max="127" value="${route.ccEnd}" data-route-index="${index}" data-field="ccEnd">
                  <button type="button" class="btn btn-ghost btn-tiny" data-route-index="${index}" data-action="setCcRangeAll">All</button>
                </div>` : ''}
              </div>
              <div class="field-row">
                ${switchHtml(index, 'programChange', !!(route.typeFlags & DATA_TYPE_BITS.programChange), 'Program Change')}
              </div>
              <div class="field-row">
                ${switchHtml(index, 'pitchBend', !!(route.typeFlags & DATA_TYPE_BITS.pitchBend), 'Pitch Bend')}
              </div>
              <div class="field-row">
                ${switchHtml(index, 'clock', !!route.clockEnabled, 'Clock')}
              </div>
            </div>
            <div class="switches-col">
              <div class="field-row">
                ${switchHtml(index, 'pressure', pressureOn, 'Ch Pressure AT')}
                ${pressureOn ? `<div class="cc-map-row cc-map-row-indent">
                  ${switchHtml(index, 'pressureMap', pressureMapOn, '↳ AT to CC')}
                  <input type="number" min="0" max="127" value="${route.cpMapCC}" data-route-index="${index}" data-field="cpMapCC" ${pressureMapOn ? '' : 'disabled'}>
                </div>` : ''}
              </div>
              <div class="field-row">
                ${switchHtml(index, 'aftertouch', aftertouchOn, 'Poly AT')}
                ${aftertouchOn ? `<div class="cc-map-row cc-map-row-indent">
                  ${switchHtml(index, 'aftertouchMap', aftertouchMapOn, '↳ AT to CC')}
                  <input type="number" min="0" max="127" value="${route.atMapCC}" data-route-index="${index}" data-field="atMapCC" ${aftertouchMapOn ? '' : 'disabled'}>
                </div>` : ''}
              </div>
              <div class="field-row">
                ${switchHtml(index, 'sysex', !!(route.typeFlags & DATA_TYPE_BITS.sysex), 'Sysex')}
              </div>
              <div class="field-row">
                ${switchHtml(index, 'common', !!route.commonEnabled, 'Common')}
              </div>
              <div class="field-row">
                ${switchHtml(index, 'realtime', !!route.realtimeEnabled, 'Realtime (Transport)')}
              </div>
            </div>
          </div>
          <p class="note-hint">Unselected data types pass through normally based on the Main Mode above.</p>
        </div>

        <div class="route-output-col">
          <div class="col-header-row">${portSwitchesHtml(route, index, 'outPorts', 'Output')}</div>
          <div class="field-row">
            <label>Output Channels <span class="channel-warning">${countBits(route.outputChannels) > 2 ? 'Caution: dense data may lag if cloned to too many channels.' : ''}</span></label>
            ${channelGrid(index, 'outputChannels', route.outputChannels)}
          </div>
          <div class="field-row-cols">
            <div class="field-row-cols">
              <div class="field-subrow">
                <label>Transpose</label>
                <input type="number" min="-64" max="63" value="${route.transpose}" data-route-index="${index}" data-field="transpose">
              </div>
              <div class="field-subrow">
                <label>Velocity %</label>
                <input type="number" min="10" max="200" step="10" value="${route.velocityScale}" data-route-index="${index}" data-field="velocityScale">
              </div>
            </div>
            <div class="field-row">
              <div class="cc-map-row">
                ${switchHtml(index, 'ccMapEnabled', ccMapEnabled, `CC Map Start${ccMapInvalid ? ' — too high' : ''}`)}
                <input type="number" min="0" max="127" value="${ccMapDisplayValue}" data-route-index="${index}" data-field="ccMapStart" ${ccMapEnabled ? '' : 'disabled'} ${ccMapInvalid ? 'style="border-color:var(--red)"' : ''}>
              </div>
              <p class="note-hint cc-map-hint">CC range now begins with this CC.</p>
            </div>
            ${toggleRowsHtml(route, index)}
          </div>
        </div>
      </div>
    </div>`}
  </div>`;
}

// The "many channels" warning is computed at render time only — channel checkboxes don't trigger
// a re-render (the whole point is avoiding a full-list rebuild on every click), so it needs its
// own tiny direct DOM update or it just goes stale after the first render.
function updateChannelWarning(index) {
  const card = routesContainer.querySelector(`.route-card[data-route-index="${index}"]`);
  const warningEl = card?.querySelector('.channel-warning');
  if (!warningEl) return;
  const on = countBits(state.routes[index].outputChannels) > 2;
  warningEl.textContent = on ? 'Caution: dense data may lag if cloned to too many channels.' : '';
}

// Same idea as updateChannelWarning above — "All Channels"/"All Data" only reflect the current
// combined state at render time, and most of the individual switches/checkboxes they summarize
// don't trigger a full re-render (only the ones with their own nested sub-content do, for that
// unrelated reason), so without this targeted sync the master switches would silently go stale
// the moment you turn off anything that doesn't happen to also cause a re-render. Called after
// every route field change, refresh or not, so it always reflects the state accurately.
function syncMasterSwitches(index) {
  const card = routesContainer.querySelector(`.route-card[data-route-index="${index}"]`);
  if (!card) return;
  const route = state.routes[index];
  const allTypesEl = card.querySelector('[data-field="allInputTypes"]');
  if (allTypesEl) allTypesEl.checked = allInputTypesOn(route);
  const allChEl = card.querySelector('[data-field="allInputChannels"]');
  if (allChEl) allChEl.checked = allInputChannelsOn(route);
  const allInEl = card.querySelector('[data-field="allInPorts"]');
  if (allInEl) allInEl.checked = allPortsOn(route, 'inPorts');
  const allOutEl = card.querySelector('[data-field="allOutPorts"]');
  if (allOutEl) allOutEl.checked = allPortsOn(route, 'outPorts');
}

function countBits(mask) {
  let n = 0;
  for (let i = 0; i < 16; i++) if (mask & (1 << i)) n++;
  return n;
}

// ============================================================================================
// Route features (firmware 1.6 and later). Each one is a switch on the route's output side, with its
// controls hidden while it's off. Only one route can have Note to CC on.
// ============================================================================================
function featuresSupported() {
  return !device.connected || device.protocolVersion >= 4;
}

// Note to Feature and CC to Feature: each note or CC the route hears that has an entry runs its
// action instead of passing through. The Launchpad's round buttons run an action each too.
function mapListFor(owner) { return owner === 'cc' ? state.ccMap : state.noteMap; }
function actionFor(owner, i) { return owner === 'lp' ? state.lpActions[i] : mapListFor(owner)[i]?.action; }

function mapSectionHtml(route, index, owner) {
  const isCc = owner === 'cc';
  const bit = isCc ? FEATURE.CC_MAP : FEATURE.NOTE_TO_CC;
  const on = !!(route.features & bit);
  const label = isCc ? 'CC to Feature' : 'Note to Feature';
  const field = isCc ? 'ccMapOn' : 'noteToCc';
  const list = mapListFor(owner);
  const rows = list.map((e, i) => {
    const ids = `data-act-owner="${owner}" data-act-index="${i}" data-route-index="${index}"`;
    const source = isCc
      ? `<input type="number" min="0" max="127" value="${e.source}" data-map-source ${ids} title="CC number">`
      : `<select data-map-source ${ids} title="Note">${noteSelectOptions(e.source)}</select>`;
    return `<div class="notecc-row">
        ${source}
        <button type="button" class="btn btn-ghost btn-tiny" data-action="learnMapSource" ${ids}>Learn</button>
        <span class="notecc-arrow">→</span>
        ${actionEditorHtml(e.action, ids, noteSelectOptions, false, DIVISIONS)}
        <button type="button" class="btn btn-icon btn-danger btn-tiny" data-action="removeMapEntry" ${ids} title="Remove">${TRASH_SVG}</button>
      </div>`;
  }).join('');
  return `<div class="${featureRowClass(on)}">
      ${switchHtml(index, field, on, label)}
      ${on ? `<div class="notecc-table">${rows}
        ${list.length < MAP_ENTRIES ? `<button type="button" class="btn btn-ghost btn-tiny" data-action="addMapEntry" data-act-owner="${owner}" data-route-index="${index}">+ Add ${isCc ? 'CC' : 'Note'}</button>` : ''}
      </div>` : ''}
    </div>`;
}

// Launchpad: remaps an older Launchpad's grid to a normal drum pad (from note 36), and gives each of
// its 16 round buttons an action.
const LP_BUTTON_LABELS = [...Array.from({ length: 8 }, (_, i) => `Top ${i + 1}`), ...Array.from({ length: 8 }, (_, i) => `Side ${i + 1}`)];

const QUAD_BLOCK_LABELS = ['Lower Left', 'Upper Left', 'Lower Right', 'Upper Right'];

function launchpadSectionHtml(route, index) {
  const on = !!(route.features & FEATURE.LAUNCHPAD);
  const buttons = state.lpActions.map((act, i) => `<div class="lp-button">
        <span class="lp-button-label">${LP_BUTTON_LABELS[i]}</span>
        ${actionEditorHtml(act, `data-act-owner="lp" data-act-index="${i}" data-route-index="${index}"`, noteSelectOptions, true, DIVISIONS)}
      </div>`).join('');
  const opt = (v, cur, l) => `<option value="${v}" ${cur === v ? 'selected' : ''}>${l}</option>`;
  const octave = Math.min(2, route.lpOctave || 0);
  const quad = route.lpQuad || defaultLpQuad();
  const qAttrs = `data-route-index="${index}"`;
  return `<div class="${featureRowClass(on)}">
      ${on ? `<div class="feature-head">
        ${switchHtml(index, 'launchpad', on, 'Launchpad')}
        ${switchHtml(index, 'lpQuadEnabled', quad.enabled, 'Break Off a Quad')}
      </div>
      <div class="arp-fields">
        <div class="arp-field"><label>Octave</label><select data-field="lpOctave" ${qAttrs}>${opt(0, octave, '0')}${opt(1, octave, '-1')}${opt(2, octave, '-2')}</select></div>
        ${quad.enabled ? `<div class="arp-field"><label>Quadrant</label><select data-lp-quad-field="block" ${qAttrs}>${QUAD_BLOCK_LABELS.map((l, i2) => opt(i2, quad.block, l)).join('')}</select></div>
        <div class="arp-field"><label>Quadrant Channel</label><select data-lp-quad-field="channel" ${qAttrs}>${Array.from({ length: 16 }, (_, c) => opt(c, quad.channel, c + 1)).join('')}</select></div>
        <div class="arp-field"><label>Quadrant Start Note</label><select data-lp-quad-field="startNote" ${qAttrs}>${noteSelectOptions(quad.startNote)}</select></div>` : ''}
      </div>` : switchHtml(index, 'launchpad', on, 'Launchpad')}
      ${on ? `<div class="lp-buttons-header">
        <p class="note-hint">Buttons light to show what they're doing. Button light color is a raw Launchpad velocity, used while the button is on. 0 keeps the usual colors.</p>
        <button type="button" class="btn btn-ghost btn-tiny" data-action="resetLpDefaults">Reset to Defaults</button>
      </div>
      <div class="lp-buttons">${buttons}</div>` : ''}
    </div>`;
}

routesContainer.addEventListener('change', (e) => {
  const field = e.target.dataset.lpQuadField;
  if (!field) return;
  const route = state.routes[parseInt(e.target.dataset.routeIndex, 10)];
  const quad = (route.lpQuad = route.lpQuad || defaultLpQuad());
  quad[field] = parseInt(e.target.value, 10);
  markDirty();
});

// Mapping anything to a looper action puts the button in Looper mode, the looper only runs there.
function ensureLooperMode() {
  if (state.global.buttonAction === 11) return;
  state.global.buttonAction = 11;
  renderGlobalCard();
  toast('Button Action set to LOOPER for the looper mapping', 'warn');
}

routesContainer.addEventListener('change', (e) => {
  const owner = e.target.dataset.actOwner;
  if (!owner) return;
  const index = parseInt(e.target.dataset.routeIndex, 10);
  const i = parseInt(e.target.dataset.actIndex, 10);
  if (e.target.dataset.mapSource !== undefined) {
    const entry = mapListFor(owner)[i];
    let v = parseInt(e.target.value, 10);
    if (!entry || Number.isNaN(v)) return;
    v = Math.min(127, Math.max(0, v));
    e.target.value = v;
    entry.source = v;
    markDirty();
    return;
  }
  const act = actionFor(owner, i);
  const field = e.target.dataset.actField;
  if (!act || !field) return;
  const wasLooper = usesLooper(act); // before applyActionField reassigns act's fields on a type change
  const redraw = applyActionField(act, field, e.target.value);
  if (usesLooper(act)) ensureLooperMode();
  markDirty();
  // A type change here can make looperMapped() flip either way (first Looper mapping added, or
  // the last one removed), which the per-route Looper switch and the Looper row's own collapse
  // both depend on — refresh every route, not just this one, to pick that up everywhere.
  if (redraw && (wasLooper || usesLooper(act))) renderRoutes();
  else if (redraw) refreshRouteCard(index);
  if (redraw || usesLiveChannel(act)) refreshArpCards();
});

routesContainer.addEventListener('click', async (e) => {
  const el = e.target.closest('[data-action="addMapEntry"], [data-action="removeMapEntry"], [data-action="learnMapSource"], [data-action="learnActOut"]');
  if (!el) return;
  const owner = el.dataset.actOwner;
  const index = parseInt(el.dataset.routeIndex, 10);
  const i = parseInt(el.dataset.actIndex, 10);
  const action = el.dataset.action;
  if (action === 'addMapEntry') {
    const list = mapListFor(owner);
    const last = list[list.length - 1];
    list.push({ source: last ? Math.min(127, last.source + 1) : (owner === 'cc' ? 20 : 36), action: last ? { ...last.action } : defaultAction(ACT.CC_MOMENTARY) });
  } else if (action === 'removeMapEntry') {
    mapListFor(owner).splice(i, 1);
    refreshArpCards();
  } else {
    if (!device.connected) { toast('Not connected to a device', 'error'); return; }
    const wantCc = action === 'learnActOut' || owner === 'cc';
    el.disabled = true;
    el.textContent = wantCc ? 'Move a control…' : 'Play a note…';
    try {
      const r = await device.learn();
      if (wantCc && r.type !== 2) toast('That was a note, move a control', 'error');
      else if (!wantCc && r.type !== 1) toast('That was a CC, play a note', 'error');
      else if (action === 'learnActOut') actionFor(owner, i).a = r.number;
      else mapListFor(owner)[i].source = r.number;
    } catch (err) {
      toast('Nothing heard in 10 seconds', 'error');
    }
  }
  markDirty();
  refreshRouteCard(index);
});

// A route with a Live Out Channel mapped on it shows its arp's Follows Live Ch Change switch.
function routeHasLiveChannel(route) {
  const inList = (list) => list.some((e) => usesLiveChannel(e.action));
  return ((route.features & FEATURE.NOTE_TO_CC) && inList(state.noteMap)) ||
    ((route.features & FEATURE.CC_MAP) && inList(state.ccMap)) ||
    ((route.features & FEATURE.LAUNCHPAD) && state.lpActions.some(usesLiveChannel));
}

function refreshArpCards() {
  state.routes.forEach((r, i) => { if (r.arpSlot) refreshRouteCard(i); });
}

// Scale: snaps notes to the key set in the yellow Scale row, with this route's Layout for how notes map in.
function scaleSectionHtml(route, index) {
  const on = !!(route.features & FEATURE.SCALE);
  return `<div class="${featureRowClass(on)}">
      ${switchHtml(index, 'scaleOn', on, 'Scale', 'switch-yellow')}
      ${on ? `<div class="scale-layout"><label>Layout</label>
        <select data-scale-field="layout" data-route-index="${index}">${LAYOUTS.map((l, i) => `<option value="${i}" ${(route.scaleLayout || 0) === i ? 'selected' : ''}>${l}</option>`).join('')}</select>
      </div>` : ''}
    </div>`;
}

routesContainer.addEventListener('change', (e) => {
  if (e.target.dataset.scaleField !== 'layout') return;
  state.routes[parseInt(e.target.dataset.routeIndex, 10)].scaleLayout = parseInt(e.target.value, 10);
  markDirty();
});

// Auto Chord: each note plays a chord. With Scale on the same route, the positions are scale
// degrees from the played note, so the chord stays in key. Without it, they're the 12 semitones.
const CHROMATIC_POSITION_LABELS = ['Root', 'm2', 'M2', 'm3', 'M3', 'P4', 'TT', 'P5', 'm6', 'M6', 'm7', 'M7'];

function chordPositionCount(route) {
  return (route.features & FEATURE.SCALE) ? (SCALES[state.scale.scaleType]?.steps.length || 7) : 12;
}

// Keeps only positions a route's chord can show, so no hidden position ever sounds.
function trimChordMask(route) {
  route.chordMask = (route.chordMask || 0) & ((1 << chordPositionCount(route)) - 1);
}

function trimChordMasks() {
  state.routes.forEach((r) => { if (r.features & FEATURE.AUTO_CHORD) trimChordMask(r); });
}

function autoChordSectionHtml(route, index) {
  const on = !!(route.features & FEATURE.AUTO_CHORD);
  const count = chordPositionCount(route);
  const diatonic = count !== 12;
  const chips = Array.from({ length: count }, (_, k) => `
    <label class="switch switch-port port-chip" title="${diatonic ? `Scale position ${k + 1}` : CHROMATIC_POSITION_LABELS[k]}">
      <input type="checkbox" data-chord-bit="${k}" data-route-index="${index}" ${(route.chordMask || 0) & (1 << k) ? 'checked' : ''}>
      <span class="switch-track"><span class="switch-thumb"></span></span>
      <span class="switch-label">${diatonic ? k + 1 : CHROMATIC_POSITION_LABELS[k]}</span>
    </label>`).join('');
  return `<div class="${featureRowClass(on)}">
      ${switchHtml(index, 'autoChord', on, 'Auto Chord', 'switch-yellow')}
      ${on ? `<div class="port-switches chord-positions">${chips}</div>` : ''}
    </div>`;
}

routesContainer.addEventListener('change', (e) => {
  const bit = e.target.dataset.chordBit;
  if (bit === undefined) return;
  const k = parseInt(bit, 10);
  const route = state.routes[parseInt(e.target.dataset.routeIndex, 10)];
  route.chordMask = e.target.checked ? ((route.chordMask || 0) | (1 << k)) : ((route.chordMask || 0) & ~(1 << k));
  markDirty();
});

// Arp: up to 4 routes, each in its own slot, all on the global clock's grid.
function arpSectionHtml(route, index) {
  const on = !!route.arpSlot;
  if (!on) return `<div class="${featureRowClass(false)}">${switchHtml(index, 'arpOn', false, 'Arp', 'switch-red')}</div>`;
  const s = route.arpSlot - 1;
  const a = state.arps[s];
  const opt = (v, cur, label) => `<option value="${v}" ${cur === v ? 'selected' : ''}>${label}</option>`;
  const field = (name, html) => `<div class="arp-field"><label>${name}</label>${html}</div>`;
  let holdChannels = opt(16, a.holdChannel, 'Any');
  for (let c = 0; c < 16; c++) holdChannels += opt(c, a.holdChannel, c + 1);
  const attrs = (f) => `data-arp-field="${f}" data-arp-slot="${s}" data-route-index="${index}"`;
  const velMode = a.aftertouch ? 2 : (a.velocity ? 1 : 0);
  const chOptions = (cur) => opt(0, cur, 'Off') + Array.from({ length: 16 }, (_, c) => opt(c + 1, cur, c + 1)).join('');
  const table = divisionTableHtml(a, s, index, 'Default, on/off');
  return `<div class="${featureRowClass(true)}">
      ${switchHtml(index, 'arpOn', true, `Arp ${route.arpSlot}`, 'switch-red')}
      <div class="arp-fields">
        ${field('Mode', `<select ${attrs('mode')}>${ARP_MODE_ORDER.map((i) => opt(i, a.mode, ARP_MODES[i])).join('')}</select>`)}
        ${field('Octaves', `<select ${attrs('octaves')}>${[1, 2, 3, 4].map((o) => opt(o, a.octaves, o)).join('')}</select>`)}
        ${field('Gate %', `<input type="number" min="5" max="100" value="${a.gate}" ${attrs('gate')}>`)}
        ${field('Velocity', `<select ${attrs('velocityMode')}>${opt(0, velMode, 'Played')}${opt(1, velMode, 'Fixed')}${opt(2, velMode, 'Aftertouch')}</select>
          ${velMode === 1 ? `<input type="number" min="1" max="127" value="${a.velocity}" ${attrs('velocity')}>` : ''}`)}
        ${routeHasLiveChannel(route) ? field('&nbsp;', `<label class="switch"><input type="checkbox" ${attrs('followsLiveChannel')} ${a.followsLiveChannel ? 'checked' : ''}><span class="switch-track"><span class="switch-thumb"></span></span><span class="switch-label">Follows Live Ch Change</span></label>`) : ''}
        ${field('Bypass ch', `<select ${attrs('bypassChannel')}>${chOptions(a.bypassChannel)}</select>`)}
        ${field('Bass ch', `<select ${attrs('bassChannel')}>${chOptions(a.bassChannel)}</select>`)}
        ${field('Hold', `<select ${attrs('holdType')}>${opt(0, a.holdType, 'None')}${opt(2, a.holdType, 'CC')}</select>
          ${a.holdType ? `<select ${attrs('holdChannel')} title="Channel">${holdChannels}</select>
          <input type="number" min="0" max="127" value="${a.holdNumber}" ${attrs('holdNumber')} title="CC number">` : ''}
          <button type="button" class="btn btn-ghost btn-tiny" data-action="learnArpHold" data-arp-slot="${s}" data-route-index="${index}">Learn</button>`)}
      </div>
      <label class="arp-table-label">Division</label>
      ${table}
    </div>`;
}

// Echo: each note the route sends repeats at the echo division, fading out over its repeats. A
// division square's trigger latches its division, pressed again it goes back to the default.
// Bounce (1.64): each gap is a harmonic fraction of the last. Bounce 1/2 halves it every repeat, a
// ball settling fast, out to 8/9. Slow is the same fractions upside down, so the gaps grow.
// Stored as -8..8, 8 the strongest Bounce.
const ECHO_BOUNCES = [
  ...Array.from({ length: 8 }, (_, k) => [-(8 - k), `Slow ${k + 2}/${k + 1}`]),
  [0, 'Off'],
  ...Array.from({ length: 8 }, (_, k) => [k + 1, `Bounce ${8 - k}/${9 - k}`])
];

routesContainer.addEventListener('change', (e) => {
  if (!('echoBounce' in e.target.dataset)) return;
  state.echo.bounce = parseInt(e.target.value, 10) || 0;
  markDirty();
});

// Per-Note Parameter Lock (1.59): hold a key on this route and move a CC, and that value is locked
// to the key, played back the next time it plays. One route at a time.
function plockSectionHtml(route, index) {
  const on = !!(route.features & FEATURE.PLOCK);
  if (!on) return `<div class="${featureRowClass(false)}">${switchHtml(index, 'plockOn', false, 'Per-Note Parameter Lock')}</div>`;
  const p = state.plock || defaultPlock();
  const opt = (v, l) => `<option value="${v}" ${p.delay === v ? 'selected' : ''}>${l}</option>`;
  return `<div class="${featureRowClass(true)}">
      <div class="feature-head">
        ${switchHtml(index, 'plockOn', true, 'Per-Note Parameter Lock')}
        ${switchHtml(index, 'plockGlide', !!p.glide, 'Glide')}
      </div>
      <div class="arp-fields">
        <div class="arp-field"><label>Delay</label><select data-plock-delay data-route-index="${index}">${DIVISIONS.map((d, i) => opt(i + 1, d)).join('')}${opt(0, 'None')}</select></div>
        <div class="arp-field arp-field-switch"><button type="button" class="btn btn-ghost btn-tiny" data-action="clearPlocks">Clear Locks</button></div>
      </div>
    </div>`;
}

routesContainer.addEventListener('change', (e) => {
  if (!('plockDelay' in e.target.dataset)) return;
  state.plock = state.plock || defaultPlock();
  state.plock.delay = parseInt(e.target.value, 10) || 0;
  markDirty();
});

routesContainer.addEventListener('click', (e) => {
  if (!e.target.closest('[data-action="clearPlocks"]')) return;
  if (!device.connected) { toast('Not connected to a device', 'error'); return; }
  device.clearParameterLocks();
  toast('Parameter locks cleared', 'success');
});

function echoSectionHtml(route, index) {
  const on = !!(route.features & FEATURE.ECHO);
  if (!on) return `<div class="${featureRowClass(false)}">${switchHtml(index, 'echoOn', false, 'Echo', 'switch-red')}</div>`;
  const e = state.echo;
  const attrs = (f) => `data-echo-field="${f}" data-route-index="${index}"`;
  return `<div class="${featureRowClass(true)}">
      <div class="feature-head">
        ${switchHtml(index, 'echoOn', true, 'Echo', 'switch-red')}
        ${switchHtml(index, 'echoGlobal', !!e.global, 'Global Echo')}
        ${switchHtml(index, 'echoLatching', e.latching !== false, 'Echo Latching')}
        ${e.global ? '' : switchHtml(index, 'echoAfter', e.afterRoute !== false, 'Echo After Route')}
      </div>
      <div class="arp-fields">
        <div class="arp-field"><label>Repeats</label><input type="number" min="1" max="32" value="${e.repeats}" ${attrs('repeats')}></div>
        <div class="arp-field"><label>Level %</label><input type="number" min="1" max="100" value="${e.level}" ${attrs('level')}></div>
        <div class="arp-field"><label>Bounce</label><select data-echo-bounce data-route-index="${index}">${ECHO_BOUNCES.map(([v, l]) => `<option value="${v}" ${(e.bounce || 0) === v ? 'selected' : ''}>${l}</option>`).join('')}</select></div>
        <div class="arp-field arp-field-switch">${switchHtml(index, 'echoNatural', !!e.naturalFade, 'Natural Fade')}</div>
      </div>
      <label class="arp-table-label">Division</label>
      ${divisionTableHtml(e, 'echo', index, e.latching !== false ? 'Default, on/off' : 'Default')}
    </div>`;
}

// Stutter: holding a division square's note or CC loops that length of what the route just played,
// muting it live, until released or the runout. Every square is momentary, the default too.
function stutterSectionHtml(route, index) {
  const on = !!(route.features & FEATURE.STUTTER);
  if (!on) return `<div class="${featureRowClass(false)}">${switchHtml(index, 'stutterOn', false, 'Stutter', 'switch-red')}</div>`;
  const st = state.stutter;
  return `<div class="${featureRowClass(true)}">
      <div class="feature-head">
        ${switchHtml(index, 'stutterOn', true, 'Stutter', 'switch-red')}
        ${switchHtml(index, 'stutterGlobal', !!st.global, 'Global Stutter')}
        ${switchHtml(index, 'stutterLatching', !!st.latching, 'Stutter Latching')}
        ${st.global ? '' : switchHtml(index, 'stutterAfter', st.afterRoute !== false, 'Stutter After Route')}
      </div>
      <div class="arp-fields">
        <div class="arp-field"><label>Runout Bars</label><select data-stutter-field="maxBars" data-route-index="${index}">${STUTTER_RUNOUTS.map((v) => `<option value="${v}" ${st.maxBars === v ? 'selected' : ''}>${v === 0 ? 'Off' : v}</option>`).join('')}</select></div>
      </div>
      <label class="arp-table-label">Division</label>
      ${divisionTableHtml(st, 'stutter', index, '')}
    </div>`;
}

routesContainer.addEventListener('change', (e) => {
  if (e.target.dataset.stutterField !== 'maxBars') return;
  const v = parseInt(e.target.value, 10);
  state.stutter.maxBars = STUTTER_RUNOUTS.includes(v) ? v : 0;
  markDirty();
});

routesContainer.addEventListener('change', (e) => {
  const field = e.target.dataset.echoField;
  if (!field) return;
  const limits = { repeats: [1, 32], level: [1, 100] }[field];
  let v = parseInt(e.target.value, 10);
  if (Number.isNaN(v)) v = defaultEcho()[field];
  v = Math.min(limits[1], Math.max(limits[0], v));
  e.target.value = v;
  state.echo[field] = v;
  markDirty();
});

// A division table: the default square, then up to 7 squares a learned note or CC switches to. The
// default square's trigger turns the arp or echo off and on, and stutters like the others.
// owner is an arp slot's settings (key 0-3), the echo's (key 'echo') or the stutter's (key 'stutter').
function divisionTableHtml(owner, key, index, defaultLabel) {
  const opt = (v, cur, label) => `<option value="${v}" ${cur === v ? 'selected' : ''}>${label}</option>`;
  const ids = `data-arp-slot="${key}" data-route-index="${index}"`;
  // Stutter also has 4 and 2 bars, listed longest first above the musical divisions.
  const list = key === 'stutter' ? [[20, '4 bars'], [19, '2 bars'], ...DIVISIONS.map((d, i) => [i, d])] : DIVISIONS.map((d, i) => [i, d]);
  const divSelect = (k, cur) => `<select data-arp-square="${k}" ${ids}>${list.map(([i, d]) => opt(i, cur, d)).join('')}</select>`;
  const squares = owner.squares.map((sq, k) => sq && `
      <div class="arp-square">
        ${divSelect(k, sq.division)}
        <span class="arp-trigger">${triggerLabel(sq)}</span>
        <span class="arp-square-actions">
          <button type="button" class="btn btn-ghost btn-tiny" data-action="learnArpSquare" data-square="${k}" ${ids}>Learn</button>
          <button type="button" class="btn btn-ghost btn-tiny" data-action="removeArpSquare" data-square="${k}" ${ids} title="Remove">×</button>
        </span>
      </div>`).filter(Boolean).join('');
  const canAdd = owner.squares.some((sq) => !sq);
  const def = owner.defaultTrigger || noTrigger();
  return `<div class="arp-divisions">
      <div class="arp-square arp-square-default">
        ${divSelect(-1, owner.division)}
        <span class="arp-trigger">${[defaultLabel, def.triggerType ? triggerLabel(def) : ''].filter(Boolean).join(' · ') || 'No trigger'}</span>
        <span class="arp-square-actions">
          <button type="button" class="btn btn-ghost btn-tiny" data-action="learnArpSquare" data-square="-1" ${ids}>Learn</button>
          ${def.triggerType ? `<button type="button" class="btn btn-ghost btn-tiny" data-action="removeArpSquare" data-square="-1" ${ids} title="Clear the trigger">×</button>` : ''}
        </span>
      </div>
      ${squares}
      ${canAdd ? `<button type="button" class="arp-square arp-square-add" data-action="addArpSquare" ${ids} title="Add a division">+</button>` : ''}
    </div>`;
}

function tableOwner(key) {
  if (key === 'echo') return state.echo;
  if (key === 'stutter') return state.stutter;
  return state.arps[parseInt(key, 10)];
}

function triggerLabel(sq) {
  if (!sq.triggerType) return 'No trigger';
  const ch = sq.triggerChannel > 15 ? 'any ch' : `ch ${sq.triggerChannel + 1}`;
  return `${sq.triggerType === 1 ? 'Note' : 'CC'} ${sq.triggerNumber} · ${ch}`;
}

routesContainer.addEventListener('change', (e) => {
  const k = e.target.dataset.arpSquare;
  if (k === undefined) return;
  const a = tableOwner(e.target.dataset.arpSlot);
  const v = parseInt(e.target.value, 10);
  if (k === '-1') a.division = v;
  else a.squares[parseInt(k, 10)].division = v;
  markDirty();
});

routesContainer.addEventListener('click', async (e) => {
  const el = e.target.closest('[data-action="addArpSquare"], [data-action="removeArpSquare"], [data-action="learnArpSquare"]');
  if (!el) return;
  const a = tableOwner(el.dataset.arpSlot);
  const routeIndex = parseInt(el.dataset.routeIndex, 10);
  const action = el.dataset.action;
  if (action === 'addArpSquare') {
    const free = a.squares.findIndex((sq) => !sq);
    if (free >= 0) a.squares[free] = { division: 8, triggerType: 0, triggerChannel: 16, triggerNumber: 0 }; // 1/8
  } else if (action === 'removeArpSquare') {
    const k = parseInt(el.dataset.square, 10);
    if (k < 0) a.defaultTrigger = noTrigger();
    else a.squares[k] = null;
  } else {
    if (!device.connected) { toast('Not connected to a device', 'error'); return; }
    el.disabled = true;
    el.textContent = 'Play a note or move a control…';
    try {
      const r = await device.learn();
      const k = parseInt(el.dataset.square, 10);
      const trigger = { triggerType: r.type === 1 ? 1 : 2, triggerChannel: r.channel, triggerNumber: r.number };
      if (k < 0) a.defaultTrigger = trigger;
      else Object.assign(a.squares[k], trigger);
    } catch (err) {
      toast('Nothing heard in 10 seconds', 'error');
    }
  }
  markDirty();
  refreshRouteCard(routeIndex);
});

const ARP_STRUCTURAL = new Set(['velocityMode', 'holdType']);

routesContainer.addEventListener('change', (e) => {
  const field = e.target.dataset.arpField;
  if (!field) return;
  const a = state.arps[parseInt(e.target.dataset.arpSlot, 10)];
  if (a && field === 'followsLiveChannel') {
    a.followsLiveChannel = e.target.checked;
    markDirty();
    return;
  }
  let v = parseInt(e.target.value, 10);
  if (!a || Number.isNaN(v)) return;
  if (field === 'velocityMode') {
    a.aftertouch = v === 2;
    a.velocity = v === 1 ? (a.velocity || 100) : 0;
  } else {
    const limits = { gate: [5, 100], velocity: [1, 127], holdNumber: [0, 127] }[field];
    if (limits) { v = Math.min(limits[1], Math.max(limits[0], v)); e.target.value = v; }
    a[field] = v;
  }
  markDirty();
  if (ARP_STRUCTURAL.has(field)) refreshRouteCard(parseInt(e.target.dataset.routeIndex, 10));
});

routesContainer.addEventListener('click', async (e) => {
  const el = e.target.closest('[data-action="learnArpHold"]');
  if (!el) return;
  if (!device.connected) { toast('Not connected to a device', 'error'); return; }
  const a = state.arps[parseInt(el.dataset.arpSlot, 10)];
  el.disabled = true;
  el.textContent = 'Move a control…';
  try {
    const r = await device.learn();
    if (r.type !== 2) toast('That was a note, move a control', 'error');
    else Object.assign(a, { holdType: 2, holdChannel: r.channel, holdNumber: r.number });
    markDirty();
  } catch (err) {
    toast('Nothing heard in 10 seconds', 'error');
  }
  refreshRouteCard(parseInt(el.dataset.routeIndex, 10));
});

// A feature switch that's off takes half the column, so off switches sit two to a row. Its section
// takes the whole width once it's on.
// The switch rows under CC Map Start. Closed ones stay on top in their usual order, and the ones
// opened up (settings or a hint showing) pile up below, so no closed switch sits between open ones.
function toggleRowsHtml(route, index) {
  const rows = [];
  if (looperMapped()) rows.push([false, `<div class="field-row">
              ${switchHtml(index, 'looperEnabled', route.looperEnabled !== false, 'Looper', 'switch-green')}
            </div>`]);
  rows.push([!!route.monoRetrig, `<div class="field-row">
              ${switchHtml(index, 'monoRetrig', !!route.monoRetrig, 'Mono Retrig')}
              ${route.monoRetrig ? '<p class="note-hint">Poly to Mono. Helps samplers play more like analog CV synths. Lifting a key recalls the next still-held key.</p>' : ''}
            </div>`]);
  rows.push([!!route.roundRobin, `<div class="field-row">
              <div class="round-robin-row">
                ${switchHtml(index, 'roundRobin', !!route.roundRobin, 'Round Robin')}
                ${route.roundRobin ? switchHtml(index, 'roundRobinRandom', !!route.roundRobinRandom, 'Rand', 'cc-map-row-indent') : ''}
              </div>
              ${route.roundRobin ? `<p class="note-hint">Mono to Poly. ${route.roundRobinRandom ? 'Picks a random selected channel for each new note.' : 'Cycles each new note to a different selected channel.'}</p>` : ''}
            </div>`]);
  if (featuresSupported()) {
    const sections = [mapSectionHtml(route, index, 'note'), mapSectionHtml(route, index, 'cc'), launchpadSectionHtml(route, index),
      scaleSectionHtml(route, index), autoChordSectionHtml(route, index), arpSectionHtml(route, index),
      echoSectionHtml(route, index), stutterSectionHtml(route, index), plockSectionHtml(route, index)];
    for (const html of sections) rows.push([html.trimStart().startsWith(`<div class="${featureRowClass(true)}"`), html]);
  }
  return rows.filter(r => !r[0]).map(r => r[1]).join('') + rows.filter(r => r[0]).map(r => r[1]).join('');
}
function featureRowClass(on) {
  return on ? 'field-row field-row-wide feature-open' : 'field-row';
}

// Turning a single-route feature on in one route turns it off everywhere else.
function claimFeature(index, bit, label) {
  state.routes.forEach((r, i) => {
    if (i !== index && (r.features & bit)) {
      r.features &= ~bit;
      toast(`${label} moved here from route ${i + 1}`, 'warn');
      refreshRouteCard(i);
    }
  });
}

function renderRoutes() {
  renderScaleRow();
  renderLooperRow();
  emptyHint.hidden = state.routes.length !== 0;
  routesContainer.innerHTML = state.routes.map((r, i) => renderRouteCard(r, i)).join('');
}

// Toggling a single card's note/cc range doesn't shift anyone else's index or badge, so refresh
// just that card instead of re-rendering the whole (possibly 256-long) list.
function refreshRouteCard(index, animateStaple) {
  const el = routesContainer.querySelector(`.route-card[data-route-index="${index}"]`);
  if (!el) { renderRoutes(); return; }
  el.outerHTML = renderRouteCard(state.routes[index], index, animateStaple);
}

// Structural changes (need a re-render): checkboxes that reveal/hide fields, staple toggle, delete
const STRUCTURAL_FIELDS = new Set(['note', 'cc', 'ccMapEnabled', 'aftertouch', 'aftertouchMap', 'pressure', 'pressureMap', 'allInputTypes', 'allInputChannels', 'allInPorts', 'allOutPorts', 'monoRetrig', 'roundRobin', 'roundRobinRandom', 'noteToCc', 'ccMapOn', 'launchpad', 'scaleOn', 'autoChord', 'arpOn', 'echoOn', 'stutterOn', 'lpQuadEnabled', 'stutterGlobal', 'echoGlobal', 'echoLatching', 'plockOn']);

routesContainer.addEventListener('click', (e) => {
  const actionEl = e.target.closest('[data-action]');
  if (!actionEl) return;
  const index = parseInt(actionEl.dataset.routeIndex, 10);
  const action = actionEl.dataset.action;
  if (action === 'resetLpDefaults') {
    state.lpActions = defaultLpActions();
    markDirty();
    renderRoutes();
    toast('Launchpad buttons reset to defaults', 'success');
    return;
  }
  if (action === 'delete') {
    deleteRoute(index);
  } else if (action === 'toggleCollapsed' || action === 'expand') {
    const route = state.routes[index];
    route._collapsed = action === 'expand' ? false : !route._collapsed;
    refreshRouteCard(index, true); // SMIL <animate> plays on insertion even after a full rebuild
    refreshMasterStaple(true);
  } else if (action === 'setNoteRangeAll') {
    state.routes[index].noteStart = 0;
    state.routes[index].noteEnd = 127;
    refreshRouteCard(index);
    syncRouteLive(index);
  } else if (action === 'setCcRangeAll') {
    state.routes[index].ccStart = 0;
    state.routes[index].ccEnd = 127;
    refreshRouteCard(index);
    syncRouteLive(index);
  }
});


routesContainer.addEventListener('change', (e) => {
  const index = e.target.dataset.routeIndex;
  const field = e.target.dataset.field;
  if (index === undefined || !field) return;
  const route = state.routes[parseInt(index, 10)];
  applyRouteFieldChange(route, field, e.target);
  // These three decide whether the looper is reachable, which every route's Looper switch and
  // the Looper row itself follow, so they redraw everything rather than just this card.
  if (field === 'launchpad' || field === 'noteToCc' || field === 'ccMapOn') renderRoutes();
  else if (STRUCTURAL_FIELDS.has(field)) refreshRouteCard(parseInt(index, 10));
  if (field === 'outputChannels') updateChannelWarning(parseInt(index, 10));
  syncMasterSwitches(parseInt(index, 10));
  syncRouteLive(parseInt(index, 10));
});

// Double-clicking a channel box (1.68) leaves only that channel selected on that side of the route.
// The two clicks of the double-click have already flipped it twice, so this sets it outright.
routesContainer.addEventListener('dblclick', (e) => {
  const label = e.target.closest('.channel-chip label');
  const box = label && document.getElementById(label.htmlFor);
  const field = box?.dataset.field;
  if (field !== 'inputChannels' && field !== 'outputChannels') return;
  e.preventDefault();
  const index = parseInt(box.dataset.routeIndex, 10);
  state.routes[index][field] = 1 << parseInt(box.dataset.bit, 10);
  box.closest('.channel-grid').querySelectorAll('input').forEach((el) => { el.checked = el === box; });
  if (field === 'outputChannels') updateChannelWarning(index);
  syncMasterSwitches(index);
  syncRouteLive(index);
});

routesContainer.addEventListener('input', (e) => {
  const index = e.target.dataset.routeIndex;
  const field = e.target.dataset.field;
  if (index === undefined || !field) return;
  if (e.target.tagName === 'SELECT') return; // handled on 'change'
  const route = state.routes[parseInt(index, 10)];
  applyRouteFieldChange(route, field, e.target);
  syncRouteLive(parseInt(index, 10));
});

// Enter doesn't submit anything here (no <form>), so without this it looked like it did nothing —
// blurring gives the same "committed" visual as clicking away, which also fires 'change' on these
// text/number fields (blur is a change trigger) so the value is actually saved at that point too.
routesContainer.addEventListener('keydown', (e) => {
  if (e.key !== 'Enter') return;
  const el = e.target;
  if (el.matches('.route-title-input, input[type="number"]')) {
    e.preventDefault();
    el.blur();
  }
});

function applyRouteFieldChange(route, field, el) {
  if (field === 'name') {
    // Belt-and-suspenders beyond the input's own maxlength — the wire format is a fixed 24-byte
    // ASCII field, so anything longer or outside printable ASCII would just get silently truncated
    // or replaced with '?' at save time. Keeping it in sync here means what's on screen is always
    // exactly what will actually get stored.
    const clean = el.value.slice(0, ROUTE_NAME_MAX_LEN).replace(/[^\x20-\x7E]/g, '?');
    route.name = clean;
    if (clean !== el.value) el.value = clean;
    return;
  }
  if (field === 'inputChannels' || field === 'outputChannels' || field === 'inPorts' || field === 'outPorts') {
    const bit = parseInt(el.dataset.bit, 10);
    if (el.checked) route[field] |= (1 << bit); else route[field] &= ~(1 << bit);
    return;
  }
  if (field === 'note') { setTypeFlag(route, DATA_TYPE_BITS.note, el.checked); return; }
  if (field === 'cc') { setTypeFlag(route, DATA_TYPE_BITS.cc, el.checked); return; }
  if (field === 'programChange') { setTypeFlag(route, DATA_TYPE_BITS.programChange, el.checked); return; }
  if (field === 'pitchBend') { setTypeFlag(route, DATA_TYPE_BITS.pitchBend, el.checked); return; }
  if (field === 'aftertouch') { setTypeFlag(route, DATA_TYPE_BITS.aftertouch, el.checked); return; }
  if (field === 'aftertouchMap') { setTypeFlag(route, DATA_TYPE_BITS.aftertouchMap, el.checked); return; }
  if (field === 'pressure') { setTypeFlag(route, DATA_TYPE_BITS.pressure, el.checked); return; }
  if (field === 'pressureMap') { route.cpMapEnabled = el.checked; return; }
  if (field === 'sysex') { setTypeFlag(route, DATA_TYPE_BITS.sysex, el.checked); return; }
  if (field === 'common') { route.commonEnabled = el.checked; return; }
  if (field === 'realtime') { route.realtimeEnabled = el.checked; return; }
  if (field === 'clock') { route.clockEnabled = el.checked; return; }
  if (field === 'monoRetrig') { route.monoRetrig = el.checked; return; }
  if (field === 'looperEnabled') { route.looperEnabled = el.checked; renderLooperRow(); return; }
  if (field === 'roundRobin') { route.roundRobin = el.checked; return; }
  if (field === 'roundRobinRandom') { route.roundRobinRandom = el.checked; return; }
  if (field === 'ccMapEnabled') {
    route._ccMapEnabled = el.checked;
    if (!el.checked) route.ccMapStart = route.ccStart; // switching off: snap back to "no remap"
    return;
  }
  if (field === 'allInputTypes') {
    // Flips every top-level input data-type switch at once — Note Range, CC Range, Program Change,
    // Pitch Bend, Clock, Ch Pressure Aftertouch, Poly Aftertouch, Sysex, Common, Realtime.
    // Deliberately leaves the sub-switches (AT to CC, Map, and the actual note/cc range values)
    // untouched, and leaves Mono Retrig/Round Robin alone too, since those are Output-side, not Input.
    const on = el.checked;
    route.typeFlags = on ? (route.typeFlags | 0x7F) : (route.typeFlags & ~0x7F);
    route.commonEnabled = on;
    route.realtimeEnabled = on;
    route.clockEnabled = on;
    return;
  }
  if (field === 'noteToCc' || field === 'ccMapOn') {
    const isCc = field === 'ccMapOn';
    const bit = isCc ? FEATURE.CC_MAP : FEATURE.NOTE_TO_CC;
    if (el.checked) {
      claimFeature(state.routes.indexOf(route), bit, isCc ? 'CC to Feature' : 'Note to Feature');
      const list = mapListFor(isCc ? 'cc' : 'note');
      if (!list.length) list.push({ source: isCc ? 20 : 36, action: defaultAction(ACT.CC_MOMENTARY) });
    }
    route.features = el.checked ? (route.features | bit) : (route.features & ~bit);
    refreshArpCards();
    return;
  }
  if (field === 'scaleOn') {
    route.features = el.checked ? (route.features | FEATURE.SCALE) : (route.features & ~FEATURE.SCALE);
    if (route.features & FEATURE.AUTO_CHORD) trimChordMask(route);
    renderScaleRow();
    return;
  }
  if (field === 'arpOn') {
    if (el.checked) {
      const used = new Set(state.routes.filter((r) => r !== route).map((r) => r.arpSlot));
      const free = [1, 2, 3, 4].find((n) => !used.has(n));
      if (!free) {
        el.checked = false;
        toast('All 4 arps are in use', 'error');
        return;
      }
      route.arpSlot = free;
    } else {
      route.arpSlot = 0;
    }
    renderClockRow();
    return;
  }
  if (field === 'autoChord') {
    if (el.checked) {
      route.features |= FEATURE.AUTO_CHORD;
      trimChordMask(route);
      // Starts as a major chord: positions 1, 3, 5 in a scale, or root, M3, P5 without one.
      if (!route.chordMask) route.chordMask = (route.features & FEATURE.SCALE) ? 0b10101 : 0b10010001;
    } else {
      route.features &= ~FEATURE.AUTO_CHORD;
    }
    return;
  }
  if (field === 'stutterOn') {
    if (el.checked) claimFeature(state.routes.indexOf(route), FEATURE.STUTTER, 'Stutter');
    route.features = el.checked ? (route.features | FEATURE.STUTTER) : (route.features & ~FEATURE.STUTTER);
    renderClockRow();
    return;
  }
  if (field === 'echoOn') {
    if (el.checked) claimFeature(state.routes.indexOf(route), FEATURE.ECHO, 'Echo');
    route.features = el.checked ? (route.features | FEATURE.ECHO) : (route.features & ~FEATURE.ECHO);
    renderClockRow();
    return;
  }
  if (field === 'stutterAfter') { state.stutter.afterRoute = el.checked; return; }
  if (field === 'stutterLatching') { state.stutter.latching = el.checked; return; }
  if (field === 'stutterGlobal') { state.stutter.global = el.checked; return; }
  if (field === 'echoAfter') { state.echo.afterRoute = el.checked; return; }
  if (field === 'echoLatching') { state.echo.latching = el.checked; return; }
  if (field === 'echoNatural') { state.echo.naturalFade = el.checked; return; }
  if (field === 'plockGlide') { state.plock = state.plock || defaultPlock(); state.plock.glide = el.checked; return; }
  if (field === 'plockOn') {
    if (el.checked) claimFeature(state.routes.indexOf(route), FEATURE.PLOCK, 'Per-Note Parameter Lock');
    route.features = el.checked ? (route.features | FEATURE.PLOCK) : (route.features & ~FEATURE.PLOCK);
    return;
  }
  if (field === 'echoGlobal') { state.echo.global = el.checked; return; }
  if (field === 'lpOctave') {
    route.lpOctave = parseInt(el.value, 10);
    return;
  }
  if (field === 'lpQuadEnabled') {
    const quad = (route.lpQuad = route.lpQuad || defaultLpQuad());
    quad.enabled = el.checked;
    return;
  }
  if (field === 'launchpad') {
    if (el.checked) claimFeature(state.routes.indexOf(route), FEATURE.LAUNCHPAD, 'Launchpad');
    route.features = el.checked ? (route.features | FEATURE.LAUNCHPAD) : (route.features & ~FEATURE.LAUNCHPAD);
    return;
  }
  if (field === 'allInPorts' || field === 'allOutPorts') {
    // Flips every port switch shown in that row, leaving hidden sidecar ports alone.
    const portField = field === 'allInPorts' ? 'inPorts' : 'outPorts';
    const mask = visiblePortMask(route, portField);
    route[portField] = el.checked ? (route[portField] | mask) : (route[portField] & ~mask);
    return;
  }
  if (field === 'allInputChannels') {
    route.inputChannels = el.checked ? 0xFFFF : 0;
    return;
  }
  if (['noteStart', 'noteEnd', 'ccStart', 'ccEnd', 'transpose', 'velocityScale', 'ccMapStart', 'atMapCC', 'cpMapCC'].includes(field)) {
    const raw = el.value.trim();
    // "-" (and "") are valid in-progress states while typing a negative transpose value — bail
    // without touching state or the field so the next keystroke can complete the number. Clamping
    // eagerly here used to stomp "-" back to "0" before a second digit could ever be typed.
    if (raw === '' || raw === '-') return;
    let value = parseInt(raw, 10);
    if (Number.isNaN(value)) return;
    // Clamp to the field's own min/max (transpose is -64..63, velocityScale is 10..200, everything
    // else is 0..127) instead of just storing whatever was typed — a value outside the wire
    // format's range would either get silently truncated on the device or corrupt the byte, so
    // clamp here where it's visible.
    const min = el.min !== '' ? parseInt(el.min, 10) : -Infinity;
    const max = el.max !== '' ? parseInt(el.max, 10) : Infinity;
    value = Math.min(max, Math.max(min, value));
    route[field] = value;
    if (String(value) !== el.value) el.value = value;
    // While CC mapping is off, CC Map Start just mirrors ccStart (visibly, and in the data actually
    // sent to the device) — a targeted DOM write, not a refresh, so this field doesn't steal focus
    // away from whatever's still being typed in ccStart.
    if (field === 'ccStart' && !route._ccMapEnabled) {
      route.ccMapStart = value;
      const mapInput = routesContainer.querySelector(`.route-card[data-route-index="${el.dataset.routeIndex}"] [data-field="ccMapStart"]`);
      if (mapInput) mapInput.value = value;
    }
    return;
  }
}

function setTypeFlag(route, bit, on) {
  route.typeFlags = on ? (route.typeFlags | bit) : (route.typeFlags & ~bit);
}

function syncRouteLive(index) {
  markDirty();
}

// Firmware 1.2 and later hold 64 routes and report it in IDENTITY_REPLY. Firmware 1.0 reports 256
// but only keeps 99 after a restart, so the limit never goes above 64.
const ROUTE_LIMIT = 64;
function routeLimit() {
  return Math.min(ROUTE_LIMIT, device.deviceInfo?.maxRoutes || ROUTE_LIMIT);
}

function addRoute() {
  if (state.routes.length >= routeLimit()) {
    toast(`The FoxWizard holds up to ${routeLimit()} routes`, 'error');
    return;
  }
  state.routes.push(defaultRoute());
  renderRoutes();
  refreshMasterStaple();
  markDirty();
}

function deleteRoute(index) {
  state.routes.splice(index, 1);
  renderRoutes();
  refreshMasterStaple();
  markDirty();
}

function reorderRoutes(from, to) {
  const [moved] = state.routes.splice(from, 1);
  state.routes.splice(to, 0, moved);
  renderRoutes();
  markDirty();
}

attachDragReorder(routesContainer, { itemSelector: '.route-card', onReorder: reorderRoutes });

// ============================================================================================
// Toolbar: preset file + device bulk sync
// ============================================================================================
document.getElementById('addRouteBtn').addEventListener('click', addRoute);
document.getElementById('addRouteBtnTop').addEventListener('click', addRoute);
document.getElementById('emptyHintAddRoute').addEventListener('click', (e) => { e.preventDefault(); addRoute(); });

document.getElementById('downloadPresetBtn').addEventListener('click', () => {
  downloadPreset({ name: state.presetName, global: state.global, routes: state.routes, noteMap: state.noteMap, ccMap: state.ccMap, lpActions: state.lpActions, scale: state.scale, arps: state.arps, echo: state.echo, stutter: state.stutter, looper: state.looper, plock: state.plock });
  toast('Preset downloaded', 'success');
});

document.getElementById('uploadPresetInput').addEventListener('change', async (e) => {
  const file = e.target.files[0];
  e.target.value = '';
  if (!file) return;
  try {
    const preset = await readPresetFile(file);
    state.presetName = preset.name;
    state.global = { ...defaultGlobal(), ...preset.global };
    state.noteMap = preset.noteMap || [];
    state.ccMap = preset.ccMap || [];
    state.lpActions = preset.lpActions || defaultLpActions();
    state.scale = preset.scale || defaultScale();
    state.arps = preset.arps || Array.from({ length: ARP_SLOTS }, defaultArp);
    state.echo = preset.echo || defaultEcho();
    state.stutter = preset.stutter || defaultStutter();
    state.looper = preset.looper || defaultLooper();
    state.plock = preset.plock || defaultPlock();
    state.routes = preset.routes.map((r) => ({
      _uid: nextUid++, _collapsed: false, _ccMapEnabled: r.ccMapStart !== r.ccStart, ...r
    }));
    renderGlobalCard();
    renderRoutes();
    markDirty();
    if (state.routes.length > routeLimit()) {
      toast(`This preset has ${state.routes.length} routes. The FoxWizard holds ${routeLimit()}, delete some before saving.`, 'error');
    } else {
      toast('Preset loaded — click "Save to Device" to push it', 'success');
    }
  } catch (err) {
    toast('Could not read that preset file', 'error');
  }
});

document.getElementById('loadFromDeviceBtn').addEventListener('click', () => loadFromDevice(false));

async function loadFromDevice(silent) {
  if (!device.connected) { if (!silent) toast('Not connected to a device', 'error'); return; }
  showOverlay('Loading settings from device…');
  try {
    const { global, routes, features } = await device.loadAll((done, total) => setOverlayProgress(done, total));
    state.noteMap = features.noteMap || [];
    state.ccMap = features.ccMap || [];
    state.lpActions = features.lpActions || defaultLpActions();
    state.scale = features.scale || defaultScale();
    state.arps = features.arps || Array.from({ length: ARP_SLOTS }, defaultArp);
    state.echo = features.echo || defaultEcho();
    state.stutter = features.stutter || defaultStutter();
    state.looper = features.looper || defaultLooper();
    state.plock = features.plock || defaultPlock();
    state.global = { ...defaultGlobal(), ...global }; // older firmware sends no clock settings
    state.routes = routes.map((r) => ({
      _uid: nextUid++, _collapsed: false, _ccMapEnabled: r.ccMapStart !== r.ccStart, name: '', ...r
    }));
    renderGlobalCard();
    renderRoutes();
    clearDirty(); // freshly pulled from the device, so local state now matches it exactly
    if (!silent) toast('Loaded settings from device', 'success');
  } catch (err) {
    toast('Failed to load settings from device', 'error');
  } finally {
    hideOverlay();
  }
}

saveToDeviceBtn.addEventListener('click', async () => {
  if (!device.connected) { toast('Not connected to a device', 'error'); return; }
  if (state.routes.length > routeLimit()) {
    toast(`The FoxWizard holds up to ${routeLimit()} routes. Delete ${state.routes.length - routeLimit()} before saving.`, 'error');
    return;
  }
  showOverlay('Sending settings to device…');
  try {
    await device.saveAll(state.global, state.routes, { noteMap: state.noteMap, ccMap: state.ccMap, lpActions: state.lpActions, scale: state.scale, arps: state.arps, echo: state.echo, stutter: state.stutter, looper: state.looper, plock: state.plock }, (done, total) => setOverlayProgress(done, total));
    clearDirty();
    toast('Saved to device', 'success');
  } catch (err) {
    toast('Failed to save settings to device', 'error');
  } finally {
    hideOverlay();
  }
});

// ============================================================================================
// Install prompt (PWA)
// ============================================================================================
// Best-effort "already installed" detection — supported browsers report this via matchMedia,
// Safari on iOS via navigator.standalone. If neither is available/true we just fall back to the
// normal beforeinstallprompt flow, which Chrome typically won't fire anyway once installed.
const runningStandalone = window.matchMedia?.('(display-mode: standalone)').matches || window.navigator.standalone === true;

let deferredInstallPrompt = null;
if (!runningStandalone) {
  window.addEventListener('beforeinstallprompt', (e) => {
    e.preventDefault();
    deferredInstallPrompt = e;
    document.getElementById('installBtn').hidden = false;
  });
  document.getElementById('installBtn').addEventListener('click', async () => {
    if (!deferredInstallPrompt) return;
    deferredInstallPrompt.prompt();
    await deferredInstallPrompt.userChoice;
    deferredInstallPrompt = null;
    document.getElementById('installBtn').hidden = true;
  });
}

// ============================================================================================
// Boot
// ============================================================================================
renderGlobalCard();
renderRoutes();
device.init();
