import { DeviceLink, DATA_TYPE_BITS, ROUTE_NAME_MAX_LEN, PORTS, PORT_TRS, PORT_USB, SIDECAR_INPUT_PORTS, SIDECAR_OUTPUT_PORTS } from './midi.js';
import { serializePreset, parsePreset, downloadPreset, readPresetFile, channelsToRangeString } from './presetXml.js';
import { attachDragReorder } from './dragReorder.js';

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
  return { mode: 0, buttonAction: 0, buttonParamA: 64, buttonParamB: 0, buttonToggleMomentary: 0, ledBrightnessStep: 10 };
}

function defaultRoute() {
  return {
    _uid: nextUid++,
    _collapsed: false, // UI-only, never sent to the device or saved in presets
    _ccMapEnabled: false, // UI-only — off means the CC Map Start field just mirrors ccStart (no remap)
    name: '',
    enabled: true,
    // Every port this FoxWizard has right now, so a fresh route is a full passthrough.
    inPorts: PORT_TRS | PORT_USB | (sidecarPresent ? SIDECAR_INPUT_PORTS : 0),
    inputChannels: 0x0001, // ch1 only
    // All top-level input data types on by default (every bit except bit7/aftertouchMap, which is
    // a sub-switch under Poly Aftertouch, not one of the top-level switches the master toggle
    // controls) — a fresh route works as a full passthrough immediately, narrow down from there.
    typeFlags: 0x7F,
    noteStart: 0, noteEnd: 127,
    ccStart: 0, ccEnd: 127,
    outPorts: PORT_TRS | PORT_USB | (sidecarPresent ? SIDECAR_OUTPUT_PORTS : 0),
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
    cpMapCC: 0
  };
}

const state = {
  presetName: 'Untitled Preset',
  global: defaultGlobal(),
  routes: []
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
  overlay.hidden = false;
}
function setOverlayProgress(done, total) {
  overlayProgress.textContent = total ? `${done} / ${total}` : '';
}
function hideOverlay() { overlay.hidden = true; }

let toastStack = document.querySelector('.toast-stack');
if (!toastStack) {
  toastStack = document.createElement('div');
  toastStack.className = 'toast-stack';
  document.body.appendChild(toastStack);
}
// sticky: stays until clicked, for things worth reading after the fact.
function toast(message, kind, sticky) {
  const el = document.createElement('div');
  el.className = 'toast' + (kind ? ` toast-${kind}` : '') + (sticky ? ' toast-sticky' : '');
  el.textContent = message;
  toastStack.appendChild(el);
  if (sticky) {
    el.title = 'Click to dismiss';
    el.addEventListener('click', () => el.remove());
  } else {
    setTimeout(() => el.remove(), 3200);
  }
}

