/*
  Mini_MIDI_Multitool.ino by woz.lol
  Foxwizard Mini MIDI Multitool — RP2040 USB-C MIDI <-> DIN/TRS MIDI router

  Note: Activity Lights fade in over 1 sec and fade out over 1/2 sec so that the blinking is not
  distracting while playing. LED brightness is scaled by an 11-step knob (see ledBrightnessStep,
  0..10, where 0 is fully off)
  on top of the "balanced max" per-LED ceilings below, which keep the red/green/yellow LEDs looking
  even to the eye at full brightness rather than all blasting 255.

  Purpose:
  - USB-C MIDI device <-> DIN/TRS MIDI adaptor, factory default ("USB Adaptor Mode")
  - Full custom router ("Router Mode" + up to 256 user-defined routes), configured entirely over
    a SysEx protocol from the FOXWIZARD PATCHLING web app (Web MIDI, Chrome). See
    ../SYSEX_PROTOCOL.md for the wire format this file implements — keep both in sync.
  - Activity Lights for DIN/TRS OUT, USB, and DIN/TRS IN
  - Front button, configurable action (panic/pitch/mod/cc/octave) via SysEx

  Wiring:
  - GP1 / board pin 1 = DIN MIDI IN to this RP2040 RX
  - GP0 / board pin 0 = DIN MIDI OUT from this RP2040 TX
  - GP2 = OUT activity light
  - GP3 = USB activity light
  - GP4 = IN activity light
  - GP5 = front button, active LOW with INPUT_PULLUP

  Arduino setup:
  - Board: Waveshare RP2040-Zero
  - Tools -> USB Stack = Adafruit TinyUSB

  Factory reset: hold the button while power is applied / USB is plugged in (checked once at boot,
  same as the original simple adaptor sketch). This also works as a software command over SysEx.

  Note on file layout: every struct/enum used as a function parameter type is declared up front,
  before any function definitions. The Arduino IDE auto-generates forward prototypes for every
  function and inserts them right after the #include block — if a custom struct were defined
  further down the file, that auto-generated prototype would reference a type the compiler hasn't
  seen yet and fail to build. Keeping all types at the top sidesteps that.
*/

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <EEPROM.h>

// ============================================================================================
// Constants
// ============================================================================================
constexpr uint8_t LED_OUT_MAX = 128; // Red — balanced max, NOT literal 255 (keeps colors even)
constexpr uint8_t LED_USB_MAX = 96;  // Green
constexpr uint8_t LED_IN_MAX = 223;  // Yellow

constexpr uint8_t PIN_DIN_MIDI_TX = 0;
constexpr uint8_t PIN_DIN_MIDI_RX = 1;
constexpr uint8_t PIN_LED_OUT = 2;
constexpr uint8_t PIN_LED_USB = 3;
constexpr uint8_t PIN_LED_IN = 4;
constexpr uint8_t PIN_BUTTON = 5;
constexpr uint32_t DIN_BAUD = 31250;
constexpr uint32_t LED_FADE_IN_MS = 1000;
constexpr uint32_t LED_FADE_OUT_MS = 500;
constexpr uint32_t ACTIVITY_HOLD_MS = 12;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 25;
constexpr uint32_t STARTUP_BEAT_MS = 125;
constexpr uint32_t STARTUP_FADE_MS = 250;
constexpr uint32_t PANIC_FLASH_MS = 35;
constexpr uint32_t BRIGHTNESS_PREVIEW_HOLD_MS = 400;

constexpr uint8_t SYSEX_MFR_ID = 0x7D;   // reserved for non-commercial/educational use
constexpr uint8_t SYSEX_DEVICE_ID = 0x4D; // 'M'
constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr uint8_t FW_VERSION_MAJOR = 1;
constexpr uint8_t FW_VERSION_MINOR = 0;
const char DEVICE_NAME[] = "Foxwizard Mini MIDI Multitool"; // long name sent in the SysEx IDENTITY_REPLY
const char USB_DESCRIPTOR_NAME[] = "FoxWizard"; // short name shown by the OS/DAW in the USB MIDI port list

constexpr uint16_t MAX_ROUTES = 256;
constexpr uint8_t ROUTE_NAME_MAX_LEN = 24; // ASCII characters, enforced web-side too
constexpr uint8_t ROUTE_RAW_BYTES = 17 + ROUTE_NAME_MAX_LEN;
constexpr uint8_t MAX_PENDING = 64;

// ============================================================================================
// Types
// ============================================================================================
enum SysexCmd : uint8_t {
  CMD_IDENTITY_REQUEST = 0x01,
  CMD_IDENTITY_REPLY = 0x02,
  CMD_GET_GLOBAL = 0x10,
  CMD_GLOBAL_DATA = 0x11,
  CMD_SET_GLOBAL = 0x12,
  CMD_GET_ROUTE_COUNT = 0x20,
  CMD_ROUTE_COUNT = 0x21,
  CMD_GET_ROUTE = 0x22,
  CMD_ROUTE_DATA = 0x23,
  CMD_ADD_ROUTE = 0x24,
  CMD_SET_ROUTE = 0x25,
  CMD_DELETE_ROUTE = 0x26,
  CMD_MOVE_ROUTE = 0x27,
  CMD_CLEAR_ALL_ROUTES = 0x28,
  CMD_PREVIEW_BRIGHTNESS = 0x30,
  CMD_COMMIT = 0x31,
  CMD_FACTORY_RESET = 0x32,
  CMD_ACK = 0x7E
};

enum AckStatus : uint8_t {
  ACK_OK = 0,
  ACK_ERR_BAD_INDEX = 1,
  ACK_ERR_BAD_DATA = 2,
  ACK_ERR_FULL = 3
};

enum : uint8_t { DEV_TRS = 0, DEV_USB = 1, DEV_BOTH = 2 };
enum : uint8_t { OUT_NONE = 0, OUT_TRS = 1, OUT_USB = 2, OUT_BOTH = 3 };
enum : uint8_t { MODE_USB_ADAPTOR = 0, MODE_ROUTER = 1 };
enum : uint8_t {
  BTN_PANIC = 0,
  BTN_PITCH_UP = 1,
  BTN_PITCH_DOWN = 2,
  BTN_MOD = 3,
  BTN_CC_MOMENTARY = 4,
  BTN_CC_TOGGLE = 5,
  BTN_OCTAVE_UP = 6,
  BTN_OCTAVE_DOWN = 7,
  BTN_NOTE_MOMENTARY = 8,
  BTN_NOTE_TOGGLE = 9
};
enum MsgKind : uint8_t {
  MK_NOTE = 0,
  MK_CC = 1,
  MK_PC = 2,
  MK_PITCH = 3,
  MK_POLY_AT = 4,
  MK_CHAN_PRESSURE = 5,
  MK_SYSEX = 6,
  MK_COMMON = 7,    // System Common: MTC Quarter Frame 0xF1, Song Position 0xF2, Song Select 0xF3, Tune Request 0xF6
  MK_CLOCK = 8,     // System Realtime: Clock 0xF8 only — split from the rest since it's continuous chatter (24/quarter note during playback) rather than a rare one-off event
  MK_REALTIME = 9   // System Realtime, everything else: Start 0xFA, Continue 0xFB, Stop 0xFC, Active Sensing 0xFE, Reset 0xFF
};

#pragma pack(push, 1)
struct RouteData {
  uint8_t flags;      // bit7 enabled, bits1-0 inputDevice
  uint8_t inChLow;
  uint8_t inChHigh;
  uint8_t typeFlags;  // bit0 note,1 cc,2 pc,3 pitch,4 polyAT,5 chanPressure,6 sysex,7 polyAT mapped to CC
  uint8_t noteStart;
  uint8_t noteEnd;
  uint8_t ccStart;
  uint8_t ccEnd;
  uint8_t outDevice;
  uint8_t outChLow;
  uint8_t outChHigh;
  uint8_t transpose;  // semitone + 64
  uint8_t velocityScale; // percent, 10-200, applied to outgoing Note On velocity (100 = unchanged)
  uint8_t ccMapStart;
  uint8_t atMapCC;    // destination CC number when typeFlags bit7 is set (poly aftertouch remapped to CC)
  uint8_t cpMapCC;    // bit7=channel pressure remapped to CC, bits6-0=destination CC number
  uint8_t flags2;     // bit0=Mono Retrig, bit1=Round Robin, bit2=System Common, bit3=System Realtime (Transport, everything but Clock), bit4=MIDI Clock, bit5=Round Robin Random (true random channel pick instead of cycling; only meaningful with bit1 set)
  char name[ROUTE_NAME_MAX_LEN]; // ASCII, zero-padded, not necessarily NUL-terminated if it fills the buffer
};

struct GlobalSettings {
  uint8_t mode;
  uint8_t buttonAction;
  uint8_t buttonParamA;
  uint8_t buttonParamB;
  uint8_t buttonToggleMomentary;
  uint8_t ledBrightnessStep;
};
#pragma pack(pop)

static_assert(sizeof(RouteData) == ROUTE_RAW_BYTES, "RouteData must stay wire-compatible");
static_assert(sizeof(GlobalSettings) == 6, "GlobalSettings must stay wire-compatible");

enum LedPhase { LED_OFF, LED_RISING, LED_ON, LED_FALLING };

struct ActivityLed {
  uint8_t pin;
  uint8_t baseMax;
  uint8_t maxLevel;
  LedPhase phase = LED_OFF;
  uint32_t phaseStartMs = 0;
  uint32_t lastActivityMs = 0;
  uint8_t currentLevel = 0;
  uint8_t fadeStartLevel = 0;
  bool activitySeen = false;
};

struct PendingMsg {
  bool used;
  uint8_t channel;
  uint8_t kind;
  uint8_t number;
  uint8_t statusHi;
  uint8_t data1;
  uint8_t data2;
  uint8_t len; // bytes to actually transmit (1-3)
};

struct SysexState {
  enum Phase { IDLE, PEEK1, PEEK2, OURS, FORWARDING } phase = IDLE;
  uint8_t cmdBuf[64]; // cmd(1) + index(2) + pack7(ROUTE_RAW_BYTES=41) which packs to 47 bytes = 50 needed
  uint8_t cmdLen = 0;
  uint8_t fwdDevice = OUT_NONE;
};

// Small per-source accumulators so a USB-bound sysex forward can be chunked into 3-byte USB-MIDI
// packets without the DIN-origin and USB-origin (echo, Router Mode) streams corrupting each other.
struct UsbOutSysexAccum {
  uint8_t buf[3];
  uint8_t len = 0;
};

struct DinParser {
  uint8_t runningStatus = 0;
  uint8_t data[2] = {0, 0};
  uint8_t needed = 0;
  uint8_t have = 0;
};

// ============================================================================================
// Globals
// ============================================================================================
Adafruit_USBD_MIDI usb_midi;

RouteData routes[MAX_ROUTES];
uint16_t routeCount = 0;
GlobalSettings globalSettings;

ActivityLed ledOut = {PIN_LED_OUT, LED_OUT_MAX, LED_OUT_MAX};
ActivityLed ledUsb = {PIN_LED_USB, LED_USB_MAX, LED_USB_MAX};
ActivityLed ledIn = {PIN_LED_IN, LED_IN_MAX, LED_IN_MAX};

PendingMsg pendingTrs[MAX_PENDING];
PendingMsg pendingUsb[MAX_PENDING];
uint8_t pendingTrsCount = 0;
uint8_t pendingUsbCount = 0;

SysexState dinSysex;
SysexState usbSysex;
UsbOutSysexAccum usbOutFromDin;
UsbOutSysexAccum usbOutFromUsb;

DinParser dinParser;

uint8_t replyBuf[64];
uint8_t replyLen;

bool lastButtonReading = HIGH;
bool stableButtonState = HIGH;
uint32_t lastButtonChangeMs = 0;

bool buttonHeld = false;
bool ccToggleState = false;
bool noteToggleState = false;
bool octaveToggleState = false;
int8_t currentOctaveShift = 0; // 0 or +12/-12 semitones, applied to routed+default note output

bool rampActive = false;
bool rampRising = false;
int16_t rampValue = 0;
int16_t rampStartValue = 0;
uint32_t rampStartMs = 0;
uint32_t rampDurationMs = 0;

// buttonParamA (0-127) is the pitch/mod ramp's speed: 127 is a true instant jump to the target
// value, 0 is the slowest ramp, taking RAMP_MAX_SECONDS to sweep the full range. Squared (not
// linear) so raw values near the fast/instant end cover a lot less time each than values near the
// slow end. The web UI shows and accepts this as a plain seconds value using the exact same
// formula, so keep both in sync.
static const float RAMP_MAX_SECONDS = 4.0f;

// Per (channel, original note): while Octave Up/Down is the active button action and this channel
// is affected (see octaveShiftMatchesChannel), tracks what a currently-held note is ACTUALLY
// sounding as right now — which port it arrived on, its shifted output note, and its velocity —
// so that (a) the eventual real Note Off releases the exact pitch that's sounding even if the
// shift changed since the press, and (b) a shift change can kill + immediately retrigger every
// currently-held note at the new pitch instead of just cutting it off and leaving it silent until
// the key is released and pressed again. 0xFF in octaveHeldShiftedNote means "not currently held".
uint8_t octaveHeldShiftedNote[16][128];
uint8_t octaveHeldVelocity[16][128];
uint8_t octaveHeldInputDevice[16][128];

// Poly aftertouch -> CC mapping needs to know which currently-held note should "own" the mapped CC
// when more than one note is held, otherwise independently-varying finger pressure on different
// notes would fight over one destination value. Small per-channel note-priority stack: most recent
// Note On goes on top, Note Off removes wherever it is, and the mapped CC always follows whatever's
// on top now — falling back to the next-held note's own last-reported pressure, or to 0 once
// nothing is held. Depth is 128, not a "should be enough" guess — a MIDI channel only has 128
// possible distinct note numbers (0-127), so this is the actual hard ceiling on how many notes
// could ever be simultaneously held, and the overflow-guard in noteStackOn() below can never
// actually trigger. (~8KB of RAM for all 32 channel/device stacks combined, cheap relative to the
// 256KB available.)
constexpr uint8_t NOTE_STACK_DEPTH = 128;
struct NoteHoldStack {
  uint8_t notes[NOTE_STACK_DEPTH];
  uint8_t pressures[NOTE_STACK_DEPTH];
  uint8_t count = 0;
};
NoteHoldStack noteStacks[2][16]; // [DEV_TRS/DEV_USB][channel]

// Mono Retrig / Round Robin — per-route note tracking.
// One shared small table per route serves both features, keyed by the input note number:
//   - Mono Retrig (flags2 bit0): poly->mono conversion. Every held note is tracked with its own
//     velocity and a "how recently was this pressed" order counter; only the most-recently-pressed
//     held note is ever actually sounding, and releasing it recalls whichever remaining held note
//     has the highest order value (falls back through the "next oldest" held note, goes silent once
//     none are left) — same idea as the poly-aftertouch note-priority stack above, applied to notes
//     themselves instead of a mapped CC value.
//   - Round Robin (flags2 bit1): each new Note On cycles to the next of the route's selected output
//     channels rather than fanning out to all of them; the channel actually used is remembered per
//     note so that note's own Note Off is sent back to that same channel, not wherever the cursor
//     has since rotated to.
// Depth 16 (not one slot per possible note number) is a deliberate size/cost tradeoff — both
// features are fundamentally about melodic, mostly-monophonic playing, not dense chords, so this is
// generous headroom rather than a tight cap. If it's ever exceeded, the overflow note simply isn't
// tracked (never silently stuck: it never got an on sent for tracking purposes, so it never needs an
// off either) — same graceful-degradation approach as the poly aftertouch note stack.
constexpr uint8_t ROUTE_NOTE_TRACK_DEPTH = 16;
struct RouteNoteSlot {
  uint8_t note = 0xFF;  // input note number this slot is tracking; 0xFF = empty/free
  uint8_t velocity = 0; // last velocity this note was pressed with (Mono Retrig recall)
  uint16_t order = 0;   // Mono Retrig priority — higher means more recently pressed
  uint8_t channel = 0xFF; // 0-15, output channel this note's Note On actually went to (Round Robin); 0xFF = none yet
};
struct RouteNoteTrack {
  RouteNoteSlot slots[ROUTE_NOTE_TRACK_DEPTH];
  uint16_t orderCounter = 0;
  uint8_t rrCursor = 0;        // Round Robin rotating cursor, this route's own
  bool monoRetrigActive = false;   // is a note currently sounding through Mono Retrig for this route
  uint8_t monoRetrigActiveNote = 0xFF;
};
RouteNoteTrack routeNoteTracks[MAX_ROUTES];

// This whole note-hold-stack machinery only matters to routes with poly aftertouch mapped to CC —
// a rare, minor feature. Rather than paying its (small but nonzero) per-Note-On/Off/aftertouch-
// message bookkeeping cost on every single MIDI event regardless of whether anything actually uses
// it, this cached flag gates that work entirely. It's only recomputed when the route list actually
// changes (add/set/delete/clear/load-from-flash) — never per MIDI event, since a per-event scan of
// every route would cost more than the bookkeeping it's meant to save.
bool anyAftertouchMapRoutes = false;
void recomputeAnyAftertouchMapRoutes() {
  anyAftertouchMapRoutes = false;
  for (uint16_t i = 0; i < routeCount; ++i) {
    if (routes[i].typeFlags & 0x80) { anyAftertouchMapRoutes = true; return; }
  }
}

// routeNoteTracks is indexed by route position, but ADD/DELETE/MOVE_ROUTE shift routes around in
// the array — without this, Mono Retrig/Round Robin state left over at an index could end up
// misattributed to a completely different route after a reorder. Route mutations only ever happen
// in an infrequent bulk Save-to-Device/Load-from-Device operation (never live per-keystroke editing,
// see the local-only architecture elsewhere in this file), and that operation already rebuilds the
// whole route table from scratch, so resetting every route's tracking state on any structural change
// is both correct and cheap relative to how rarely it actually runs.
void resetAllRouteNoteTracks() {
  for (uint16_t i = 0; i < MAX_ROUTES; ++i) routeNoteTracks[i] = RouteNoteTrack();
}

RouteNoteSlot *routeNoteFindSlot(RouteNoteTrack &t, uint8_t note) {
  for (uint8_t i = 0; i < ROUTE_NOTE_TRACK_DEPTH; ++i) if (t.slots[i].note == note) return &t.slots[i];
  return nullptr;
}
RouteNoteSlot *routeNoteFindOrAllocSlot(RouteNoteTrack &t, uint8_t note) {
  RouteNoteSlot *existing = routeNoteFindSlot(t, note);
  if (existing) return existing;
  for (uint8_t i = 0; i < ROUTE_NOTE_TRACK_DEPTH; ++i) {
    if (t.slots[i].note == 0xFF) { t.slots[i].note = note; return &t.slots[i]; }
  }
  return nullptr; // table full for this route — note goes untracked, see comment above
}
void routeNoteFreeSlot(RouteNoteTrack &t, uint8_t note) {
  RouteNoteSlot *s = routeNoteFindSlot(t, note);
  if (s) *s = RouteNoteSlot();
}

// Advances the route's Round Robin cursor and returns the next 0-15 output channel with its bit set
// in outMask, wrapping around. Returns 0xFF if outMask is empty (nothing selected).
uint8_t nextRoundRobinChannel(uint8_t &cursor, uint16_t outMask) {
  if (outMask == 0) return 0xFF;
  for (uint8_t attempts = 0; attempts < 16; ++attempts) {
    uint8_t idx = cursor++ & 0x0F;
    if (outMask & (1u << idx)) return idx;
  }
  return 0xFF;
}

// Round Robin's "Random" mode: true uniform pick (rp2040's actual hardware RNG, not a seeded
// random() — no anti-repeat logic, the same channel can legitimately come up twice in a row).
// Returns 0xFF if outMask is empty.
uint8_t randomChannelInMask(uint16_t outMask) {
  if (outMask == 0) return 0xFF;
  uint8_t candidates[16];
  uint8_t count = 0;
  for (uint8_t c = 0; c < 16; ++c) {
    if (outMask & (1u << c)) candidates[count++] = c;
  }
  return candidates[rp2040.hwrand32() % count];
}