const saveToDeviceBtn = document.getElementById('saveToDeviceBtn');
function markDirty() {
  dirty = true;
  saveToDeviceBtn.classList.add('has-unsaved');
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

let latestFirmware = null; // false if firmware.json couldn't be loaded
const latestFirmwareReady = fetch('firmware/firmware.json', { cache: 'no-store' })
  .then((r) => (r.ok ? r.json() : Promise.reject(new Error('firmware.json'))))
  .then((j) => { latestFirmware = j; })
  .catch(() => { latestFirmware = false; });

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
    const res = await fetch(`firmware/${latestFirmware.bin}`, { cache: 'no-store' });
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
let hostPortMask = 0; // bit 0 = a device on USB Host 1
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

async function refreshSidecarStatus() {
  if (!device.connected || updateInProgress || !overlay.hidden) return;
  let status;
  try { status = await device.getStatus(); } catch (err) { return; }
  const sidecarChanged = status.sidecar !== sidecarPresent;
  const portsChanged = status.hostPortMask !== hostPortMask;
  sidecarPresent = status.sidecar;
  hostPortMask = status.hostPortMask;
  lastHostStatus = status;
  if (status.sidecar) {
    if (hostRestartCount !== null && status.restartCount > hostRestartCount) {
      // A reset after plugging a device in or out (reason 4) recovers on its own: a brief yellow note.
      // Every other reason stays up in red until clicked.
      if (status.restartReason === 4) {
        toast(restartMessage(status), 'warn');
      } else {
        const time = new Date().toLocaleTimeString();
        toast(`${time} ${restartMessage(status)} (click to dismiss)`, 'error', true);
      }
    }
    hostRestartCount = status.restartCount;
  }
  if (sidecarChanged) {
    renderRoutes();
    renderConnectionPill();
    toast(sidecarPresent ? 'Host Edition ports connected' : 'Host Edition ports disconnected', sidecarPresent ? 'success' : 'error');
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
    toast(`Last recovery: ${restartMessage(lastHostStatus)}. ${lastHostStatus.restartCount} since the Host Edition powered up. (click to dismiss)`, lastHostStatus.restartReason === 4 ? 'warn' : 'error', true);
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
  { value: 1, label: 'Pitch Up' },
  { value: 2, label: 'Pitch Down' },
  { value: 3, label: 'Mod' },
  { value: 4, label: 'CC Momentary' },
  { value: 5, label: 'CC Toggle' },
  { value: 6, label: 'Octave Up' },
  { value: 7, label: 'Octave Down' },
  { value: 8, label: 'Note Momentary' },
  { value: 9, label: 'Note Toggle' }
];

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

// Pitch/mod ramp speed (buttonParamA, 0-127): 127 is an instant jump to the target value, 0 is
// the slowest ramp. Curved (squared) rather than linear so slider steps near the fast/instant end
// cover a lot less time per step than steps near the slow end — more usable resolution right where
// it matters most. Must match RAMP_MAX_SECONDS/RAMP_GAMMA and the seconds formula in the
// firmware's serviceRamp() exactly, since this is what makes the typed/displayed seconds value real.
const RAMP_MAX_SECONDS = 4.0;
const RAMP_GAMMA = 2;
function rampSpeedToSeconds(speed) {
  const x = (127 - speed) / 127;
  return RAMP_MAX_SECONDS * Math.pow(x, RAMP_GAMMA);
}
function rampSecondsToSpeed(seconds) {
  const clamped = Math.max(0, Math.min(RAMP_MAX_SECONDS, seconds));
  const x = Math.pow(clamped / RAMP_MAX_SECONDS, 1 / RAMP_GAMMA);
  return Math.round(127 - x * 127);
}
// Below a second, tenths alone can't tell 0.02s from 0.08s apart — hundredths matter there.
// At/above a second, tenths are already fine enough and hundredths would just be visual noise.
function formatRampSeconds(speed) {
  const seconds = rampSpeedToSeconds(speed);
  return seconds < 1 ? seconds.toFixed(2) : seconds.toFixed(1);
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
  return `<div class="mini-box-main"><p class="mini-box-label">${label}</p>${controlHtml}</div>`;
}

function renderGlobalCard() {
  const g = state.global;
  const isUsbAdaptor = g.mode === 0;

  let buttonExtraFields = '';
  if ([1, 2, 3].includes(g.buttonAction)) {
    buttonExtraFields = `
      <div class="mini-box">
        <div class="mini-box-main">
          <div class="speed-label-row">
            <p class="mini-box-label">Speed</p>
            <input type="number" step="0.01" min="0" max="${RAMP_MAX_SECONDS}" value="${formatRampSeconds(g.buttonParamA)}" id="speedSecondsInput" class="speed-seconds-input" aria-label="Ramp time in seconds">
          </div>
          <input type="range" min="0" max="127" value="${g.buttonParamA}" data-global-field="buttonParamA" id="speedSlider" style="width:100%">
        </div>
      </div>
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
  }

  const brightnessPct = g.ledBrightnessStep * 10;
  const knobAngle = -150 + (g.ledBrightnessStep / 10) * 300;
  const allCollapsed = state.routes.length > 0 && state.routes.every((r) => r._collapsed);

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
          ${BUTTON_ACTIONS.map((a) => `<option value="${a.value}" ${g.buttonAction === a.value ? 'selected' : ''}>${a.label}</option>`).join('')}
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
  wireSpeedSecondsInput();
}

// Only present for Pitch Up/Down/Mod (see buttonExtraFields above) — the seconds box is the
// source of truth for typed input, the slider stays the source of truth for drag input, and each
// side keeps the other in sync via rampSpeedToSeconds/rampSecondsToSpeed so both always agree.
function wireSpeedSecondsInput() {
  const input = document.getElementById('speedSecondsInput');
  if (!input) return;
  const slider = document.getElementById('speedSlider');
  input.addEventListener('change', () => {
    const seconds = parseFloat(input.value);
    const speed = rampSecondsToSpeed(Number.isFinite(seconds) ? seconds : 0);
    state.global.buttonParamA = speed;
    slider.value = speed;
    input.value = formatRampSeconds(speed);
    syncGlobalLive();
  });
}

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
  if (field === 'buttonAction' || field === 'buttonToggleMomentary') renderGlobalCard();
  syncGlobalLive();
});
globalCard.addEventListener('input', (e) => {
  const field = e.target.dataset.globalField;
  if (!field || e.target.type !== 'range') return;
  state.global[field] = parseInt(e.target.value, 10);
  if (field === 'buttonParamA') {
    const secondsInput = document.getElementById('speedSecondsInput');
    if (secondsInput) secondsInput.value = formatRampSeconds(state.global.buttonParamA);
  }
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

// Port switches. Each switch and its label sit inside one chip, so it's always clear which label
// goes with which switch. Sidecar ports (USB Host 1-4, TRS 2-6) only show while a sidecar is
// detected, or when the route already uses them.
function visiblePorts(route, field) {
  return PORTS.filter((p) => (field === 'outPorts' || p.input) && (!p.sidecar || sidecarPresent || (route[field] & (1 << p.bit))));
}

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
    const live = p.key.startsWith('host') && sidecarPresent && (hostPortMask & (1 << (p.bit - 2)));
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
  for (let i = 0; i < 4; i++) {
    const live = sidecarPresent && !!(hostPortMask & (1 << i));
    routesContainer.querySelectorAll(`.port-chip[data-port-bit="${i + 2}"]`).forEach((el) => {
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
              <p class="note-hint">CC range now begins with this CC.</p>
            </div>
            <div class="field-row">
              ${switchHtml(index, 'monoRetrig', !!route.monoRetrig, 'Mono Retrig')}
              <p class="note-hint">Poly to Mono. Helps samplers play more like analog CV synths. Lifting a key recalls the next still-held key.</p>
            </div>
            <div class="field-row">
              <div class="round-robin-row">
                ${switchHtml(index, 'roundRobin', !!route.roundRobin, 'Round Robin')}
                ${route.roundRobin ? switchHtml(index, 'roundRobinRandom', !!route.roundRobinRandom, 'Rand', 'cc-map-row-indent') : ''}
              </div>
              <p class="note-hint">Mono to Poly. ${route.roundRobin && route.roundRobinRandom ? 'Picks a random selected channel for each new note.' : 'Cycles each new note to a different selected channel.'}</p>
            </div>
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

function renderRoutes() {
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
const STRUCTURAL_FIELDS = new Set(['note', 'cc', 'ccMapEnabled', 'aftertouch', 'aftertouchMap', 'pressure', 'pressureMap', 'allInputTypes', 'allInputChannels', 'allInPorts', 'allOutPorts', 'roundRobin', 'roundRobinRandom']);

routesContainer.addEventListener('click', (e) => {
  const actionEl = e.target.closest('[data-action]');
  if (!actionEl) return;
  const index = parseInt(actionEl.dataset.routeIndex, 10);
  const action = actionEl.dataset.action;
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
  if (STRUCTURAL_FIELDS.has(field)) refreshRouteCard(parseInt(index, 10));
  if (field === 'outputChannels') updateChannelWarning(parseInt(index, 10));
  syncMasterSwitches(parseInt(index, 10));
  syncRouteLive(parseInt(index, 10));
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
  downloadPreset({ name: state.presetName, global: state.global, routes: state.routes });
  toast('Preset downloaded', 'success');
});

document.getElementById('uploadPresetInput').addEventListener('change', async (e) => {
  const file = e.target.files[0];
  e.target.value = '';
  if (!file) return;
  try {
    const preset = await readPresetFile(file);
    state.presetName = preset.name;
    state.global = preset.global;
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
    const { global, routes } = await device.loadAll((done, total) => setOverlayProgress(done, total));
    state.global = global;
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
    await device.saveAll(state.global, state.routes, (done, total) => setOverlayProgress(done, total));
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