// The single place that actually transmits a Note On/Off for a route (transpose applied), used by
// both plain passthrough and Mono Retrig's recall logic. Round Robin (flags2 bit1) is handled
// here so it applies uniformly regardless of which caller is sending the note. Deliberately never
// frees a route's note-tracking slot itself — this function has no way to know whether an "off" is
// a real key release or Mono Retrig silencing a superseded-but-still-held note (which needs its
// slot's velocity/order to survive for a possible later recall) — freeing is the caller's job, once
// the caller knows which case it actually is. See call sites for where that happens.
void sendRouteNote(uint16_t routeIndex, const RouteData &r, bool on, uint8_t note, uint8_t velocity) {
  if (r.outDevice == OUT_NONE) return;
  int16_t transpose = (int16_t)r.transpose - 64;
  int16_t outNote = (int16_t)note + transpose;
  if (outNote < 0 || outNote > 127) return; // out-of-range after transpose: does not pass

  uint16_t outMask = r.outChLow | ((uint16_t)r.outChHigh << 8);
  uint8_t data2;
  if (on) {
    // velocityScale is a percent (10-200, 100 = unchanged) applied before the existing "never send
    // velocity 0 as a Note On" floor — scaling down a soft hit could otherwise round to 0, which
    // would be read as a Note Off by running-status-aware receivers.
    uint16_t scaled = ((uint16_t)velocity * r.velocityScale) / 100;
    if (scaled > 127) scaled = 127;
    data2 = max<uint8_t>(1, (uint8_t)scaled);
  } else {
    data2 = 0;
  }

  if (!(r.flags2 & 0x02)) {
    // Normal fan-out to every selected output channel (existing behavior).
    for (uint8_t c = 0; c < 16; ++c) {
      if (!(outMask & (1u << c))) continue;
      PendingMsg msg;
      msg.used = true;
      msg.channel = c;
      msg.kind = MK_NOTE;
      msg.statusHi = on ? 0x90 : 0x80;
      msg.number = (uint8_t)outNote;
      msg.data1 = (uint8_t)outNote;
      msg.data2 = data2;
      msg.len = 3;
      if (r.outDevice == OUT_TRS || r.outDevice == OUT_BOTH) upsertPending(pendingTrs, pendingTrsCount, msg);
      if (r.outDevice == OUT_USB || r.outDevice == OUT_BOTH) upsertPending(pendingUsb, pendingUsbCount, msg);
    }
    return;
  }

  // Round Robin: exactly one channel, remembered per note so its own Note Off lands on the same
  // channel its Note On went to, regardless of where the cursor has rotated to (or what the random
  // pick lands on next) since. flags2 bit5 ("Random") swaps the cycling cursor for a true uniform
  // pick — only meaningful while Round Robin itself is on, same as the web UI only showing it then.
  RouteNoteTrack &t = routeNoteTracks[routeIndex];
  RouteNoteSlot *slot = on ? routeNoteFindOrAllocSlot(t, note) : routeNoteFindSlot(t, note);
  bool randomMode = r.flags2 & 0x20;
  uint8_t channel;
  if (on) {
    channel = randomMode ? randomChannelInMask(outMask) : nextRoundRobinChannel(t.rrCursor, outMask);
    if (slot) slot->channel = channel;
  } else if (slot && slot->channel != 0xFF) {
    channel = slot->channel;
  } else {
    channel = randomMode ? randomChannelInMask(outMask) : nextRoundRobinChannel(t.rrCursor, outMask);
  }
  if (channel == 0xFF) return; // nothing selected

  PendingMsg msg;
  msg.used = true;
  msg.channel = channel;
  msg.kind = MK_NOTE;
  msg.statusHi = on ? 0x90 : 0x80;
  msg.number = (uint8_t)outNote;
  msg.data1 = (uint8_t)outNote;
  msg.data2 = data2;
  msg.len = 3;
  if (r.outDevice == OUT_TRS || r.outDevice == OUT_BOTH) upsertPending(pendingTrs, pendingTrsCount, msg);
  if (r.outDevice == OUT_USB || r.outDevice == OUT_BOTH) upsertPending(pendingUsb, pendingUsbCount, msg);
}

// Mono Retrig (flags2 bit0): converts this route's note stream to mono with last-note priority.
// Mirrors the proven design from the ARPnMIDI project — track every held note's own velocity and
// press order, only the newest is ever actually sounding, and releasing it sends that note's own
// Note Off followed by a fresh Note On "recalling" whichever held note is now newest (or nothing, if
// none remain). Off-then-on (not overlapped) matches ARPnMIDI's own working implementation.
void handleMonoRetrigNote(uint16_t routeIndex, const RouteData &r, uint8_t note, uint8_t velocity, bool on) {
  RouteNoteTrack &t = routeNoteTracks[routeIndex];

  if (on) {
    RouteNoteSlot *slot = routeNoteFindOrAllocSlot(t, note);
    if (slot) { slot->velocity = velocity; slot->order = ++t.orderCounter; }

    if (t.monoRetrigActive && t.monoRetrigActiveNote == note) {
      // Re-trigger of the already-sounding note (no other note intervened) — nothing to change.
      return;
    }
    if (t.monoRetrigActive) sendRouteNote(routeIndex, r, false, t.monoRetrigActiveNote, 0);
    sendRouteNote(routeIndex, r, true, note, velocity);
    t.monoRetrigActive = true;
    t.monoRetrigActiveNote = note;
    return;
  }

  // Note Off. Freeing this note's slot is deferred until after sendRouteNote(false, ...) below —
  // if Round Robin is also on for this route, that call needs the slot's remembered `channel` still
  // present to target this note's own Note Off correctly (see sendRouteNote's Round Robin branch).
  if (!t.monoRetrigActive || t.monoRetrigActiveNote != note) {
    routeNoteFreeSlot(t, note); // background note (already silent on the wire) — sounding note unaffected
    return;
  }

  uint8_t winnerNote = 0xFF, winnerVelocity = 0;
  uint16_t newestOrder = 0;
  for (uint8_t i = 0; i < ROUTE_NOTE_TRACK_DEPTH; ++i) {
    RouteNoteSlot &s = t.slots[i];
    if (s.note == 0xFF || s.note == note) continue; // skip empty slots and the note being released itself
    if (winnerNote == 0xFF || s.order >= newestOrder) { winnerNote = s.note; winnerVelocity = s.velocity; newestOrder = s.order; }
  }

  sendRouteNote(routeIndex, r, false, note, 0);
  routeNoteFreeSlot(t, note); // now safe — Round Robin's channel lookup above already happened
  if (winnerNote == 0xFF) {
    t.monoRetrigActive = false;
    t.monoRetrigActiveNote = 0xFF;
  } else {
    sendRouteNote(routeIndex, r, true, winnerNote, winnerVelocity);
    t.monoRetrigActiveNote = winnerNote;
  }
}

// ============================================================================================
// Defaults / EEPROM persistence
// ============================================================================================
void setFactoryDefaults() {
  globalSettings.mode = MODE_USB_ADAPTOR;
  globalSettings.buttonAction = BTN_PANIC;
  globalSettings.buttonParamA = 0;
  globalSettings.buttonParamB = 0;
  globalSettings.buttonToggleMomentary = 0;
  globalSettings.ledBrightnessStep = 10; // 100% (scale is 0..10, step 0 = fully off)
  routeCount = 0;
  anyAftertouchMapRoutes = false; // no routes left, so trivially true without needing a scan
  resetAllRouteNoteTracks();
}

constexpr uint32_t EEPROM_MAGIC = 0x464F5831; // "FOX1"
constexpr size_t EEPROM_SIZE = 4 + 6 + 2 + (MAX_ROUTES * ROUTE_RAW_BYTES); // magic+global+count+routes

void loadFromFlash() {
  EEPROM.begin(EEPROM_SIZE);
  uint32_t magic = 0;
  EEPROM.get(0, magic);
  if (magic != EEPROM_MAGIC) {
    setFactoryDefaults();
    return;
  }
  size_t addr = 4;
  EEPROM.get(addr, globalSettings);
  addr += sizeof(GlobalSettings);
  uint16_t storedCount = 0;
  EEPROM.get(addr, storedCount);
  addr += sizeof(storedCount);
  routeCount = (storedCount > MAX_ROUTES) ? MAX_ROUTES : storedCount;
  for (uint16_t i = 0; i < routeCount; ++i) {
    EEPROM.get(addr, routes[i]);
    addr += ROUTE_RAW_BYTES;
  }
  recomputeAnyAftertouchMapRoutes();
  resetAllRouteNoteTracks();
}

void saveToFlash() {
  size_t addr = 0;
  EEPROM.put(addr, EEPROM_MAGIC);
  addr += 4;
  EEPROM.put(addr, globalSettings);
  addr += sizeof(GlobalSettings);
  EEPROM.put(addr, routeCount);
  addr += sizeof(routeCount);
  for (uint16_t i = 0; i < routeCount; ++i) {
    EEPROM.put(addr, routes[i]);
    addr += ROUTE_RAW_BYTES;
  }
  EEPROM.commit();
}

// ============================================================================================
// Activity Lights (unchanged fade engine from the original adaptor sketch, plus a brightness knob)
// ============================================================================================
// step 0..10 -> 0%..100%. Step 0 is fully off (no activity glow at all), not just dim.
uint8_t scaledMax(uint8_t baseMax, uint8_t step) {
  if (step == 0) return 0;
  uint16_t pct = (uint16_t)step * 10;
  uint16_t v = ((uint16_t)baseMax * pct + 50) / 100;
  if (v > baseMax) v = baseMax;
  return (uint8_t)v;
}

void applyBrightnessToLeds() {
  ledOut.maxLevel = scaledMax(ledOut.baseMax, globalSettings.ledBrightnessStep);
  ledUsb.maxLevel = scaledMax(ledUsb.baseMax, globalSettings.ledBrightnessStep);
  ledIn.maxLevel = scaledMax(ledIn.baseMax, globalSettings.ledBrightnessStep);
}

uint8_t scaleByte(uint32_t elapsed, uint32_t duration) {
  if (elapsed >= duration) return 255;
  return static_cast<uint8_t>((elapsed * 255UL) / duration);
}

void writeActivityLed(ActivityLed &led, uint8_t value) {
  led.currentLevel = (value > led.maxLevel) ? led.maxLevel : value;
  analogWrite(led.pin, led.currentLevel);
}

void serviceActivityLed(ActivityLed &led) {
  const uint32_t now = millis();

  if (led.activitySeen) {
    led.activitySeen = false;
    led.lastActivityMs = now;
    if (led.phase != LED_RISING && led.phase != LED_ON) {
      led.phase = LED_RISING;
      led.phaseStartMs = now;
      led.fadeStartLevel = led.currentLevel;
    }
  }

  switch (led.phase) {
    case LED_OFF:
      writeActivityLed(led, 0);
      break;

    case LED_RISING: {
      const uint32_t elapsed = now - led.phaseStartMs;
      const uint8_t progress = scaleByte(elapsed, LED_FADE_IN_MS);
      const uint8_t level = led.fadeStartLevel + (((led.maxLevel - led.fadeStartLevel) * progress) / 255);
      writeActivityLed(led, level);
      if (elapsed >= LED_FADE_IN_MS) {
        led.phase = LED_ON;
        writeActivityLed(led, led.maxLevel);
      }
      break;
    }

    case LED_ON:
      writeActivityLed(led, led.maxLevel);
      if (now - led.lastActivityMs > ACTIVITY_HOLD_MS) {
        led.phase = LED_FALLING;
        led.phaseStartMs = now;
        led.fadeStartLevel = led.currentLevel;
      }
      break;

    case LED_FALLING: {
      if (now - led.lastActivityMs <= ACTIVITY_HOLD_MS) {
        led.phase = LED_RISING;
        led.phaseStartMs = now;
        led.fadeStartLevel = led.currentLevel;
        break;
      }

      const uint32_t elapsed = now - led.phaseStartMs;
      if (elapsed >= LED_FADE_OUT_MS) {
        led.phase = LED_OFF;
        writeActivityLed(led, 0);
      } else {
        const uint8_t progress = scaleByte(elapsed, LED_FADE_OUT_MS);
        const uint8_t level = led.fadeStartLevel - ((led.fadeStartLevel * progress) / 255);
        writeActivityLed(led, level);
      }
      break;
    }
  }
}

void noteActivity(ActivityLed &led) {
  led.activitySeen = true;
}

void serviceLeds() {
  serviceActivityLed(ledOut);
  serviceActivityLed(ledUsb);
  serviceActivityLed(ledIn);
}

void setAllLedLevels(uint8_t outLevel, uint8_t usbLevel, uint8_t inLevel) {
  analogWrite(PIN_LED_OUT, outLevel);
  analogWrite(PIN_LED_USB, usbLevel);
  analogWrite(PIN_LED_IN, inLevel);
}

void fadeAllLeds(uint8_t fromLevel, uint8_t toLevel, uint32_t durationMs) {
  const uint32_t startMs = millis();
  while (true) {
#ifdef TINYUSB_NEED_POLLING_TASK
    TinyUSBDevice.task();
#endif
    const uint32_t elapsed = millis() - startMs;
    if (elapsed >= durationMs) break;
    const int16_t delta = static_cast<int16_t>(toLevel) - static_cast<int16_t>(fromLevel);
    const uint8_t level = static_cast<uint8_t>(fromLevel + ((delta * static_cast<int32_t>(elapsed)) / static_cast<int32_t>(durationMs)));
    setAllLedLevels(level, level, level);
    delay(2);
  }
  setAllLedLevels(toLevel, toLevel, toLevel);
}

void allLedConfirmationFlashes() {
  for (uint8_t i = 0; i < 3; ++i) {
    setAllLedLevels(255, 255, 255);
    delay(PANIC_FLASH_MS);
    setAllLedLevels(0, 0, 0);
    delay(PANIC_FLASH_MS);
  }
  ledOut.currentLevel = ledUsb.currentLevel = ledIn.currentLevel = 0;
  ledOut.phase = ledUsb.phase = ledIn.phase = LED_OFF;
}

void startupSequence() {
  setAllLedLevels(ledOut.maxLevel, 0, 0);
  delay(STARTUP_BEAT_MS);
  setAllLedLevels(0, ledUsb.maxLevel, 0);
  delay(STARTUP_BEAT_MS);
  setAllLedLevels(0, 0, ledIn.maxLevel);
  delay(STARTUP_BEAT_MS);
  setAllLedLevels(0, ledUsb.maxLevel, 0);
  delay(STARTUP_BEAT_MS);
  setAllLedLevels(ledOut.maxLevel, 0, 0);
  delay(STARTUP_BEAT_MS);
  setAllLedLevels(0, 0, 0);
  delay(STARTUP_BEAT_MS);
  fadeAllLeds(0, 255, STARTUP_FADE_MS);
  fadeAllLeds(255, 0, STARTUP_FADE_MS);
}

// Brightness "knob" preview: flash all three LEDs proportionally at the new level, then let them
// fade back to idle through the normal engine — gives instant visual confirmation on real hardware.
void previewBrightness() {
  applyBrightnessToLeds();
  setAllLedLevels(ledOut.maxLevel, ledUsb.maxLevel, ledIn.maxLevel);
  delay(BRIGHTNESS_PREVIEW_HOLD_MS);
  ledOut.currentLevel = ledOut.maxLevel;
  ledUsb.currentLevel = ledUsb.maxLevel;
  ledIn.currentLevel = ledIn.maxLevel;
  ledOut.phase = ledUsb.phase = ledIn.phase = LED_FALLING;
  ledOut.phaseStartMs = ledUsb.phaseStartMs = ledIn.phaseStartMs = millis();
  ledOut.fadeStartLevel = ledOut.maxLevel;
  ledUsb.fadeStartLevel = ledUsb.maxLevel;
  ledIn.fadeStartLevel = ledIn.maxLevel;
  ledOut.lastActivityMs = ledUsb.lastActivityMs = ledIn.lastActivityMs = millis() - ACTIVITY_HOLD_MS - 1;
}

// ============================================================================================
// pack7 / unpack7 — see SYSEX_PROTOCOL.md
// ============================================================================================
uint8_t pack7(const uint8_t *raw, uint8_t rawLen, uint8_t *out) {
  uint8_t outLen = 0;
  uint8_t i = 0;
  while (i < rawLen) {
    uint8_t groupLen = min((uint8_t)7, (uint8_t)(rawLen - i));
    uint8_t msbs = 0;
    for (uint8_t j = 0; j < groupLen; ++j) msbs |= ((raw[i + j] >> 7) & 1) << j;
    out[outLen++] = msbs;
    for (uint8_t j = 0; j < groupLen; ++j) out[outLen++] = raw[i + j] & 0x7F;
    i += groupLen;
  }
  return outLen;
}

uint8_t unpack7(const uint8_t *packed, uint8_t packedLen, uint8_t *out) {
  uint8_t outLen = 0;
  uint8_t i = 0;
  while (i < packedLen) {
    uint8_t msbs = packed[i++];
    uint8_t groupLen = min((uint8_t)7, (uint8_t)(packedLen - i));
    for (uint8_t j = 0; j < groupLen; ++j) {
      out[outLen++] = packed[i + j] | (((msbs >> j) & 1) << 7);
    }
    i += groupLen;
  }
  return outLen;
}

uint8_t checksum7(const uint8_t *raw, uint8_t len) {
  uint16_t sum = 0;
  for (uint8_t i = 0; i < len; ++i) sum += raw[i];
  return (uint8_t)(sum % 128);
}

// ============================================================================================
// Routing engine
// ============================================================================================
void sendDinBytes(uint8_t statusHi, uint8_t channel, uint8_t data1, uint8_t data2, uint8_t len) {
  Serial1.write((uint8_t)(statusHi | channel));
  if (len > 1) Serial1.write(data1);
  if (len > 2) Serial1.write(data2);
  noteActivity(ledOut);
}

void sendUsbBytes(uint8_t statusHi, uint8_t channel, uint8_t data1, uint8_t data2, uint8_t len) {
  uint8_t status = statusHi | channel;
  uint8_t cin;
  switch (statusHi) {
    case 0x80: cin = 0x08; break;
    case 0x90: cin = 0x09; break;
    case 0xA0: cin = 0x0A; break;
    case 0xB0: cin = 0x0B; break;
    case 0xC0: cin = 0x0C; break;
    case 0xD0: cin = 0x0D; break;
    case 0xE0: cin = 0x0E; break;
    // Falls through for System Common/Realtime (statusHi >= 0xF0, no channel nibble to speak of).
    // 0x05 = Single Byte (realtime, or a 1-byte common message like Tune Request). 0x02/0x03 = 2/3-
    // byte System Common (MTC Quarter Frame, Song Select, Song Position Pointer) per the USB-MIDI
    // spec — NOT 0x06/0x04, which are Sysex-continuation CINs and would missort these into whatever
    // sysex stream happens to be in flight on the receiving end.
    default: cin = (len == 1) ? 0x05 : ((len == 2) ? 0x02 : 0x03); break;
  }
  uint8_t packet[4] = {cin, status, data1, data2};
  usb_midi.writePacket(packet);
  noteActivity(ledUsb);
}

void upsertPending(PendingMsg *table, uint8_t &count, const PendingMsg &msg) {
  for (uint8_t i = 0; i < count; ++i) {
    if (table[i].channel == msg.channel && table[i].kind == msg.kind && table[i].number == msg.number) {
      table[i] = msg;
      return;
    }
  }
  if (count < MAX_PENDING) {
    table[count++] = msg;
  }
  // else: pathological fan-out beyond MAX_PENDING distinct destinations for one event — dropped.
}

bool routeMatches(const RouteData &r, uint8_t inputDevice, uint8_t channel, MsgKind kind, uint8_t number) {
  if (!(r.flags & 0x80)) return false;
  uint8_t rd = r.flags & 0x03;
  if (rd != DEV_BOTH && rd != inputDevice) return false;

  // Sysex and System Common/Realtime carry no channel nibble at all, so the input channel bitmask
  // simply doesn't apply to them (unlike every other kind, which is always addressed to one of the
  // 16 channels).
  if (kind != MK_SYSEX && kind != MK_COMMON && kind != MK_REALTIME && kind != MK_CLOCK) {
    uint16_t mask = r.inChLow | ((uint16_t)r.inChHigh << 8);
    if (!(mask & (1u << channel))) return false;
  }

  switch (kind) {
    case MK_NOTE:
      if (!(r.typeFlags & 0x01)) return false;
      if (number < r.noteStart || number > r.noteEnd) return false;
      break;
    case MK_CC:
      if (!(r.typeFlags & 0x02)) return false;
      if (number < r.ccStart || number > r.ccEnd) return false;
      break;
    case MK_PC: if (!(r.typeFlags & 0x04)) return false; break;
    case MK_PITCH: if (!(r.typeFlags & 0x08)) return false; break;
    case MK_POLY_AT: if (!(r.typeFlags & 0x10)) return false; break;
    case MK_CHAN_PRESSURE: if (!(r.typeFlags & 0x20)) return false; break;
    case MK_SYSEX: if (!(r.typeFlags & 0x40)) return false; break;
    case MK_COMMON: if (!(r.flags2 & 0x04)) return false; break;
    case MK_REALTIME: if (!(r.flags2 & 0x08)) return false; break;
    case MK_CLOCK: if (!(r.flags2 & 0x10)) return false; break;
  }
  return true;
}

// Push a Note On (note becomes top) or remove a Note Off, wherever it sits in the stack. Retriggering
// an already-held note keeps its tracked pressure and just moves it back to the top.
void noteStackOn(NoteHoldStack &s, uint8_t note) {
  for (uint8_t i = 0; i < s.count; ++i) {
    if (s.notes[i] == note) {
      uint8_t pressure = s.pressures[i];
      for (uint8_t j = i; j < s.count - 1; ++j) { s.notes[j] = s.notes[j + 1]; s.pressures[j] = s.pressures[j + 1]; }
      s.count--;
      s.notes[s.count] = note; s.pressures[s.count] = pressure; s.count++;
      return;
    }
  }
  if (s.count >= NOTE_STACK_DEPTH) return; // beyond depth: not tracked, can't become priority note
  s.notes[s.count] = note; s.pressures[s.count] = 0; s.count++;
}
void noteStackOff(NoteHoldStack &s, uint8_t note) {
  for (uint8_t i = 0; i < s.count; ++i) {
    if (s.notes[i] == note) {
      for (uint8_t j = i; j < s.count - 1; ++j) { s.notes[j] = s.notes[j + 1]; s.pressures[j] = s.pressures[j + 1]; }
      s.count--;
      return;
    }
  }
}
void noteStackSetPressure(NoteHoldStack &s, uint8_t note, uint8_t pressure) {
  for (uint8_t i = 0; i < s.count; ++i) if (s.notes[i] == note) { s.pressures[i] = pressure; return; }
}

void applyRouteOutput(const RouteData &r, uint8_t inputDevice, uint8_t channel, MsgKind kind, uint8_t number,
                       uint8_t statusHi, uint8_t data1, uint8_t data2) {
  if (r.outDevice == OUT_NONE) return; // enabled + matched, but deliberately silenced

  if (kind == MK_POLY_AT && (r.typeFlags & 0x80)) {
    // Only the top-of-stack ("priority") note may drive the mapped CC — see noteStacks above.
    // emitAftertouchMapUpdate() re-triggers this with the new top's own value whenever priority
    // shifts, so off-priority notes can just be dropped here without the mapped CC going stale.
    NoteHoldStack &s = noteStacks[inputDevice == DEV_TRS ? 0 : 1][channel];
    uint8_t top = s.count ? s.notes[s.count - 1] : 0xFF; // 0xFF: no held note, never a real note number
    if (number != top) return;
  }

  uint16_t outMask = r.outChLow | ((uint16_t)r.outChHigh << 8);
  for (uint8_t c = 0; c < 16; ++c) {
    if (!(outMask & (1u << c))) continue;

    PendingMsg msg;
    msg.used = true;
    msg.channel = c;
    msg.kind = kind;
    msg.statusHi = statusHi;
    msg.len = 3;

    if (kind == MK_CC) {
      int16_t shift = (int16_t)r.ccMapStart - (int16_t)r.ccStart;
      int16_t outCc = (int16_t)number + shift;
      if (outCc < 0 || outCc > 127) continue;
      msg.number = (uint8_t)outCc;
      msg.data1 = (uint8_t)outCc;
      msg.data2 = data2;
    } else if (kind == MK_POLY_AT) {
      if (r.typeFlags & 0x80) {
        // Remapped: aftertouch pressure becomes a CC value on atMapCC instead of passing through
        // as aftertouch. Changes msg.kind/statusHi too so dedup and USB CIN treat it as a real CC.
        msg.kind = MK_CC;
        msg.statusHi = 0xB0;
        msg.number = r.atMapCC;
        msg.data1 = r.atMapCC;
        msg.data2 = data2;
      } else {
        msg.number = number;
        msg.data1 = number;
        msg.data2 = data2;
      }
    } else if (kind == MK_PC) {
      msg.number = 0;
      msg.data1 = data1;
      msg.data2 = 0;
      msg.len = 2;
    } else if (kind == MK_CHAN_PRESSURE) {
      if (r.cpMapCC & 0x80) {
        // Same remap trick as poly aftertouch above, bit7 of cpMapCC is the enable flag since the
        // CC number itself only needs 7 bits.
        uint8_t destCc = r.cpMapCC & 0x7F;
        msg.kind = MK_CC;
        msg.statusHi = 0xB0;
        msg.number = destCc;
        msg.data1 = destCc;
        msg.data2 = data1; // channel pressure's single value becomes the CC value
      } else {
        msg.number = 0;
        msg.data1 = data1;
        msg.data2 = 0;
        msg.len = 2;
      }
    } else if (kind == MK_PITCH) {
      msg.number = 0;
      msg.data1 = data1;
      msg.data2 = data2;
      msg.len = 3;
    } else {
      continue; // sysex handled separately
    }

    if (r.outDevice == OUT_TRS || r.outDevice == OUT_BOTH) upsertPending(pendingTrs, pendingTrsCount, msg);
    if (r.outDevice == OUT_USB || r.outDevice == OUT_BOTH) upsertPending(pendingUsb, pendingUsbCount, msg);
  }
}

void flushPending() {
  for (uint8_t i = 0; i < pendingTrsCount; ++i) {
    PendingMsg &m = pendingTrs[i];
    sendDinBytes(m.statusHi, m.channel, m.data1, m.data2, m.len);
  }
  for (uint8_t i = 0; i < pendingUsbCount; ++i) {
    PendingMsg &m = pendingUsb[i];
    sendUsbBytes(m.statusHi, m.channel, m.data1, m.data2, m.len);
  }
  pendingTrsCount = 0;
  pendingUsbCount = 0;
}

// Called whenever a Note On/Off changes which note is on top of a channel's hold stack — pushes a
// fresh CC value (the new top note's own last-reported pressure, or 0 if nothing's held anymore) to
// any routes mapping poly aftertouch to CC on that channel/device, so the mapped CC doesn't just
// sit stale waiting for that note's next real aftertouch message (which may never come).
void emitAftertouchMapUpdate(uint8_t inputDevice, uint8_t channel, NoteHoldStack &stack) {
  uint8_t topNote = stack.count ? stack.notes[stack.count - 1] : 0xFF;
  uint8_t topPressure = stack.count ? stack.pressures[stack.count - 1] : 0;
  bool any = false;
  for (uint16_t i = 0; i < routeCount; ++i) {
    if (!(routes[i].typeFlags & 0x80)) continue;
    if (routeMatches(routes[i], inputDevice, channel, MK_POLY_AT, topNote)) {
      any = true;
      applyRouteOutput(routes[i], inputDevice, channel, MK_POLY_AT, topNote, 0xA0, topNote, topPressure);
    }
  }
  if (any) flushPending();
}

// USB Adaptor Mode: USB-in -> DIN-out only, DIN-in -> USB-out only (matches the original simple
// adaptor sketch exactly). Router Mode: everything is merged/broadcast to both physical ports.
void applyModeDefault(uint8_t inputDevice, uint8_t statusHi, uint8_t channel, uint8_t data1, uint8_t data2, uint8_t len) {
  if (globalSettings.mode == MODE_USB_ADAPTOR) {
    if (inputDevice == DEV_USB) sendDinBytes(statusHi, channel, data1, data2, len);
    else sendUsbBytes(statusHi, channel, data1, data2, len);
  } else {
    sendDinBytes(statusHi, channel, data1, data2, len);
    sendUsbBytes(statusHi, channel, data1, data2, len);
  }
}

// The single place that actually matches routes and transmits a Note On/Off (Mono Retrig/Round
// Robin included), used both for real incoming note events and for the synthetic kill+retrigger
// pair a mid-note octave shift change sends (see retriggerHeldNotesForShiftChange) — those need to
// go through exactly the same route matching/transpose/velocity-scale/fan-out as a real note would,
// not a simplified copy of it.
void dispatchNoteEvent(uint8_t inputDevice, uint8_t channel, bool isNoteOn, uint8_t note, uint8_t velocity) {
  bool matchedAny = false;
  for (uint16_t i = 0; i < routeCount; ++i) {
    if (routeMatches(routes[i], inputDevice, channel, MK_NOTE, note)) {
      matchedAny = true;
      if (routes[i].flags2 & 0x01) {
        handleMonoRetrigNote(i, routes[i], note, velocity, isNoteOn);
      } else {
        sendRouteNote(i, routes[i], isNoteOn, note, velocity);
        // Without Mono Retrig there's no "silenced but still held" concept — every Note Off here
        // is a genuine release, so it's always correct (and necessary, to avoid leaking a slot per
        // distinct note ever played) to free the Round Robin channel-memory slot now.
        if (!isNoteOn) routeNoteFreeSlot(routeNoteTracks[i], note);
      }
    }
  }
  flushPending();

  if (!matchedAny) {
    applyModeDefault(inputDevice, isNoteOn ? 0x90 : 0x80, channel, note, velocity, 3);
  } else {
    // still count as "seen" on the input side for the IN LED even if fully consumed by routes
    if (inputDevice == DEV_TRS) noteActivity(ledIn); else noteActivity(ledUsb);
  }
}

void processIncomingEvent(uint8_t inputDevice, uint8_t statusHi, uint8_t channel, uint8_t data1, uint8_t data2, uint8_t len) {
  MsgKind kind;
  uint8_t number = 0;
  switch (statusHi) {
    case 0x80: case 0x90: kind = MK_NOTE; number = data1; break;
    case 0xA0: kind = MK_POLY_AT; number = data1; break;
    case 0xB0: kind = MK_CC; number = data1; break;
    case 0xC0: kind = MK_PC; break;
    case 0xD0: kind = MK_CHAN_PRESSURE; break;
    case 0xE0: kind = MK_PITCH; break;
    default: return; // system common/realtime bytes never reach here
  }

  bool isNoteOn = (kind == MK_NOTE) && (statusHi == 0x90 && data2 > 0);

  // Octave Up/Down (front button): substitute the shifted note number before anything else sees
  // it, so route matching/aftertouch tracking/dispatch all agree on the same (possibly transposed)
  // note. Gated on the button action itself, not just currentOctaveShift != 0 — a note held from
  // before the shift engages still needs to be tracked here, so a later shift change can find and
  // retrigger it (see retriggerHeldNotesForShiftChange).
  if (kind == MK_NOTE && octaveShiftActionActive() && octaveShiftMatchesChannel(channel)) {
    if (isNoteOn) {
      int16_t shifted = (int16_t)number + currentOctaveShift;
      if (shifted < 0 || shifted > 127) {
        octaveHeldShiftedNote[channel][number] = 0xFF; // out of range: not sounding
        return; // doesn't pass, same as route transpose
      }
      octaveHeldShiftedNote[channel][number] = (uint8_t)shifted;
      octaveHeldVelocity[channel][number] = data2;
      octaveHeldInputDevice[channel][number] = inputDevice;
      number = (uint8_t)shifted;
      data1 = (uint8_t)shifted;
    } else {
      // Release whatever shifted pitch this original note is CURRENTLY sounding as, not whatever
      // the live shift would recompute to — those can differ if the shift changed while the note
      // was held (retriggerHeldNotesForShiftChange already re-pointed this entry at that pitch).
      uint8_t shiftedNote = octaveHeldShiftedNote[channel][number];
      octaveHeldShiftedNote[channel][number] = 0xFF;
      if (shiftedNote == 0xFF) return; // wasn't actually sounding (e.g. was out of range when pressed) — nothing to release
      number = shiftedNote;
      data1 = shiftedNote;
    }
  }

  // Keep the note-hold stack (used for poly aftertouch -> CC priority, see noteStacks above) in
  // sync with real Note On/Off and poly aftertouch traffic — but only when some route actually
  // needs it (see anyAftertouchMapRoutes above), so this rare feature costs nothing on every
  // Note/aftertouch message when nobody's using it.
  if (anyAftertouchMapRoutes && (kind == MK_NOTE || kind == MK_POLY_AT)) {
    NoteHoldStack &stack = noteStacks[inputDevice == DEV_TRS ? 0 : 1][channel];
    if (kind == MK_NOTE) {
      uint8_t before = stack.count ? stack.notes[stack.count - 1] : 0xFF;
      if (isNoteOn) noteStackOn(stack, number); else noteStackOff(stack, number);
      uint8_t after = stack.count ? stack.notes[stack.count - 1] : 0xFF;
      if (after != before) emitAftertouchMapUpdate(inputDevice, channel, stack);
    } else {
      noteStackSetPressure(stack, number, data2);
    }
  }

  if (kind == MK_NOTE) {
    dispatchNoteEvent(inputDevice, channel, isNoteOn, number, data2);
    return;
  }

  bool matchedAny = false;
  for (uint16_t i = 0; i < routeCount; ++i) {
    if (routeMatches(routes[i], inputDevice, channel, kind, number)) {
      matchedAny = true;
      applyRouteOutput(routes[i], inputDevice, channel, kind, number, statusHi, data1, data2);
    }
  }
  flushPending();

  if (!matchedAny) {
    applyModeDefault(inputDevice, statusHi, channel, data1, data2, len);
  } else {
    // still count as "seen" on the input side for the IN LED even if fully consumed by routes
    if (inputDevice == DEV_TRS) noteActivity(ledIn); else noteActivity(ledUsb);
  }
}

// Shared by Sysex, System Common, and System Realtime — none of them are channel-addressed, so
// there's no per-channel dedup table the way Note/CC/etc get; the last matching route's outDevice
// simply wins as the destination, same idea as route-order priority elsewhere, just resolved once
// up front instead of per-message via upsertPending.
uint8_t resolveDestination(uint8_t inputDevice, MsgKind kind) {
  bool matchedAny = false;
  uint8_t chosen = OUT_NONE;
  for (uint16_t i = 0; i < routeCount; ++i) {
    if (routeMatches(routes[i], inputDevice, 0, kind, 0)) {
      matchedAny = true;
      chosen = routes[i].outDevice;
    }
  }
  if (matchedAny) return chosen;

  if (globalSettings.mode == MODE_USB_ADAPTOR) {
    return (inputDevice == DEV_USB) ? OUT_TRS : OUT_USB;
  }
  return OUT_BOTH;
}

// System Common (0xF1-0xF7 minus sysex framing) and System Realtime (0xF8-0xFF, split into Clock
// vs. everything else) carry no channel nibble and no note/cc number, so — unlike Note/CC/etc —
// they never go through applyRouteOutput's per-channel PendingMsg/dedup machinery, just straight to
// whichever physical port(s) resolveDestination() lands on, mirroring exactly how Sysex is already
// handled. The message kind is inferred from the status byte itself so every call site can just
// pass the raw byte through without also having to classify it themselves.
void forwardCommonOrRealtime(uint8_t inputDevice, uint8_t statusByte, uint8_t data1, uint8_t data2, uint8_t len) {
  MsgKind kind;
  if (statusByte == 0xF8) kind = MK_CLOCK;
  else if (statusByte >= 0xF8) kind = MK_REALTIME;
  else kind = MK_COMMON;
  uint8_t dest = resolveDestination(inputDevice, kind);
  if (dest == OUT_TRS || dest == OUT_BOTH) sendDinBytes(statusByte, 0, data1, data2, len);
  if (dest == OUT_USB || dest == OUT_BOTH) sendUsbBytes(statusByte, 0, data1, data2, len);
}

// ============================================================================================
// SysEx assembly: peek first two data bytes to tell "our command" apart from arbitrary sysex,
// which is streamed straight through to its resolved destination instead of being buffered (a
// synth patch dump can be far bigger than we want to hold in RAM).
// ============================================================================================
void handleSysexCommand(uint8_t sourcePort, const uint8_t *cmdBuf, uint8_t cmdLen);

void usbSysexByte(UsbOutSysexAccum &accum, uint8_t b) {
  accum.buf[accum.len++] = b;
  if (b == 0xF7) {
    uint8_t cin = 0x04 + accum.len;
    uint8_t pkt[4] = {cin, 0, 0, 0};
    for (uint8_t i = 0; i < accum.len; ++i) pkt[1 + i] = accum.buf[i];
    usb_midi.writePacket(pkt);
    accum.len = 0;
    noteActivity(ledUsb);
    return;
  }
  if (accum.len == 3) {
    uint8_t pkt[4] = {0x04, accum.buf[0], accum.buf[1], accum.buf[2]};
    usb_midi.writePacket(pkt);
    accum.len = 0;
    noteActivity(ledUsb);
  }
}

void forwardSysexByte(uint8_t fromInputDevice, uint8_t destDevice, uint8_t b) {
  if (destDevice == OUT_TRS || destDevice == OUT_BOTH) {
    Serial1.write(b);
    noteActivity(ledOut);
  }
  if (destDevice == OUT_USB || destDevice == OUT_BOTH) {
    usbSysexByte(fromInputDevice == DEV_TRS ? usbOutFromDin : usbOutFromUsb, b);
  }
}

void feedSysexByte(uint8_t inputDevice, SysexState &s, uint8_t b) {
  if (b == 0xF0) {
    s.phase = SysexState::PEEK1;
    s.cmdLen = 0;
    return;
  }

  if (b == 0xF7) {
    if (s.phase == SysexState::OURS) {
      handleSysexCommand(inputDevice, s.cmdBuf, s.cmdLen);
    } else if (s.phase == SysexState::FORWARDING) {
      forwardSysexByte(inputDevice, s.fwdDevice, b);
    } else if (s.phase == SysexState::PEEK1) {
      // F0 F7 — empty/malformed message, still forward transparently
      s.fwdDevice = resolveDestination(inputDevice, MK_SYSEX);
      forwardSysexByte(inputDevice, s.fwdDevice, 0xF0);
      forwardSysexByte(inputDevice, s.fwdDevice, b);
    } else if (s.phase == SysexState::PEEK2) {
      s.fwdDevice = resolveDestination(inputDevice, MK_SYSEX);
      forwardSysexByte(inputDevice, s.fwdDevice, 0xF0);
      forwardSysexByte(inputDevice, s.fwdDevice, 0x7D);
      forwardSysexByte(inputDevice, s.fwdDevice, b);
    }
    s.phase = SysexState::IDLE;
    return;
  }

  if (b & 0x80) {
    // any other new status byte aborts the sysex per spec; caller re-handles it as normal status
    s.phase = SysexState::IDLE;
    return;
  }

  switch (s.phase) {
    case SysexState::PEEK1:
      if (b == SYSEX_MFR_ID) {
        s.phase = SysexState::PEEK2;
      } else {
        s.fwdDevice = resolveDestination(inputDevice, MK_SYSEX);
        forwardSysexByte(inputDevice, s.fwdDevice, 0xF0);
        forwardSysexByte(inputDevice, s.fwdDevice, b);
        s.phase = SysexState::FORWARDING;
      }
      break;
    case SysexState::PEEK2:
      if (b == SYSEX_DEVICE_ID) {
        s.phase = SysexState::OURS;
        s.cmdLen = 0;
      } else {
        s.fwdDevice = resolveDestination(inputDevice, MK_SYSEX);
        forwardSysexByte(inputDevice, s.fwdDevice, 0xF0);
        forwardSysexByte(inputDevice, s.fwdDevice, SYSEX_MFR_ID);
        forwardSysexByte(inputDevice, s.fwdDevice, b);
        s.phase = SysexState::FORWARDING;
      }
      break;
    case SysexState::OURS:
      if (s.cmdLen < sizeof(s.cmdBuf)) s.cmdBuf[s.cmdLen++] = b;
      break;
    case SysexState::FORWARDING:
      forwardSysexByte(inputDevice, s.fwdDevice, b);
      break;
    default:
      break; // stray data byte outside a sysex message; ignore
  }
}

// ============================================================================================
// SysEx command handling
// ============================================================================================
void beginReply(uint8_t cmd) {
  replyLen = 0;
  replyBuf[replyLen++] = 0xF0;
  replyBuf[replyLen++] = SYSEX_MFR_ID;
  replyBuf[replyLen++] = SYSEX_DEVICE_ID;
  replyBuf[replyLen++] = cmd;
}

void appendReply(uint8_t b) { if (replyLen < sizeof(replyBuf) - 1) replyBuf[replyLen++] = b; }

void sendReply(uint8_t sourcePort) {
  appendReply(0xF7);
  if (sourcePort == DEV_TRS) {
    for (uint8_t i = 0; i < replyLen; ++i) Serial1.write(replyBuf[i]);
    noteActivity(ledOut);
  } else {
    UsbOutSysexAccum accum; // fresh accumulator: our own replies are always short + self-contained
    for (uint8_t i = 0; i < replyLen; ++i) usbSysexByte(accum, replyBuf[i]);
  }
}

void sendAck(uint8_t sourcePort, uint8_t originalCmd, uint8_t status, uint8_t checksum, uint16_t extra) {
  beginReply(CMD_ACK);
  appendReply(originalCmd);
  appendReply(status);
  appendReply(checksum);
  appendReply(extra & 0x7F);
  appendReply((extra >> 7) & 0x7F);
  sendReply(sourcePort);
}

void resetButtonRuntimeState();

uint16_t read16(const uint8_t *buf) { return (uint16_t)buf[0] | ((uint16_t)buf[1] << 7); }

void handleSysexCommand(uint8_t sourcePort, const uint8_t *cmdBuf, uint8_t cmdLen) {
  if (cmdLen < 1) return;
  uint8_t cmd = cmdBuf[0];
  const uint8_t *payload = cmdBuf + 1;
  uint8_t payloadLen = cmdLen - 1;

  switch (cmd) {
    case CMD_IDENTITY_REQUEST: {
      beginReply(CMD_IDENTITY_REPLY);
      appendReply(PROTOCOL_VERSION);
      appendReply(FW_VERSION_MAJOR);
      appendReply(FW_VERSION_MINOR);
      appendReply(MAX_ROUTES & 0x7F);
      appendReply((MAX_ROUTES >> 7) & 0x7F);
      for (uint8_t i = 0; DEVICE_NAME[i] != '\0'; ++i) appendReply((uint8_t)DEVICE_NAME[i] & 0x7F);
      sendReply(sourcePort);
      break;
    }

    case CMD_GET_GLOBAL: {
      beginReply(CMD_GLOBAL_DATA);
      appendReply(globalSettings.mode);
      appendReply(globalSettings.buttonAction);
      appendReply(globalSettings.buttonParamA);
      appendReply(globalSettings.buttonParamB);
      appendReply(globalSettings.buttonToggleMomentary);
      appendReply(globalSettings.ledBrightnessStep);
      sendReply(sourcePort);
      break;
    }

    case CMD_SET_GLOBAL: {
      if (payloadLen < 6) { sendAck(sourcePort, cmd, ACK_ERR_BAD_DATA, 0, 0); break; }
      globalSettings.mode = payload[0];
      globalSettings.buttonAction = payload[1];
      globalSettings.buttonParamA = payload[2];
      globalSettings.buttonParamB = payload[3];
      globalSettings.buttonToggleMomentary = payload[4];
      globalSettings.ledBrightnessStep = payload[5];
      applyBrightnessToLeds();
      resetButtonRuntimeState();
      sendAck(sourcePort, cmd, ACK_OK, checksum7(payload, 6), 0);
      break;
    }

    case CMD_GET_ROUTE_COUNT: {
      beginReply(CMD_ROUTE_COUNT);
      appendReply(routeCount & 0x7F);
      appendReply((routeCount >> 7) & 0x7F);
      sendReply(sourcePort);
      break;
    }

    case CMD_GET_ROUTE: {
      if (payloadLen < 2) { sendAck(sourcePort, cmd, ACK_ERR_BAD_DATA, 0, 0); break; }
      uint16_t idx = read16(payload);
      if (idx >= routeCount) { sendAck(sourcePort, cmd, ACK_ERR_BAD_INDEX, 0, 0); break; }
      beginReply(CMD_ROUTE_DATA);
      appendReply(idx & 0x7F);
      appendReply((idx >> 7) & 0x7F);
      uint8_t packed[48]; // pack7(41 raw bytes) = 47 packed bytes
      uint8_t packedLen = pack7((const uint8_t *)&routes[idx], ROUTE_RAW_BYTES, packed);
      for (uint8_t i = 0; i < packedLen; ++i) appendReply(packed[i]);
      sendReply(sourcePort);
      break;
    }

    case CMD_ADD_ROUTE: {
      if (routeCount >= MAX_ROUTES) { sendAck(sourcePort, cmd, ACK_ERR_FULL, 0, 0); break; }
      RouteData r;
      unpack7(payload, payloadLen, (uint8_t *)&r);
      routes[routeCount] = r;
      uint16_t newIndex = routeCount;
      routeCount++;
      recomputeAnyAftertouchMapRoutes();
      resetAllRouteNoteTracks();
      sendAck(sourcePort, cmd, ACK_OK, checksum7((const uint8_t *)&r, ROUTE_RAW_BYTES), newIndex);
      break;
    }

    case CMD_SET_ROUTE: {
      if (payloadLen < 2) { sendAck(sourcePort, cmd, ACK_ERR_BAD_DATA, 0, 0); break; }
      uint16_t idx = read16(payload);
      if (idx >= routeCount) { sendAck(sourcePort, cmd, ACK_ERR_BAD_INDEX, 0, 0); break; }
      RouteData r;
      unpack7(payload + 2, payloadLen - 2, (uint8_t *)&r);
      routes[idx] = r;
      recomputeAnyAftertouchMapRoutes();
      resetAllRouteNoteTracks();
      sendAck(sourcePort, cmd, ACK_OK, checksum7((const uint8_t *)&r, ROUTE_RAW_BYTES), idx);
      break;
    }

    case CMD_DELETE_ROUTE: {
      if (payloadLen < 2) { sendAck(sourcePort, cmd, ACK_ERR_BAD_DATA, 0, 0); break; }
      uint16_t idx = read16(payload);
      if (idx >= routeCount) { sendAck(sourcePort, cmd, ACK_ERR_BAD_INDEX, 0, 0); break; }
      for (uint16_t i = idx; i + 1 < routeCount; ++i) routes[i] = routes[i + 1];
      routeCount--;
      recomputeAnyAftertouchMapRoutes();
      resetAllRouteNoteTracks();
      sendAck(sourcePort, cmd, ACK_OK, 0, idx);
      break;
    }

    case CMD_MOVE_ROUTE: {
      if (payloadLen < 4) { sendAck(sourcePort, cmd, ACK_ERR_BAD_DATA, 0, 0); break; }
      uint16_t from = read16(payload);
      uint16_t to = read16(payload + 2);
      if (from >= routeCount || to >= routeCount) { sendAck(sourcePort, cmd, ACK_ERR_BAD_INDEX, 0, 0); break; }
      RouteData moved = routes[from];
      if (from < to) {
        for (uint16_t i = from; i < to; ++i) routes[i] = routes[i + 1];
      } else if (from > to) {
        for (uint16_t i = from; i > to; --i) routes[i] = routes[i - 1];
      }
      routes[to] = moved;
      resetAllRouteNoteTracks(); // route order changed, so per-index tracking state would be misattributed otherwise
      sendAck(sourcePort, cmd, ACK_OK, 0, to);
      break;
    }

    case CMD_CLEAR_ALL_ROUTES: {
      routeCount = 0;
      recomputeAnyAftertouchMapRoutes();
      resetAllRouteNoteTracks();
      sendAck(sourcePort, cmd, ACK_OK, 0, 0);
      break;
    }

    case CMD_PREVIEW_BRIGHTNESS: {
      if (payloadLen < 1) { sendAck(sourcePort, cmd, ACK_ERR_BAD_DATA, 0, 0); break; }
      globalSettings.ledBrightnessStep = payload[0];
      previewBrightness();
      sendAck(sourcePort, cmd, ACK_OK, payload[0], 0);
      break;
    }

    case CMD_COMMIT: {
      saveToFlash();
      sendAck(sourcePort, cmd, ACK_OK, 0, 0);
      break;
    }

    case CMD_FACTORY_RESET: {
      setFactoryDefaults();
      applyBrightnessToLeds();
      resetButtonRuntimeState();
      saveToFlash();
      sendAck(sourcePort, cmd, ACK_OK, 0, 0);
      allLedConfirmationFlashes();
      break;
    }

    default:
      break; // unknown command, ignore
  }
}

// ============================================================================================
// DIN input parsing (running status) -> processIncomingEvent / feedSysexByte
// ============================================================================================
uint8_t midiDataLength(uint8_t status) {
  if (status >= 0xF8) return 0;
  if (status >= 0xF0) {
    switch (status) {
      case 0xF1: case 0xF3: return 1;
      case 0xF2: return 2;
      default: return 0;
    }
  }
  switch (status & 0xF0) {
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
    case 0xC0: case 0xD0: return 1;
    default: return 0;
  }
}

void processDinByte(uint8_t b) {
  if (b >= 0xF8) {
    // realtime: never buffered, never blocks sysex — but now goes through the same route-matching
    // (and Mode fallthrough) as everything else, instead of always auto-passing regardless of route
    // config.
    noteActivity(ledIn);
    forwardCommonOrRealtime(DEV_TRS, b, 0, 0, 1);
    return;
  }

  if (dinSysex.phase != SysexState::IDLE || b == 0xF0) {
    noteActivity(ledIn);
    feedSysexByte(DEV_TRS, dinSysex, b);
    if (dinSysex.phase != SysexState::IDLE) return;
    if (b == 0xF0 || b == 0xF7) return;
    // fall through: feedSysexByte aborted on an unexpected status byte, reprocess b normally
  }

  noteActivity(ledIn);

  if (b & 0x80) {
    dinParser.runningStatus = b;
    dinParser.needed = midiDataLength(b);
    dinParser.have = 0;
    if (dinParser.needed == 0) {
      // Only reachable for a System Common status with no data bytes (Tune Request 0xF6) — every
      // real channel status (0x80-0xE0) always needs at least 1 data byte per midiDataLength(), and
      // Realtime (0xF8+) is already intercepted above before ever reaching here.
      forwardCommonOrRealtime(DEV_TRS, b, 0, 0, 1);
      dinParser.runningStatus = 0;
    }
    return;
  }

  if (!dinParser.runningStatus || !dinParser.needed) return;
  dinParser.data[dinParser.have++] = b;
  if (dinParser.have < dinParser.needed) return;

  const uint8_t status = dinParser.runningStatus;
  const uint8_t data1 = dinParser.data[0];
  const uint8_t data2 = (dinParser.needed > 1) ? dinParser.data[1] : 0;
  if (status >= 0xF0) {
    // System Common with data bytes: MTC Quarter Frame (0xF1), Song Position Pointer (0xF2), Song
    // Select (0xF3). Previously this fell into processIncomingEvent with `status & 0xF0`, which
    // collapses all three to 0xF0 and silently drops them (0xF0 isn't a case processIncomingEvent
    // handles) — routed through forwardCommonOrRealtime now so they're no longer dropped.
    forwardCommonOrRealtime(DEV_TRS, status, data1, data2, dinParser.needed + 1);
  } else {
    processIncomingEvent(DEV_TRS, status & 0xF0, status & 0x0F, data1, data2, dinParser.needed + 1);
  }
  dinParser.have = 0;
  if (status >= 0xF0) dinParser.runningStatus = 0;
}

void pumpDinIn() {
  while (Serial1.available() > 0) {
    processDinByte(static_cast<uint8_t>(Serial1.read()));
  }
}

// ============================================================================================
// USB input parsing (4-byte USB-MIDI packets) -> processIncomingEvent / feedSysexByte
// ============================================================================================
void pumpUsbIn() {
  uint8_t packet[4];
  while (usb_midi.readPacket(packet)) {
    uint8_t cin = packet[0] & 0x0F;
    noteActivity(ledUsb);

    if (cin == 0x04) {
      feedSysexByte(DEV_USB, usbSysex, packet[1]);
      feedSysexByte(DEV_USB, usbSysex, packet[2]);
      feedSysexByte(DEV_USB, usbSysex, packet[3]);
      continue;
    }
    if (cin == 0x06) {
      feedSysexByte(DEV_USB, usbSysex, packet[1]);
      feedSysexByte(DEV_USB, usbSysex, packet[2]);
      continue;
    }
    if (cin == 0x07) {
      feedSysexByte(DEV_USB, usbSysex, packet[1]);
      feedSysexByte(DEV_USB, usbSysex, packet[2]);
      feedSysexByte(DEV_USB, usbSysex, packet[3]);
      continue;
    }
    if (cin == 0x05) {
      if (usbSysex.phase != SysexState::IDLE) {
        feedSysexByte(DEV_USB, usbSysex, packet[1]); // lone 0xF7 closing a sysex
      } else {
        // Realtime (>=0xF8) or a 1-byte System Common message (Tune Request 0xF6) — both route
        // through forwardCommonOrRealtime, which tells them apart by the status byte itself.
        forwardCommonOrRealtime(DEV_USB, packet[1], 0, 0, 1);
      }
      continue;
    }
    if (cin == 0x02) { // 2-byte System Common: MTC Quarter Frame (0xF1) or Song Select (0xF3)
      forwardCommonOrRealtime(DEV_USB, packet[1], packet[2], 0, 2);
      continue;
    }
    if (cin == 0x03) { // 3-byte System Common: Song Position Pointer (0xF2)
      forwardCommonOrRealtime(DEV_USB, packet[1], packet[2], packet[3], 3);
      continue;
    }

    uint8_t statusHi = packet[1] & 0xF0;
    uint8_t channel = packet[1] & 0x0F;
    switch (cin) {
      case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0E:
        processIncomingEvent(DEV_USB, statusHi, channel, packet[2], packet[3], 3);
        break;
      case 0x0C: case 0x0D:
        processIncomingEvent(DEV_USB, statusHi, channel, packet[2], 0, 2);
        break;
      case 0x0F:
        // Same "Single Byte" convention as CIN 0x05 above — some hosts use 0x0F instead of 0x05 for
        // realtime/single-byte-common; forwardCommonOrRealtime handles either the same way.
        forwardCommonOrRealtime(DEV_USB, packet[1], 0, 0, 1);
        break;
      default:
        break;
    }
  }
}

// ============================================================================================
// Front button: panic / pitch ramp / mod ramp / cc momentary / cc toggle / octave shift
// ============================================================================================
bool octaveShiftActionActive() {
  return globalSettings.buttonAction == BTN_OCTAVE_UP || globalSettings.buttonAction == BTN_OCTAVE_DOWN;
}

// buttonParamB is the channel Octave Up/Down affects (unlike resolveButtonChannel(), "All" here
// genuinely means every channel rather than collapsing to channel 1 — there's no single message
// being sent that needs one specific channel, this just gates which passing notes get shifted).
bool octaveShiftMatchesChannel(uint8_t ch) {
  return globalSettings.buttonParamB > 15 || globalSettings.buttonParamB == ch;
}

// Kills and immediately retriggers (at the new shift) every note this channel currently has held,
// so a mid-note octave change is heard as a pitch jump on the sustained note rather than a dropout
// that needs the key released and re-pressed to resume. Called before currentOctaveShift is
// actually updated, so octaveHeldShiftedNote's old values are still the currently-sounding pitch.
void retriggerHeldNotesForShiftChange(uint8_t channel, int8_t newShift) {
  for (uint16_t note = 0; note < 128; ++note) {
    uint8_t oldShifted = octaveHeldShiftedNote[channel][note];
    if (oldShifted == 0xFF) continue; // nothing currently held for this original note
    uint8_t inputDevice = octaveHeldInputDevice[channel][note];
    uint8_t velocity = octaveHeldVelocity[channel][note];

    dispatchNoteEvent(inputDevice, channel, false, oldShifted, 0); // kill the old pitch

    int16_t newShiftedI = (int16_t)note + newShift;
    if (newShiftedI < 0 || newShiftedI > 127) {
      // Out of range at the new shift: stays silent (matching route transpose's own out-of-range
      // behavior) until the key is released and pressed again.
      octaveHeldShiftedNote[channel][note] = 0xFF;
      continue;
    }
    uint8_t newShifted = (uint8_t)newShiftedI;
    octaveHeldShiftedNote[channel][note] = newShifted;
    dispatchNoteEvent(inputDevice, channel, true, newShifted, velocity); // start the new pitch
  }
}

void resetButtonRuntimeState() {
  buttonHeld = false;
  ccToggleState = false;
  noteToggleState = false;
  octaveToggleState = false;
  currentOctaveShift = 0;
  rampActive = false;
  rampValue = 0;
  memset(octaveHeldShiftedNote, 0xFF, sizeof(octaveHeldShiftedNote));
}

uint8_t resolveButtonChannel() {
  return (globalSettings.buttonParamB > 15) ? 0 : globalSettings.buttonParamB; // "All" collapses to ch1 for single-channel messages
}

void sendPanic() {
  // Deliberately NOT sending an individual Note Off for every note on every channel here — that's
  // 16 x 128 x 2 ports = 4096+ messages, which over a 31250-baud DIN line takes close to 2 seconds
  // of real transmit time and makes "panic" feel broken instead of instant. CC 120 (All Sound Off)
  // and CC 123 (All Notes Off) already silence essentially all MIDI gear immediately; the full
  // per-note sweep is a legacy fallback for very old non-compliant synths that's not worth trading
  // away a responsive panic button for. 96 messages total here, well under 100ms even on DIN.
  for (uint8_t channel = 0; channel < 16; ++channel) {
    applyModeDefault(DEV_USB, 0xB0, channel, 121, 0, 3); // Reset All Controllers
    applyModeDefault(DEV_USB, 0xB0, channel, 123, 0, 3); // All Notes Off
    applyModeDefault(DEV_USB, 0xB0, channel, 120, 0, 3); // All Sound Off
    applyModeDefault(DEV_TRS, 0xB0, channel, 121, 0, 3);
    applyModeDefault(DEV_TRS, 0xB0, channel, 123, 0, 3);
    applyModeDefault(DEV_TRS, 0xB0, channel, 120, 0, 3);
    serviceLeds();
  }
  Serial1.flush();
  allLedConfirmationFlashes();
}

void startRamp(bool rising) {
  rampActive = true;
  rampRising = rising;
  rampStartValue = rampValue;
  rampStartMs = millis();

  bool isPitch = (globalSettings.buttonAction == BTN_PITCH_UP || globalSettings.buttonAction == BTN_PITCH_DOWN);
  int16_t maxVal = isPitch ? 8191 : 127;
  int16_t target = rising ? maxVal : 0;

  // buttonParamA sets how long a FULL 0<->maxVal sweep takes. If this leg only has to cover part
  // of that distance — e.g. the button was released partway through the rise — scale the duration
  // down proportionally so the ramp's actual rate stays constant, instead of a short partial leg
  // taking just as long as a full sweep would.
  float x = (127.0f - globalSettings.buttonParamA) / 127.0f;
  float fullSweepSeconds = RAMP_MAX_SECONDS * x * x;
  uint32_t fullSweepMs = (uint32_t)(fullSweepSeconds * 1000.0f + 0.5f);
  int32_t remainingDistance = abs((int32_t)target - (int32_t)rampStartValue);
  rampDurationMs = (uint32_t)((int64_t)remainingDistance * fullSweepMs / maxVal);
}

void serviceRamp() {
  if (!rampActive) return;
  const uint32_t now = millis();

  bool isPitch = (globalSettings.buttonAction == BTN_PITCH_UP || globalSettings.buttonAction == BTN_PITCH_DOWN);
  int16_t maxVal = isPitch ? 8191 : 127;
  int16_t target = rampRising ? maxVal : 0;

  // Interpolate directly from elapsed time rather than pre-dividing the sweep into a fixed number
  // of steps at a fixed interval: at short durations (well under a second), a handful of coarse
  // steps quantized to whole milliseconds collapses many different speed settings down to the same
  // 0ms or 1ms-per-step timing, making them sound identical. Recomputing the target value fresh
  // from now - rampStartMs every tick uses the device's full time resolution instead, and only
  // actually sends a MIDI message when that recomputed value has changed.
  uint32_t elapsed = now - rampStartMs;
  bool done = (rampDurationMs == 0 || elapsed >= rampDurationMs);
  int16_t newValue = done ? target
    : (int16_t)(rampStartValue + (int32_t)(target - rampStartValue) * (int32_t)elapsed / (int32_t)rampDurationMs);

  if (newValue != rampValue) {
    rampValue = newValue;
    uint8_t channel = resolveButtonChannel();
    if (isPitch) {
      bool down = (globalSettings.buttonAction == BTN_PITCH_DOWN);
      int16_t bend = down ? -rampValue : rampValue;
      uint16_t centered = (uint16_t)(8192 + bend);
      applyModeDefault(DEV_USB, 0xE0, channel, centered & 0x7F, (centered >> 7) & 0x7F, 3);
      applyModeDefault(DEV_TRS, 0xE0, channel, centered & 0x7F, (centered >> 7) & 0x7F, 3);
    } else {
      applyModeDefault(DEV_USB, 0xB0, channel, 1, (uint8_t)rampValue, 3);
      applyModeDefault(DEV_TRS, 0xB0, channel, 1, (uint8_t)rampValue, 3);
    }
  }

  if (done && !rampRising) rampActive = false;
}

void sendCc(uint8_t value) {
  uint8_t channel = resolveButtonChannel();
  applyModeDefault(DEV_USB, 0xB0, channel, globalSettings.buttonParamA, value, 3);
  applyModeDefault(DEV_TRS, 0xB0, channel, globalSettings.buttonParamA, value, 3);
}

// Note momentary/toggle reuse buttonParamA as the note number (0-127, same field cc actions use
// for the cc number) and buttonParamB as the channel — no protocol/struct changes needed.
void sendNote(bool on) {
  uint8_t channel = resolveButtonChannel();
  uint8_t note = globalSettings.buttonParamA;
  uint8_t statusHi = on ? 0x90 : 0x80;
  uint8_t velocity = on ? 127 : 0;
  applyModeDefault(DEV_USB, statusHi, channel, note, velocity, 3);
  applyModeDefault(DEV_TRS, statusHi, channel, note, velocity, 3);
}

void setOctaveShift(int8_t newShift) {
  // Unlike resolveButtonChannel()'s single-channel collapse, "All" here needs every channel
  // retriggered, not just channel 1 — octaveShiftMatchesChannel() treats "All" the same way when
  // deciding which passing notes to shift in the first place.
  if (newShift != currentOctaveShift) {
    if (globalSettings.buttonParamB > 15) {
      for (uint8_t ch = 0; ch < 16; ++ch) retriggerHeldNotesForShiftChange(ch, newShift);
    } else {
      retriggerHeldNotesForShiftChange(globalSettings.buttonParamB, newShift);
    }
  }
  currentOctaveShift = newShift;
}

void onButtonPress() {
  buttonHeld = true;
  switch (globalSettings.buttonAction) {
    case BTN_PANIC: sendPanic(); break;
    case BTN_PITCH_UP: case BTN_PITCH_DOWN: startRamp(true); break;
    case BTN_MOD: startRamp(true); break;
    case BTN_CC_MOMENTARY: sendCc(127); break;
    case BTN_CC_TOGGLE:
      ccToggleState = !ccToggleState;
      sendCc(ccToggleState ? 127 : 0);
      break;
    case BTN_OCTAVE_UP:
      if (globalSettings.buttonToggleMomentary) { octaveToggleState = !octaveToggleState; setOctaveShift(octaveToggleState ? 12 : 0); }
      else setOctaveShift(12);
      break;
    case BTN_OCTAVE_DOWN:
      if (globalSettings.buttonToggleMomentary) { octaveToggleState = !octaveToggleState; setOctaveShift(octaveToggleState ? -12 : 0); }
      else setOctaveShift(-12);
      break;
    case BTN_NOTE_MOMENTARY: sendNote(true); break;
    case BTN_NOTE_TOGGLE:
      noteToggleState = !noteToggleState;
      sendNote(noteToggleState);
      break;
  }
}

void onButtonRelease() {
  buttonHeld = false;
  switch (globalSettings.buttonAction) {
    case BTN_PITCH_UP: case BTN_PITCH_DOWN: case BTN_MOD:
      startRamp(false);
      break;
    case BTN_CC_MOMENTARY: sendCc(0); break;
    case BTN_OCTAVE_UP: case BTN_OCTAVE_DOWN:
      if (!globalSettings.buttonToggleMomentary) setOctaveShift(0);
      break;
    case BTN_NOTE_MOMENTARY: sendNote(false); break;
    default: break;
  }
}

void serviceButton() {
  const uint32_t now = millis();
  const bool reading = digitalRead(PIN_BUTTON);

  if (reading != lastButtonReading) {
    lastButtonChangeMs = now;
    lastButtonReading = reading;
  }

  if (now - lastButtonChangeMs < BUTTON_DEBOUNCE_MS) return;
  if (reading == stableButtonState) return;

  stableButtonState = reading;
  if (stableButtonState == LOW) onButtonPress();
  else onButtonRelease();
}

// ============================================================================================
// Setup / loop
// ============================================================================================
void setup() {
  pinMode(PIN_LED_OUT, OUTPUT);
  pinMode(PIN_LED_USB, OUTPUT);
  pinMode(PIN_LED_IN, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  setAllLedLevels(0, 0, 0);

  const bool factoryResetRequested = digitalRead(PIN_BUTTON) == LOW;

  // Manufacturer/product descriptors must be set before TinyUSBDevice.begin() — that's when USB
  // enumeration happens, and it's what makes the OS/DAW show "FoxWizard" instead of a generic
  // "RP2040" in the MIDI port list.
  TinyUSBDevice.setManufacturerDescriptor(USB_DESCRIPTOR_NAME);
  TinyUSBDevice.setProductDescriptor(USB_DESCRIPTOR_NAME);

  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
  }

  usb_midi.setStringDescriptor(USB_DESCRIPTOR_NAME);
  usb_midi.begin();

  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }

  Serial1.setTX(PIN_DIN_MIDI_TX);
  Serial1.setRX(PIN_DIN_MIDI_RX);
  Serial1.begin(DIN_BAUD);

  if (factoryResetRequested) {
    setFactoryDefaults();
    saveToFlash();
  } else {
    loadFromFlash();
  }
  applyBrightnessToLeds();
  resetButtonRuntimeState();

  startupSequence();
}

void loop() {
#ifdef TINYUSB_NEED_POLLING_TASK
  TinyUSBDevice.task();
#endif

  pumpUsbIn();
  pumpDinIn();
  serviceButton();
  serviceRamp();
  serviceLeds();
}
