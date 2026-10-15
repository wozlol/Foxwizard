/*
  Mini_MIDI_Multitool_Host_Edition.ino by woz.lol
  FoxWizard Mini MIDI Multitool, Host Edition main brain. Same as Mini_MIDI_Multitool.ino plus a
  serial link to the host brain (../Foxwizard_Host_Edition_Sidecar), which adds a USB hub host, one more
  DIN/TRS MIDI IN, and five more DIN/TRS MIDI OUTs. See ../README.md.

  Ports: TRS/DIN and USB Device on this board. With a sidecar attached, also USB Host 1-4 (one per
  hub port) and TRS 2-6 (TRS 2 is an input and an output, TRS 3-6 are outputs only).
  USB Adaptor Mode: every input goes to every other port, never back to itself. Router Mode: every
  input goes to every output. Routes take over either default.

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
  - GP26 = host link TX to host brain GP29 (PIO serial)
  - GP27 = host link RX from host brain GP28 (PIO serial)

  Arduino setup:
  - Board: Waveshare RP2040-Zero
  - Tools -> USB Stack = Adafruit TinyUSB
  - Tools -> Flash Size = 2MB (Sketch: 1792KB, FS: 256KB). Firmware updates over MIDI are staged in
    that FS. Saved settings live in the EEPROM area, which sits at the same address in every layout.

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
#include <Updater.h>
#include "foxwizard_link.h"
#include "src/clock_engine.h"

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
constexpr uint8_t PIN_LINK_TX = 26;
constexpr uint8_t PIN_LINK_RX = 27;
constexpr size_t LINK_RX_BUFFER = 8192;        // filled by the PIO receive interrupt
constexpr uint16_t LINK_RX_PAUSE_AT = 2048;     // ask the host brain to pause above this many waiting bytes
constexpr uint16_t LINK_RX_RESUME_AT = 1024;    // and to resume below this
constexpr uint16_t LINK_RX_BUDGET = 256;        // bytes parsed per loop, so a flood never starves the rest
constexpr uint8_t INPUT_BUDGET = 64;            // USB Device packets or TRS/DIN bytes handled per loop
constexpr uint32_t WATCHDOG_MS = 4000;          // reboots if the loop ever stops running this long
constexpr uint32_t HOST_PRESENT_MS = 1500;      // host brain counts as present this long after its last port map frame
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
constexpr uint8_t PROTOCOL_VERSION = 4; // 2: 45-byte routes with port masks, GET_STATUS. 3: clock settings, LEARN. 4: 53-byte routes with feature fields, feature blocks
constexpr uint8_t FW_VERSION_MAJOR = 1;
constexpr uint8_t FW_VERSION_MINOR = 22;
constexpr uint8_t FW_CHUNK_BYTES = 128;          // firmware bytes per FW_DATA message, 147 once packed
constexpr uint32_t FW_REBOOT_DELAY_MS = 300;     // lets the FW_END ACK reach the host before rebooting
const char DEVICE_NAME[] = "FoxWizard Mini MIDI Multitool"; // long name sent in the SysEx IDENTITY_REPLY
const char USB_DESCRIPTOR_NAME[] = "FoxWizard"; // short name shown by the OS/DAW in the USB MIDI port list

constexpr uint16_t MAX_ROUTES = 64; // leaves each saved route room to grow to 63 bytes, see the EEPROM layout below
constexpr uint8_t ROUTE_NAME_MAX_LEN = 24; // ASCII characters, enforced web-side too
constexpr uint8_t ROUTE_RAW_BYTES = 17 + ROUTE_NAME_MAX_LEN + 4 + 8; // protocol 1: 41 bytes, protocol 2-3: 45, protocol 4: 53
constexpr uint8_t MAX_PENDING = 64;

// ============================================================================================
// Types
// ============================================================================================
enum SysexCmd : uint8_t {
  CMD_IDENTITY_REQUEST = 0x01,
  CMD_IDENTITY_REPLY = 0x02,
  CMD_GET_GLOBAL = 0x10,
  CMD_GET_STATUS = 0x13,
  CMD_STATUS_DATA = 0x14,
  CMD_LEARN = 0x15,
  CMD_LEARN_DATA = 0x16,
  CMD_GET_FEATURE = 0x17,
  CMD_FEATURE_DATA = 0x18,
  CMD_SET_FEATURE = 0x19,
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
  CMD_FW_BEGIN = 0x40,
  CMD_FW_DATA = 0x41,
  CMD_FW_END = 0x42,
  CMD_ACK = 0x7E
};

enum AckStatus : uint8_t {
  ACK_OK = 0,
  ACK_ERR_BAD_INDEX = 1,
  ACK_ERR_BAD_DATA = 2,
  ACK_ERR_FULL = 3,
  ACK_ERR_UPDATE = 4
};

// Input sources. DEV_BOTH is only a legacy route input value, meaning TRS/DIN and USB Device.
enum : uint8_t { DEV_TRS = 0, DEV_USB = 1, DEV_BOTH = 2, DEV_HOST = 3, DEV_TRS2 = 4 };
constexpr uint8_t SOURCE_COUNT = 5;
// Ports, one bit each, for route inPorts/outPorts and every output mask. TRS 3-6 are outputs only.
// Bits 0-1 match the legacy outDevice values (1 TRS/DIN, 2 USB Device, 3 both).
constexpr uint16_t PORT_TRS = 1u << 0;
constexpr uint16_t PORT_USB = 1u << 1;
constexpr uint16_t PORT_HOST1 = 1u << 2;     // USB Host 1-4 are bits 2-5
constexpr uint16_t PORT_TRS2 = 1u << 6;      // TRS 2-6 are bits 6-10
constexpr uint16_t PORT_HOST_ALL = 0x003C;
constexpr uint16_t PORT_TRS2_6 = 0x07C0;
constexpr uint8_t PORT_COUNT = 11;
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
  BTN_NOTE_TOGGLE = 9,
  BTN_TAP_TEMPO = 10
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
  uint8_t flags;      // bit7 enabled, bit6 inPorts/outPorts in use, bits1-0 legacy inputDevice
  uint8_t inChLow;
  uint8_t inChHigh;
  uint8_t typeFlags;  // bit0 note,1 cc,2 pc,3 pitch,4 polyAT,5 chanPressure,6 sysex,7 polyAT mapped to CC
  uint8_t noteStart;
  uint8_t noteEnd;
  uint8_t ccStart;
  uint8_t ccEnd;
  uint8_t outDevice;  // legacy outputs (bit0 TRS/DIN, bit1 USB Device), used when flags bit6 is clear
  uint8_t outChLow;
  uint8_t outChHigh;
  uint8_t transpose;  // semitone + 64
  uint8_t velocityScale; // percent, 10-200, applied to outgoing Note On velocity (100 = unchanged)
  uint8_t ccMapStart;
  uint8_t atMapCC;    // destination CC number when typeFlags bit7 is set (poly aftertouch remapped to CC)
  uint8_t cpMapCC;    // bit7=channel pressure remapped to CC, bits6-0=destination CC number
  uint8_t flags2;     // bit0=Mono Retrig, bit1=Round Robin, bit2=System Common, bit3=System Realtime (Transport, everything but Clock), bit4=MIDI Clock, bit5=Round Robin Random (true random channel pick instead of cycling; only meaningful with bit1 set)
  char name[ROUTE_NAME_MAX_LEN]; // ASCII, zero-padded, not necessarily NUL-terminated if it fills the buffer
  uint16_t inPorts;   // PORT_* inputs, used when flags bit6 is set
  uint16_t outPorts;  // PORT_* outputs, used when flags bit6 is set
  // Protocol 4. Zero means off in every field.
  uint16_t features;  // FEATURE_* switches, see the feature blocks
  uint8_t arpSlot;    // 0 none, 1-4 arp slot
  uint8_t scaleLayout; // LAYOUT_* for a Scale route. Key and scale are global, the Layout is per route
  uint16_t chordMask; // Auto Chord positions: bit k = scale position k+1 on a Scale route, else k semitones
  uint8_t spare[2];   // room for later route features without changing the wire format again
};

struct GlobalSettings {
  uint8_t mode;
  uint8_t buttonAction;
  uint8_t buttonParamA;
  uint8_t buttonParamB;
  uint8_t buttonToggleMomentary;
  uint8_t ledBrightnessStep;
  // Global clock (protocol 3). Zero means the default in every field, see effectiveBpm() and friends,
  // so saves from before these fields existed load as defaults.
  uint8_t clockSource;  // 0 = the FoxWizard is master, 1-7 = follow port bit 0-6 (TRS/DIN ... TRS 2)
  uint16_t bpm;         // 20-300, 0 = 120
  uint8_t beatsPerBar;  // time signature top, 0 = 4
  uint8_t beatUnit;     // time signature bottom (2, 4, 8, 16), 0 = 4
  uint8_t clockOut;     // 1 = send MIDI clock to every port while master
  uint8_t tapType;      // tap tempo trigger: 0 none, 1 note, 2 CC
  uint8_t tapChannel;   // 0-15, 16 = any channel
  uint8_t tapNumber;    // note or CC number
};
#pragma pack(pop)

static_assert(sizeof(RouteData) == ROUTE_RAW_BYTES, "RouteData must stay wire-compatible");
static_assert(sizeof(GlobalSettings) == 15, "GlobalSettings layout is saved to flash, append only");
constexpr uint8_t GLOBAL_WIRE_BYTES = 15; // GLOBAL_DATA/SET_GLOBAL payload, bpm sent as two 7-bit bytes

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
  uint8_t cmdBuf[160]; // largest is FW_DATA: cmd(1) + index(2) + pack7(FW_CHUNK_BYTES=128) which packs to 147 bytes = 150 needed
  uint8_t cmdLen = 0;
  uint16_t fwdPorts = 0;
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
SerialPIO linkSerial(PIN_LINK_TX, PIN_LINK_RX, LINK_RX_BUFFER);

RouteData routes[MAX_ROUTES];
uint16_t routeCount = 0;
GlobalSettings globalSettings;

ActivityLed ledOut = {PIN_LED_OUT, LED_OUT_MAX, LED_OUT_MAX};
ActivityLed ledUsb = {PIN_LED_USB, LED_USB_MAX, LED_USB_MAX};
ActivityLed ledIn = {PIN_LED_IN, LED_IN_MAX, LED_IN_MAX};

PendingMsg pending[PORT_COUNT][MAX_PENDING]; // one dedup table per output port
uint8_t pendingCount[PORT_COUNT] = {0};

SysexState dinSysex;
SysexState usbSysex;
UsbOutSysexAccum usbOutFrom[SOURCE_COUNT];  // per source, so two SysEx streams never mix
UsbOutSysexAccum linkOutFrom[SOURCE_COUNT];

MidiQueue<LinkFrame, 64, 0> linkRtQueue;         // realtime and control frames, always sent first
MidiQueue<LinkFrame, 1024, 64> linkNormalQueue;  // everything else, waits here while the host brain pauses us
LinkFrame linkTxFrame;
uint8_t linkTxPos = 5; // 5 = no frame in progress
LinkFrameReader linkReader;
bool hostAskedPause = false;
uint32_t hostFlowMs = 0;
bool pausingHost = false;
uint32_t lastFlowSentMs = 0;
SysexState hostSysex[LINK_PORT_COUNT]; // per host port, so two devices' SysEx never mix on input
uint8_t currentHostPort = 1;             // hub port (1-4) of the USB Host message being handled
uint8_t hostPortMask = 0;                // bit n = USB MIDI device mounted on hub port n+1
uint8_t hostFwVersion = 0;
uint8_t hostDropCount = 0;               // host brain's dropped-message count, saturates at 127
uint8_t hostRestartReason = 0;           // RESTART_* from the host brain's last start
uint8_t hostRestartPort = 0;             // hub port, for RESTART_WEDGE
uint8_t hostRestartCount = 0;            // recovery restarts since the host brain powered up, saturates at 127
bool hostRestartSeen = false;            // hostRestartCount holds a real value
bool hostRestarted = false;              // set when the host brain restarts, handled in the loop

// Stuck-note guard. Notes held from USB Host and TRS 2 inputs, by original channel and note: 0 = not
// held, 1-4 = the USB Host port, 0x80 = TRS 2. If the host brain restarts or disappears, their Note
// Offs never arrive, so the main brain releases them itself, through the same routes.
uint8_t sidecarHeld[16][128];
bool hostWasPresent = false;
bool hostEverPresent = false; // the first time the host brain appears isn't a return
uint32_t hostLastStatusMs = 0;

DinParser dinParser;

uint8_t replyBuf[160];

// ============================================================================================
// Feature blocks: settings shared by the route(s) that switch a feature on, saved after the routes.
// Each block has a type and a fixed size. A route switches a feature on with a FEATURE_* bit.
// ============================================================================================
constexpr uint16_t FEATURE_NOTE_TO_CC = 1u << 0; // only the first route with it on uses it
constexpr uint16_t FEATURE_LAUNCHPAD = 1u << 1;  // same
constexpr uint16_t FEATURE_SCALE = 1u << 2;      // same
constexpr uint16_t FEATURE_AUTO_CHORD = 1u << 3; // same

constexpr uint8_t FEATURE_TYPE_NOTE_TO_CC = 1;
constexpr uint8_t FEATURE_TYPE_LAUNCHPAD = 2;
constexpr uint8_t FEATURE_TYPE_SCALE = 3;
constexpr uint8_t FEATURE_TYPE_ARP_1 = 4; // arp slots 1-4 are types 4-7
constexpr uint8_t ARP_SLOTS = 4;
constexpr uint8_t FEATURE_TYPE_LAST = FEATURE_TYPE_ARP_1 + ARP_SLOTS - 1;

// Note to CC. note is stored +1 so zero means an unused entry.
constexpr uint8_t NOTE_CC_ENTRIES = 16;
enum : uint8_t { NOTE_CC_MOMENTARY = 0, NOTE_CC_VELOCITY = 1, NOTE_CC_TOGGLE = 2 };
#pragma pack(push, 1)
struct NoteCcEntry {
  uint8_t notePlusOne; // 0 unused, else input note + 1
  uint8_t cc;
  uint8_t mode;        // NOTE_CC_*
};
#pragma pack(pop)
NoteCcEntry noteCcEntries[NOTE_CC_ENTRIES];
bool noteCcToggleState[NOTE_CC_ENTRIES];

// Launchpad: the 16 round buttons, top row left to right (CC 104-111) then the side column top to
// bottom (X-Y notes 8, 24 ... 120). Each sends a note or CC on the route's outputs.
constexpr uint8_t LAUNCHPAD_BUTTONS = 16;
enum : uint8_t { LP_BUTTON_OFF = 0, LP_BUTTON_NOTE = 1, LP_BUTTON_CC = 2 };
#pragma pack(push, 1)
struct LaunchpadButton {
  uint8_t type;   // LP_BUTTON_*
  uint8_t number; // note or CC number
};
#pragma pack(pop)
LaunchpadButton launchpadButtons[LAUNCHPAD_BUTTONS];
constexpr uint8_t LP_NOT_GRID = 0xFF; // not a grid pad: a side button, or outside the grid

// Scale: the global key (the yellow Scale row) and the Scale route's Layout. Zero is C Major, Next
// Note Up, no chord.
enum : uint8_t { LAYOUT_NEXT_UP = 0, LAYOUT_WHITE_KEYS = 1, LAYOUT_PAD_5THS = 2, LAYOUT_PAD_4THS = 3, LAYOUT_PAD_3RDS = 4 };
#pragma pack(push, 1)
struct ScaleSettings {
  uint8_t root;      // 0-11, C to B
  uint8_t scaleType; // index into SCALES
  uint8_t layout;    // LAYOUT_*
  uint16_t chordMask; // Auto Chord positions: bit k = scale position k+1 (Scale on) or k semitones (off)
  uint8_t spare[3];
};
#pragma pack(pop)
struct ScaleDef { uint8_t size; uint8_t steps[7]; }; // semitones above the root
ScaleSettings scaleSettings;

// Arp: up to 4 slots, each used by the one route whose arpSlot points at it. Zero means the default
// in every field: Up, 1/16, 1 octave, 50% gate, played velocity, no hold.
// Modes 0-5 are ARPnMIDI's classic modes (its TRIGGER is Drum Roll/Chord Pulse here), 6-12 its
// 16-step patterns.
enum : uint8_t {
  ARP_UP = 0, ARP_DOWN, ARP_UPDOWN1, ARP_UPDOWN2, ARP_DRUM, ARP_RANDOM,
  ARP_PAT_UP_4STEP, ARP_PAT_RHYTHM, ARP_PAT_OSTINATO, ARP_PAT_OCTAVE_WALK, ARP_PAT_FIFTH, ARP_PAT_BASS_CHORD, ARP_PAT_CHORD_RUN,
  // 1.20. As Played: the order the notes were pressed. Converge: outside in. Diverge: inside out.
  // Thumb: the lowest note between each of the others. Pinky: the highest between each of the others.
  ARP_AS_PLAYED, ARP_CONVERGE, ARP_DIVERGE, ARP_THUMB, ARP_PINKY,
  ARP_MODE_COUNT
};
constexpr uint8_t ARP_SQUARES = 7; // division table squares besides the default one
#pragma pack(push, 1)
struct ArpSquare {
  uint8_t divisionPlus1; // fw::MusicalDivision + 1, 0 = square not used
  uint8_t triggerType;   // 0 none, 1 note, 2 CC (64 and up). Held, it plays this division
  uint8_t triggerChannel; // 0-15, 16 = any
  uint8_t triggerNumber;
};
struct ArpSettings {
  uint8_t mode;          // ARP_*
  uint8_t divisionPlus1; // fw::MusicalDivision + 1, 0 = 1/16
  uint8_t octaves;       // 1-4, 0 = 1
  uint8_t gatePercent;   // 5-100, 0 = 50
  uint8_t velocity;      // 1-127 fixed, 0 = each note's own velocity
  uint8_t holdType;      // 0 none, 2 CC (64 and up holds, like a sustain pedal)
  uint8_t holdChannel;   // 0-15, 16 = any
  uint8_t holdNumber;    // CC number
  uint8_t aftertouchVelocity; // 1 = velocity follows channel pressure, ARPnMIDI's drum roll option
  uint8_t bypassChannel; // 0 off, 1-16: the held notes also play unarped on this channel
  uint8_t bassChannel;   // 0 off, 1-16: the lowest held note plays an octave down here, retriggering
  uint8_t spare[5];
  ArpSquare squares[ARP_SQUARES];
  uint8_t spare2[4];
};
#pragma pack(pop)
ArpSettings arpSettings[ARP_SLOTS];

constexpr uint8_t ARP_MAX_OUT = 48;        // drum mode: notes x octaves at once

struct ArpRuntime {
  bool physical[128];    // keys held right now
  bool latched[128];     // notes kept by hold
  uint8_t velocity[128];
  uint16_t pressOrder[128]; // when each note was pressed, for As Played
  uint16_t pressCount;
  uint8_t physicalCount;
  bool hold;
  bool running;
  uint16_t routeIndex;
  uint64_t pendingStartUs; // 0 = no fresh start waiting
  uint32_t nextStepPulse;
  uint32_t sequenceStep;
  uint8_t patternStep;   // 0-15 through a pattern
  uint8_t active[ARP_MAX_OUT];
  uint8_t activeCount;
  uint64_t gateOffUs;    // 0 = nothing to release
  uint8_t moment[ARP_SQUARES]; // division squares held, last pressed on top
  uint8_t momentCount;
  uint8_t pressure;      // last channel pressure, for aftertouch velocity
  uint8_t sideOn[128];   // bypass channel + 1 each note is sounding on, 0 none
  uint8_t bassNote;      // bass note + 1 sounding, 0 none
  uint8_t bassCh;        // the channel it's on
};
ArpRuntime arps[ARP_SLOTS];
// Scale and Auto Chord: the notes each held input note plays, so its Note Off releases exactly
// those, and how many held notes share each output note, so releasing one doesn't cut another.
constexpr uint8_t CHORD_MAX = 12;
// One shared pool of held keys for every Scale and Auto Chord route, sized by keys held at once, not
// by routes. Whether an output note still sounds is counted from the pool.
constexpr uint8_t HELD_POOL = 128;
struct HeldKey {
  bool used;
  uint8_t route;
  uint8_t in;
  uint8_t count;
  uint8_t out[CHORD_MAX];
};
HeldKey heldPool[HELD_POOL];

fw::ClockEngine clockEngine; // ported from ARPnMIDI, see src/clock_engine.h

// TRS/DIN output queue. Bytes wait here instead of the loop waiting on the 31,250-baud UART, so
// timing work never stalls. Realtime bytes have their own lane and go first, MIDI allows them
// between any two bytes. Inputs pause while the queue is nearly full, so nothing is dropped.
constexpr uint16_t TRS_QUEUE_BYTES = 2048;
constexpr uint16_t TRS_INPUT_HEADROOM = 512;
constexpr uint8_t TRS_REALTIME_BYTES = 64;
uint8_t trsQueue[TRS_QUEUE_BYTES];
uint16_t trsHead = 0;
uint16_t trsTail = 0;
uint8_t trsRealtime[TRS_REALTIME_BYTES];
uint8_t trsRtHead = 0;
uint8_t trsRtTail = 0;
uint32_t trsDropped = 0;

// USB Device output queue. TinyUSB's own USB MIDI buffer holds only 16 packets and drops what doesn't
// fit, which cut long SysEx replies (a 53-byte route is 23 packets) and bursts like a note cloned to
// 16 channels. Packets wait here instead. Realtime has its own lane and goes first.
constexpr uint16_t USB_QUEUE_PACKETS = 512;
constexpr uint16_t USB_INPUT_HEADROOM = 128;
constexpr uint8_t USB_REALTIME_PACKETS = 32;
uint8_t usbQueue[USB_QUEUE_PACKETS][4];
uint16_t usbHead = 0;
uint16_t usbTail = 0;
uint8_t usbRealtime[USB_REALTIME_PACKETS][4];
uint8_t usbRtHead = 0;
uint8_t usbRtTail = 0;
uint32_t usbDropped = 0;

// Learn (CMD_LEARN): the next Note On or CC from any input is sent back to whoever asked.
constexpr uint32_t LEARN_TIMEOUT_MS = 10000;
bool learnActive = false;
uint8_t learnReplyPort = DEV_USB;
uint32_t learnUntilMs = 0;

// Tap tempo: the average of the last few taps. A gap longer than TAP_RESET_MS starts over.
constexpr uint32_t TAP_RESET_MS = 2000;
constexpr uint8_t TAP_HISTORY = 5;
uint32_t tapTimesMs[TAP_HISTORY];
uint8_t tapCount = 0;

// Firmware update over SysEx, see ../SYSEX_PROTOCOL.md. fwNextChunk is the FW_DATA index expected
// next. fwLastChecksum lets a repeated chunk (its ACK was lost and the host retried) be confirmed
// again without writing it twice.
uint16_t fwNextChunk = 0;
uint8_t fwLastChecksum = 0;
bool fwRebootPending = false;
uint32_t fwRebootAtMs = 0;
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
NoteHoldStack noteStacks[4][16]; // [TRS/DIN, USB Device, USB Host, TRS 2][channel]

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

NoteHoldStack &noteStackFor(uint8_t inputDevice, uint8_t channel) {
  return noteStacks[inputDevice == DEV_USB ? 1 : inputDevice == DEV_HOST ? 2 : inputDevice == DEV_TRS2 ? 3 : 0][channel];
}

// The port a message being handled came in on. USB Host uses currentHostPort.
uint16_t sourcePort(uint8_t inputDevice) {
  switch (inputDevice) {
    case DEV_USB: return PORT_USB;
    case DEV_HOST: return PORT_HOST1 << (currentHostPort - 1);
    case DEV_TRS2: return PORT_TRS2;
    default: return PORT_TRS;
  }
}

// Routes saved before the sidecar existed have flags bit6 clear and keep their old meaning.
uint16_t routeInputPorts(const RouteData &r) {
  if (r.flags & 0x40) return r.inPorts;
  switch (r.flags & 0x03) {
    case DEV_TRS: return PORT_TRS;
    case DEV_USB: return PORT_USB;
    default: return PORT_TRS | PORT_USB;
  }
}

uint16_t routeOutputPorts(const RouteData &r) {
  return (r.flags & 0x40) ? r.outPorts : (uint16_t)(r.outDevice & (PORT_TRS | PORT_USB));
}

// This whole note-hold-stack machinery only matters to routes with poly aftertouch mapped to CC —
// a rare, minor feature. Rather than paying its (small but nonzero) per-Note-On/Off/aftertouch-
// message bookkeeping cost on every single MIDI event regardless of whether anything actually uses
// it, this cached flag gates that work entirely. It's only recomputed when the route list actually
// changes (add/set/delete/clear/load-from-flash) — never per MIDI event, since a per-event scan of
// every route would cost more than the bookkeeping it's meant to save.
HeldKey *findHeldKey(uint8_t route, uint8_t in) {
  for (uint8_t k = 0; k < HELD_POOL; ++k) {
    if (heldPool[k].used && heldPool[k].route == route && heldPool[k].in == in) return &heldPool[k];
  }
  return nullptr;
}

// Whether another held key on this route still plays this output note.
// Remembers what a key played. With the pool full the key still plays, its Note Off then finds
// nothing, and the stuck-note guard is the backstop. 128 keys held at once makes that unlikely.
void holdKey(uint8_t route, uint8_t in, const uint8_t *outs, uint8_t count) {
  for (uint8_t k = 0; k < HELD_POOL; ++k) {
    if (heldPool[k].used) continue;
    heldPool[k].used = true;
    heldPool[k].route = route;
    heldPool[k].in = in;
    heldPool[k].count = count;
    memcpy(heldPool[k].out, outs, count);
    return;
  }
}

bool heldOutSounding(uint8_t route, uint8_t note) {
  for (uint8_t k = 0; k < HELD_POOL; ++k) {
    const HeldKey &h = heldPool[k];
    if (!h.used || h.route != route) continue;
    for (uint8_t j = 0; j < h.count; ++j) if (h.out[j] == note) return true;
  }
  return false;
}

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
  memset(heldPool, 0, sizeof(heldPool));
  arpForgetAll(); // the arps point at routes by index, which a route change can move
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
// Launchpad echo: a Launchpad route sends each message from the Launchpad straight back to its port,
// unchanged, so the pressed pad or button lights. Then that port is left out of everything the route
// sends for that message, in every mode, so the remapped note never reaches it too.
uint16_t launchpadExcludePorts = 0;

uint16_t routeOutputsNow(const RouteData &r) {
  return routeOutputPorts(r) & ~launchpadExcludePorts;
}

void sendRouteNote(uint16_t routeIndex, const RouteData &r, bool on, uint8_t note, uint8_t velocity) {
  if (routeOutputPorts(r) == 0) return;
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
      upsertPendingMask(routeOutputsNow(r), msg);
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
  upsertPendingMask(routeOutputsNow(r), msg);
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
  globalSettings.clockSource = 0;
  globalSettings.bpm = 120;
  globalSettings.beatsPerBar = 4;
  globalSettings.beatUnit = 4;
  globalSettings.clockOut = 0;
  globalSettings.tapType = 0;
  globalSettings.tapChannel = 16;
  globalSettings.tapNumber = 0;
  routeCount = 0;
  resetFeatureBlocks();
  anyAftertouchMapRoutes = false; // no routes left, so trivially true without needing a scan
  resetAllRouteNoteTracks();
}

// Saved settings layout "FOX2": magic(4), globalBytes(1), routeBytes(1), global, routeCount(2),
// routes. The stored sizes let a later firmware with bigger structs read an older save: it copies
// the bytes that were saved and leaves the rest zero. So new fields go at the end of their struct,
// and zero must mean off. "FOX1" (firmware 1.0) is the same layout without the two size bytes,
// with a 6-byte global and 41-byte routes.
constexpr uint32_t EEPROM_MAGIC_V1 = 0x464F5831; // "FOX1"
constexpr uint32_t EEPROM_MAGIC_V2 = 0x464F5832; // "FOX2"
constexpr size_t EEPROM_SIZE = 4096;             // the core's EEPROM maximum
// Feature blocks after the routes: marker (2), each block's type and size (2) and data, end byte (1).
constexpr size_t FEATURE_BLOCKS_BYTES = 2 + (2 + sizeof(NoteCcEntry) * NOTE_CC_ENTRIES) + (2 + sizeof(LaunchpadButton) * LAUNCHPAD_BUTTONS) + (2 + sizeof(ScaleSettings)) + ARP_SLOTS * (2 + sizeof(ArpSettings)) + 1;
static_assert(4 + 2 + sizeof(GlobalSettings) + 2 + MAX_ROUTES * sizeof(RouteData) + FEATURE_BLOCKS_BYTES <= EEPROM_SIZE,
              "saved settings must fit the EEPROM area, lower MAX_ROUTES if routes or feature blocks grow");

void eepromReadBytes(size_t addr, void *dst, size_t len) {
  uint8_t *d = (uint8_t *)dst;
  for (size_t i = 0; i < len; ++i) d[i] = EEPROM.read(addr + i);
}

void eepromWriteBytes(size_t addr, const void *src, size_t len) {
  const uint8_t *s = (const uint8_t *)src;
  for (size_t i = 0; i < len; ++i) EEPROM.write(addr + i, s[i]);
}

void loadFromFlash() {
  EEPROM.begin(EEPROM_SIZE);
  uint32_t magic = 0;
  eepromReadBytes(0, &magic, sizeof(magic));
  size_t addr = 4;
  uint8_t globalBytes = 6;
  uint8_t routeBytes = 41;
  if (magic == EEPROM_MAGIC_V2) {
    globalBytes = EEPROM.read(4);
    routeBytes = EEPROM.read(5);
    addr = 6;
  } else if (magic != EEPROM_MAGIC_V1) {
    setFactoryDefaults();
    return;
  }
  if (globalBytes == 0 || routeBytes == 0) {
    setFactoryDefaults();
    return;
  }

  memset(&globalSettings, 0, sizeof(globalSettings));
  eepromReadBytes(addr, &globalSettings, min((size_t)globalBytes, sizeof(GlobalSettings)));
  addr += globalBytes;

  uint16_t storedCount = 0;
  eepromReadBytes(addr, &storedCount, sizeof(storedCount));
  addr += sizeof(storedCount);
  uint16_t fits = (addr < EEPROM_SIZE) ? (uint16_t)((EEPROM_SIZE - addr) / routeBytes) : 0;
  routeCount = min(storedCount, min(MAX_ROUTES, fits));
  for (uint16_t i = 0; i < routeCount; ++i) {
    memset(&routes[i], 0, sizeof(RouteData));
    eepromReadBytes(addr, &routes[i], min((size_t)routeBytes, sizeof(RouteData)));
    addr += routeBytes;
  }
  loadFeatureBlocks(addr);
  // Saves from 1.12-1.14 kept one chord in the Scale block, it moves to the Auto Chord route.
  if (scaleSettings.chordMask) {
    for (uint16_t i = 0; i < routeCount; ++i) {
      if ((routes[i].features & FEATURE_AUTO_CHORD) && !routes[i].chordMask) routes[i].chordMask = scaleSettings.chordMask;
    }
    scaleSettings.chordMask = 0;
  }
  // Saves from 1.11-1.13 kept one Layout in the Scale block, it moves to the Scale route.
  if (scaleSettings.layout) {
    for (uint16_t i = 0; i < routeCount; ++i) {
      if ((routes[i].features & FEATURE_SCALE) && !routes[i].scaleLayout) routes[i].scaleLayout = scaleSettings.layout;
    }
    scaleSettings.layout = 0;
  }
  recomputeAnyAftertouchMapRoutes();
  resetAllRouteNoteTracks();
}

// Feature blocks follow the routes: "FX" then (type, size, data) records, ending with type 0. A save
// without the marker (older firmware) leaves every feature at its default. A block saved by a
// newer firmware with more bytes is cut to what this one knows, a shorter one is zero-filled.
constexpr uint8_t FEATURE_MARKER[2] = {'F', 'X'};

uint8_t *featureBlockData(uint8_t type, uint8_t &size) {
  if (type >= FEATURE_TYPE_ARP_1 && type < FEATURE_TYPE_ARP_1 + ARP_SLOTS) { size = sizeof(ArpSettings); return (uint8_t *)&arpSettings[type - FEATURE_TYPE_ARP_1]; }
  if (type == FEATURE_TYPE_SCALE) { size = sizeof(scaleSettings); return (uint8_t *)&scaleSettings; }
  if (type == FEATURE_TYPE_NOTE_TO_CC) { size = sizeof(noteCcEntries); return (uint8_t *)noteCcEntries; }
  if (type == FEATURE_TYPE_LAUNCHPAD) { size = sizeof(launchpadButtons); return (uint8_t *)launchpadButtons; }
  size = 0;
  return nullptr;
}

void resetFeatureBlocks() {
  memset(noteCcEntries, 0, sizeof(noteCcEntries));
  memset(noteCcToggleState, 0, sizeof(noteCcToggleState));
  memset(launchpadButtons, 0, sizeof(launchpadButtons));
  memset(&scaleSettings, 0, sizeof(scaleSettings));
  memset(arpSettings, 0, sizeof(arpSettings));
}

void loadFeatureBlocks(size_t addr) {
  resetFeatureBlocks();
  if (addr + 3 > EEPROM_SIZE || EEPROM.read(addr) != FEATURE_MARKER[0] || EEPROM.read(addr + 1) != FEATURE_MARKER[1]) return;
  addr += 2;
  while (addr + 2 <= EEPROM_SIZE) {
    const uint8_t type = EEPROM.read(addr);
    const uint8_t stored = EEPROM.read(addr + 1);
    if (type == 0 || addr + 2 + stored > EEPROM_SIZE) return;
    uint8_t size;
    uint8_t *data = featureBlockData(type, size);
    if (data) eepromReadBytes(addr + 2, data, min(size, stored));
    addr += 2 + stored;
  }
}

size_t saveFeatureBlocks(size_t addr) {
  EEPROM.write(addr++, FEATURE_MARKER[0]);
  EEPROM.write(addr++, FEATURE_MARKER[1]);
  for (uint8_t type = 1; type <= FEATURE_TYPE_LAST; ++type) {
    uint8_t size;
    const uint8_t *data = featureBlockData(type, size);
    if (!data) continue;
    EEPROM.write(addr++, type);
    EEPROM.write(addr++, size);
    eepromWriteBytes(addr, data, size);
    addr += size;
  }
  EEPROM.write(addr++, 0);
  return addr;
}

void saveToFlash() {
  size_t addr = 0;
  const uint32_t magic = EEPROM_MAGIC_V2;
  eepromWriteBytes(addr, &magic, sizeof(magic));
  addr += sizeof(magic);
  EEPROM.write(addr++, (uint8_t)sizeof(GlobalSettings));
  EEPROM.write(addr++, (uint8_t)sizeof(RouteData));
  eepromWriteBytes(addr, &globalSettings, sizeof(GlobalSettings));
  addr += sizeof(GlobalSettings);
  eepromWriteBytes(addr, &routeCount, sizeof(routeCount));
  addr += sizeof(routeCount);
  for (uint16_t i = 0; i < routeCount; ++i) {
    eepromWriteBytes(addr, &routes[i], sizeof(RouteData));
    addr += sizeof(RouteData);
  }
  saveFeatureBlocks(addr);
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
uint8_t usbCinFor(uint8_t statusHi, uint8_t len) {
  switch (statusHi) {
    case 0x80: return 0x08;
    case 0x90: return 0x09;
    case 0xA0: return 0x0A;
    case 0xB0: return 0x0B;
    case 0xC0: return 0x0C;
    case 0xD0: return 0x0D;
    case 0xE0: return 0x0E;
    // Falls through for System Common/Realtime (statusHi >= 0xF0, no channel nibble to speak of).
    // 0x05 = Single Byte (realtime, or a 1-byte common message like Tune Request). 0x02/0x03 = 2/3-
    // byte System Common (MTC Quarter Frame, Song Select, Song Position Pointer) per the USB-MIDI
    // spec — NOT 0x06/0x04, which are Sysex-continuation CINs and would missort these into whatever
    // sysex stream happens to be in flight on the receiving end.
    default: return (len == 1) ? 0x05 : ((len == 2) ? 0x02 : 0x03);
  }
}

// ============================================================================================
// Host link: 5-byte frames carrying one USB-MIDI packet plus a port number, see ../README.md.
// Realtime has its own queue so clock never waits behind SysEx. Nothing here ever waits on the
// host brain: full queues drop, and the PIO FIFO is only written while it has room.
// ============================================================================================
// Ordinary frames keep the last 64 slots for Note Offs and the other critical messages in
// foxwizard_link.h, same rule as ARPnMIDI's secondary link. Main brain output to the link is paced
// by its own DIN OUT, so the queue only fills if the host brain holds a pause for a long time.
// The host brain sends a port map frame every 500 ms. Without one recently, there is no host
// brain (units sold without it), and the link is ignored in both directions.
bool hostPresent() {
  return hostLastStatusMs != 0 && millis() - hostLastStatusMs < HOST_PRESENT_MS;
}

void linkSend(uint8_t port, const uint8_t pkt[4]) {
  if (port != LINK_PORT_CONTROL && !hostPresent()) return;
  LinkFrame f;
  encodeLinkFrame(port, pkt, f);
  if (port == LINK_PORT_CONTROL || isRealtimePacket(pkt)) linkRtQueue.push(f, true);
  else linkNormalQueue.push(f, isCriticalPacket(pkt));
}

bool hostPaused() {
  return hostAskedPause && millis() - hostFlowMs < FLOW_EXPIRY_MS;
}

void pumpLinkOut() {
  while (true) {
    if (linkTxPos >= 5) {
      LinkFrame *next = linkRtQueue.peek();
      if (next) {
        linkTxFrame = *next;
        linkRtQueue.pop();
      } else if (!hostPaused() && (next = linkNormalQueue.peek()) != nullptr) {
        linkTxFrame = *next;
        linkNormalQueue.pop();
      } else {
        return;
      }
      linkTxPos = 0;
    }
    while (linkTxPos < 5 && linkSerial.availableForWrite() > 0) linkSerial.write(linkTxFrame.b[linkTxPos++]);
    if (linkTxPos < 5) return; // PIO FIFO full, carry on next loop
  }
}

// Asks the host brain to pause while received bytes pile up faster than the loop parses them.
void serviceLinkFlow() {
  const int waiting = linkSerial.available();
  bool wantPause = pausingHost ? waiting > LINK_RX_RESUME_AT : waiting > LINK_RX_PAUSE_AT;
  if (wantPause == pausingHost && millis() - lastFlowSentMs < FLOW_REPEAT_MS) return;
  pausingHost = wantPause;
  uint8_t pkt[4] = {CONTROL_FLOW, (uint8_t)(wantPause ? 1 : 0), 0, 0};
  linkSend(LINK_PORT_CONTROL, pkt);
  lastFlowSentMs = millis();
}

// ============================================================================================
// TRS/DIN output queue
// ============================================================================================
uint16_t trsFree() {
  return (uint16_t)((trsTail + TRS_QUEUE_BYTES - trsHead - 1) % TRS_QUEUE_BYTES);
}

void trsSend(const uint8_t *bytes, uint8_t len) {
  if (len == 1 && bytes[0] >= 0xF8) {
    const uint8_t next = (trsRtHead + 1) % TRS_REALTIME_BYTES;
    if (next == trsRtTail) { ++trsDropped; return; }
    trsRealtime[trsRtHead] = bytes[0];
    trsRtHead = next;
    return;
  }
  if (trsFree() < len) { ++trsDropped; return; } // only if inputs ignored the headroom, whole message dropped
  for (uint8_t i = 0; i < len; ++i) {
    trsQueue[trsHead] = bytes[i];
    trsHead = (trsHead + 1) % TRS_QUEUE_BYTES;
  }
}

void pumpTrsOut() {
  while (Serial1.availableForWrite() > 0) {
    if (trsRtTail != trsRtHead) {
      Serial1.write(trsRealtime[trsRtTail]);
      trsRtTail = (trsRtTail + 1) % TRS_REALTIME_BYTES;
    } else if (trsTail != trsHead) {
      Serial1.write(trsQueue[trsTail]);
      trsTail = (trsTail + 1) % TRS_QUEUE_BYTES;
    } else {
      break;
    }
  }
}

uint16_t usbQueueFree() {
  return (uint16_t)((usbTail + USB_QUEUE_PACKETS - usbHead - 1) % USB_QUEUE_PACKETS);
}

void pumpUsbOut() {
  while (usbRtTail != usbRtHead && usb_midi.writePacket(usbRealtime[usbRtTail])) usbRtTail = (usbRtTail + 1) % USB_REALTIME_PACKETS;
  while (usbTail != usbHead && usb_midi.writePacket(usbQueue[usbTail])) usbTail = (usbTail + 1) % USB_QUEUE_PACKETS;
}

void usbSend(const uint8_t packet[4]) {
  const bool realtime = ((packet[0] & 0x0F) == 0x05 || (packet[0] & 0x0F) == 0x0F) && packet[1] >= 0xF8;
  if (realtime) {
    const uint8_t next = (usbRtHead + 1) % USB_REALTIME_PACKETS;
    if (next == usbRtTail) { ++usbDropped; return; }
    memcpy(usbRealtime[usbRtHead], packet, 4);
    usbRtHead = next;
  } else {
    const uint16_t next = (usbHead + 1) % USB_QUEUE_PACKETS;
    if (next == usbTail) { ++usbDropped; return; } // only if inputs ignored the headroom
    memcpy(usbQueue[usbHead], packet, 4);
    usbHead = next;
  }
  pumpUsbOut(); // straight out when TinyUSB has room, so nothing waits for the next loop
}

// Inputs are read only while both output queues have room, so a slow output holds the inputs back
// instead of losing anything.
bool outputsHaveRoom() {
  return trsFree() >= TRS_INPUT_HEADROOM && usbQueueFree() >= USB_INPUT_HEADROOM;
}

void drainTrsOut() {
  while (trsTail != trsHead || trsRtTail != trsRtHead) pumpTrsOut();
}

// ============================================================================================
// Global clock, tap tempo, learn
// The BPM row's settings only apply while something uses the clock. Until then clock routes
// exactly as before.
// ============================================================================================
bool anyArpRoute() {
  for (uint16_t i = 0; i < routeCount; ++i) if (routes[i].arpSlot) return true;
  return false;
}

bool globalClockActive() {
  return globalSettings.buttonAction == BTN_TAP_TEMPO || anyArpRoute(); // later: looper, echo, stutter
}

uint16_t effectiveBpm() {
  const uint16_t bpm = globalSettings.bpm;
  if (bpm == 0) return 120;
  return bpm < 20 ? 20 : (bpm > 300 ? 300 : bpm);
}

uint16_t clockFollowPort() {
  const uint8_t src = globalSettings.clockSource;
  return (src >= 1 && src <= 7) ? (uint16_t)(1u << (src - 1)) : 0;
}

fw::ClockConfig clockConfig() {
  fw::ClockConfig c;
  c.followExternal = globalClockActive() && clockFollowPort() != 0;
  // As master the engine always counts pulses while the clock is in use, the arps step on them.
  // MIDI clock only goes out with Send Clock on, see serviceClock().
  c.sendClock = globalClockActive();
  c.beatsPerBar = globalSettings.beatsPerBar ? globalSettings.beatsPerBar : 4;
  c.beatUnit = globalSettings.beatUnit ? globalSettings.beatUnit : 4;
  c.manualBpm = effectiveBpm();
  return c;
}

void applyClockConfig() {
  clockEngine.setConfig(clockConfig(), time_us_64());
}

// For GET_STATUS: 0 clock not in use, 1 master, 2 following with clock arriving, 3 following, no clock.
uint8_t clockStatus() {
  if (!globalClockActive()) return 0;
  if (!clockFollowPort()) return 1;
  return clockEngine.externalClockPresent(time_us_64()) ? 2 : 3;
}

// Clock, Start, Continue, Stop. While the clock is in use, only the followed port's clock gets
// through (and drives the engine). Every other port's clock is discarded. Transport still routes.
bool acceptClockMessage(uint8_t inputDevice, uint8_t status) {
  if (status != 0xF8 && status != 0xFA && status != 0xFB && status != 0xFC) return true;
  if (!globalClockActive()) return true;
  const uint16_t follow = clockFollowPort();
  const bool fromSource = follow && sourcePort(inputDevice) == follow;
  if (fromSource) clockEngine.receiveRealtime(status, time_us_64());
  return status != 0xF8 || fromSource;
}

// Master with Send Clock on: the FoxWizard's own MIDI clock goes to every port.
void serviceClock() {
  const uint64_t now = time_us_64();
  for (uint8_t guard = 0; guard < 4 && clockEngine.takeInternalClock(now); ++guard) {
    if (globalSettings.clockOut) sendToOutputs(availablePorts(), 0xF8, 0, 0, 0, 1);
  }
}

void tapTempo() {
  if (clockFollowPort()) return; // following another clock, its tempo wins
  const uint32_t now = millis();
  if (tapCount && now - tapTimesMs[tapCount - 1] > TAP_RESET_MS) tapCount = 0;
  if (tapCount == TAP_HISTORY) {
    for (uint8_t i = 1; i < TAP_HISTORY; ++i) tapTimesMs[i - 1] = tapTimesMs[i];
    --tapCount;
  }
  tapTimesMs[tapCount++] = now;
  noteActivity(ledOut);
  if (tapCount < 2) return;
  const uint32_t avgMs = (tapTimesMs[tapCount - 1] - tapTimesMs[0]) / (tapCount - 1);
  if (avgMs == 0) return;
  uint32_t bpm = (60000 + avgMs / 2) / avgMs;
  globalSettings.bpm = (uint16_t)(bpm < 20 ? 20 : (bpm > 300 ? 300 : bpm)); // kept until Save to Device
  applyClockConfig();
}

// Learn and the tap tempo trigger see notes and CCs before routing, and consume them.
bool captureLearnOrTap(uint8_t inputDevice, MsgKind kind, bool isNoteOn, uint8_t channel, uint8_t data1, uint8_t data2) {
  if (kind != MK_NOTE && kind != MK_CC) return false;
  if (learnActive && (isNoteOn || kind == MK_CC)) {
    learnActive = false;
    beginReply(CMD_LEARN_DATA);
    appendReply(kind == MK_NOTE ? 1 : 2);
    appendReply(channel);
    appendReply(data1 & 0x7F);
    appendReply((uint8_t)__builtin_ctz(sourcePort(inputDevice))); // port bit, 0 TRS/DIN ... 6 TRS 2
    sendReply(learnReplyPort);
    return true;
  }
  if (globalClockActive() && globalSettings.tapType) {
    const bool typeMatch = (globalSettings.tapType == 1 && kind == MK_NOTE) || (globalSettings.tapType == 2 && kind == MK_CC);
    const bool channelMatch = globalSettings.tapChannel > 15 || globalSettings.tapChannel == channel;
    if (typeMatch && channelMatch && data1 == globalSettings.tapNumber) {
      if ((kind == MK_NOTE && isNoteOn) || (kind == MK_CC && data2 > 0)) tapTempo();
      return true; // the trigger doesn't pass through, its Note Off and zero CCs included
    }
  }
  return false;
}

void serviceLearnTimeout() {
  if (learnActive && (int32_t)(millis() - learnUntilMs) >= 0) learnActive = false;
}

// ============================================================================================
// Outputs
// ============================================================================================
// Every port that exists right now. The sidecar's ports only while it's present.
uint16_t availablePorts() {
  return PORT_TRS | PORT_USB | (hostPresent() ? (PORT_HOST_ALL | PORT_TRS2_6) : 0);
}

// Link ports for an output mask: USB Host 1-4 are link ports 1-4 (only hub ports with a device
// mounted), TRS 2-6 are link ports 5-9.
uint8_t linkPortsFor(uint16_t mask, uint8_t ports[9]) {
  uint8_t n = 0;
  if (!hostPresent()) return 0;
  for (uint8_t p = 1; p <= 4; ++p) {
    if ((mask & (PORT_HOST1 << (p - 1))) && (hostPortMask & (1 << (p - 1)))) ports[n++] = p;
  }
  for (uint8_t i = 0; i < 5; ++i) {
    if (mask & (PORT_TRS2 << i)) ports[n++] = LINK_PORT_DIN + i;
  }
  return n;
}

void sendToOutputs(uint16_t mask, uint8_t statusHi, uint8_t channel, uint8_t data1, uint8_t data2, uint8_t len) {
  if (mask & PORT_TRS) {
    const uint8_t bytes[3] = {(uint8_t)(statusHi | channel), data1, data2};
    trsSend(bytes, len);
    noteActivity(ledOut);
  }
  uint8_t packet[4] = {usbCinFor(statusHi, len), (uint8_t)(statusHi | channel), len > 1 ? data1 : (uint8_t)0, len > 2 ? data2 : (uint8_t)0};
  if (mask & PORT_USB) {
    usbSend(packet);
    noteActivity(ledUsb);
  }
  uint8_t ports[9];
  uint8_t n = linkPortsFor(mask, ports);
  for (uint8_t i = 0; i < n; ++i) linkSend(ports[i], packet);
  if (n) noteActivity(ledOut);
}

void upsertPendingMask(uint16_t mask, const PendingMsg &msg) {
  for (uint8_t i = 0; i < PORT_COUNT; ++i) {
    if (mask & (1u << i)) upsertPending(pending[i], pendingCount[i], msg);
  }
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
  if (!(routeInputPorts(r) & sourcePort(inputDevice))) return false;

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
  if (routeOutputPorts(r) == 0) return; // enabled + matched, but deliberately silenced

  if (kind == MK_POLY_AT && (r.typeFlags & 0x80)) {
    // Only the top-of-stack ("priority") note may drive the mapped CC — see noteStacks above.
    // emitAftertouchMapUpdate() re-triggers this with the new top's own value whenever priority
    // shifts, so off-priority notes can just be dropped here without the mapped CC going stale.
    NoteHoldStack &s = noteStackFor(inputDevice, channel);
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

    upsertPendingMask(routeOutputPorts(r), msg);
  }
}

void flushPending() {
  for (uint8_t out = 0; out < PORT_COUNT; ++out) {
    for (uint8_t i = 0; i < pendingCount[out]; ++i) {
      PendingMsg &m = pending[out][i];
      sendToOutputs(1u << out, m.statusHi, m.channel, m.data1, m.data2, m.len);
    }
    pendingCount[out] = 0;
  }
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

// Where a message goes when no route takes it. USB Adaptor Mode: every port except the one it came
// in on, so nothing echoes back to where it came from. Router Mode: every port.
uint16_t modeDefaultPorts(uint8_t inputDevice) {
  uint16_t ports = availablePorts();
  if (globalSettings.mode == MODE_USB_ADAPTOR) ports &= ~sourcePort(inputDevice);
  return ports;
}

void applyModeDefault(uint8_t inputDevice, uint8_t statusHi, uint8_t channel, uint8_t data1, uint8_t data2, uint8_t len) {
  sendToOutputs(modeDefaultPorts(inputDevice), statusHi, channel, data1, data2, len);
}

// The single place that actually matches routes and transmits a Note On/Off (Mono Retrig/Round
// Robin included), used both for real incoming note events and for the synthetic kill+retrigger
// pair a mid-note octave shift change sends (see retriggerHeldNotesForShiftChange) — those need to
// go through exactly the same route matching/transpose/velocity-scale/fan-out as a real note would,
// not a simplified copy of it.
// Note to CC: a mapped note on a route with FEATURE_NOTE_TO_CC becomes a CC on that route's output
// channels and ports instead of a note. Returns true if it was mapped.
bool handleNoteToCc(const RouteData &r, bool isNoteOn, uint8_t note, uint8_t velocity) {
  if (!(r.features & FEATURE_NOTE_TO_CC)) return false;
  for (uint8_t e = 0; e < NOTE_CC_ENTRIES; ++e) {
    const NoteCcEntry &entry = noteCcEntries[e];
    if (entry.notePlusOne != note + 1) continue;
    uint8_t value;
    if (entry.mode == NOTE_CC_TOGGLE) {
      if (!isNoteOn) return true; // a toggle only acts on press
      noteCcToggleState[e] = !noteCcToggleState[e];
      value = noteCcToggleState[e] ? 127 : 0;
    } else if (entry.mode == NOTE_CC_VELOCITY) {
      value = isNoteOn ? velocity : 0;
    } else {
      value = isNoteOn ? 127 : 0;
    }
    const uint16_t outMask = r.outChLow | ((uint16_t)r.outChHigh << 8);
    for (uint8_t c = 0; c < 16; ++c) {
      if (!(outMask & (1u << c))) continue;
      PendingMsg msg;
      msg.used = true;
      msg.channel = c;
      msg.kind = MK_CC;
      msg.statusHi = 0xB0;
      msg.number = entry.cc;
      msg.data1 = entry.cc;
      msg.data2 = value;
      msg.len = 3;
      upsertPendingMask(routeOutputsNow(r), msg);
    }
    return true;
  }
  return false;
}

// ============================================================================================
// Launchpad. Older Launchpads (original, S, Mini mk1/mk2) send X-Y notes in their default mode:
// note = 16 x row + column, top-left 0, column 8 the round side buttons, the top round buttons
// CC 104-111. A route with FEATURE_LAUNCHPAD turns the grid into the Launchpad's own drum rack
// layout, four 4x4 blocks from note 36 (bottom-left 36-51, top-left 52-67, bottom-right 68-83,
// top-right 84-99), without sending it a mode change. Scale Layouts later see a normal drum pad.
// ============================================================================================
constexpr uint8_t LP_TOP_BUTTON_CC = 104;

// Grid note for an X-Y note, LP_NOT_GRID for a side button or anything outside the grid.
uint8_t launchpadGridNote(uint8_t xy) {
  const uint8_t row = xy >> 4, col = xy & 0x0F;
  if (row > 7 || col > 7) return LP_NOT_GRID;
  const uint8_t rowFromBottom = 7 - row;
  const uint8_t block = (col >= 4 ? 2 : 0) + (rowFromBottom >= 4 ? 1 : 0);
  return 36 + block * 16 + (rowFromBottom % 4) * 4 + (col % 4);
}

// Side button index 8-15 for an X-Y note, or 0xFF.
uint8_t launchpadSideButton(uint8_t xy) {
  const uint8_t row = xy >> 4, col = xy & 0x0F;
  return (row <= 7 && col == 8) ? 8 + row : 0xFF;
}

// Port and channel only: a Launchpad button works whatever data types the route lets through.
bool routeAcceptsSource(const RouteData &r, uint8_t inputDevice, uint8_t channel) {
  if (!(r.flags & 0x80)) return false;
  if (!(routeInputPorts(r) & sourcePort(inputDevice))) return false;
  const uint16_t mask = r.inChLow | ((uint16_t)r.inChHigh << 8);
  return mask & (1u << channel);
}

void handleLaunchpadButton(const RouteData &r, uint8_t button, bool pressed) {
  const LaunchpadButton &b = launchpadButtons[button];
  if (b.type == LP_BUTTON_OFF) return;
  const uint16_t outMask = r.outChLow | ((uint16_t)r.outChHigh << 8);
  for (uint8_t c = 0; c < 16; ++c) {
    if (!(outMask & (1u << c))) continue;
    PendingMsg msg;
    msg.used = true;
    msg.channel = c;
    msg.number = b.number;
    msg.data1 = b.number;
    msg.len = 3;
    if (b.type == LP_BUTTON_NOTE) {
      msg.kind = MK_NOTE;
      msg.statusHi = pressed ? 0x90 : 0x80;
      msg.data2 = pressed ? 127 : 0;
    } else {
      msg.kind = MK_CC;
      msg.statusHi = 0xB0;
      msg.data2 = pressed ? 127 : 0;
    }
    upsertPendingMask(routeOutputsNow(r), msg);
  }
}

// ============================================================================================
// Scale. Every Layout works in scale positions: scale index si counts scale notes from root in
// octave 0, so si = n is the root an octave up for an n-note scale.
// ============================================================================================
const ScaleDef SCALES[] = {
  {7, {0, 2, 4, 5, 7, 9, 11}},  // Major
  {7, {0, 2, 3, 5, 7, 8, 10}},  // Minor
  {7, {0, 2, 4, 5, 7, 8, 11}},  // Harmonic Major
  {7, {0, 2, 3, 5, 7, 8, 11}},  // Harmonic Minor
  {7, {0, 2, 4, 5, 7, 8, 10}},  // Melodic Major
  {7, {0, 2, 3, 5, 7, 9, 11}},  // Melodic Minor
  {6, {0, 2, 3, 4, 7, 9}},      // Major Blues
  {6, {0, 3, 5, 6, 7, 10}},     // Minor Blues
  {5, {0, 2, 4, 7, 9}},         // Major Pentatonic
  {5, {0, 3, 5, 7, 10}},        // Minor Pentatonic
  {7, {0, 2, 3, 5, 7, 9, 10}},  // Dorian
  {7, {0, 2, 4, 5, 7, 9, 10}},  // Mixolydian
};
constexpr uint8_t SCALE_COUNT = sizeof(SCALES) / sizeof(SCALES[0]);
constexpr uint8_t DRUM_PAD_ROOT = 36; // pad 0 of a drum pad plays the root at this octave
constexpr uint8_t NO_NOTE = 0xFF;

int16_t floorDiv(int16_t a, int16_t b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }

const ScaleDef &currentScale() {
  return SCALES[scaleSettings.scaleType < SCALE_COUNT ? scaleSettings.scaleType : 0];
}

int16_t scaleIndexToNote(int16_t si) {
  const ScaleDef &sc = currentScale();
  const int16_t oct = floorDiv(si, sc.size);
  return (scaleSettings.root % 12) + oct * 12 + sc.steps[si - oct * sc.size];
}

// The scale index of the first scale note at or above note.
int16_t scaleIndexAtOrAbove(int16_t note) {
  const ScaleDef &sc = currentScale();
  const int16_t rel = note - (scaleSettings.root % 12);
  const int16_t oct = floorDiv(rel, 12);
  const int16_t pc = rel - oct * 12;
  for (uint8_t d = 0; d < sc.size; ++d) {
    if (sc.steps[d] >= pc) return oct * sc.size + d;
  }
  return (oct + 1) * sc.size;
}

// The note a Scale route plays for an input note, NO_NOTE if it lands outside 0-127.
uint8_t scaleTransform(uint8_t note, uint8_t layout) {
  const ScaleDef &sc = currentScale();
  int16_t out;
  switch (layout) {
    case LAYOUT_WHITE_KEYS: {
      // Each white key plays the next scale degree, keeping the keyboard's octaves. A black key plays
      // the degree of the white key below it. Same as ARPnMIDI.
      static const uint8_t pcToWhite[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
      const int16_t white = pcToWhite[note % 12];
      const int16_t oct = floorDiv(white, sc.size);
      out = (note / 12) * 12 + (scaleSettings.root % 12) + oct * 12 + sc.steps[white - oct * sc.size];
      break;
    }
    case LAYOUT_PAD_5THS:
    case LAYOUT_PAD_4THS:
    case LAYOUT_PAD_3RDS: {
      // A 4-wide pad grid from note 36: right is the next scale note, up a row is rowOffset more.
      const int16_t rowOffset = layout == LAYOUT_PAD_5THS ? 4 : (layout == LAYOUT_PAD_4THS ? 3 : 2);
      const int16_t pad = (int16_t)note - DRUM_PAD_ROOT;
      const int16_t row = floorDiv(pad, 4);
      const int16_t col = pad - row * 4;
      out = scaleIndexToNote(scaleIndexAtOrAbove(DRUM_PAD_ROOT + (scaleSettings.root % 12)) + col + row * rowOffset);
      break;
    }
    default: // LAYOUT_NEXT_UP
      out = scaleIndexToNote(scaleIndexAtOrAbove(note));
      break;
  }
  return (out < 0 || out > 127) ? NO_NOTE : (uint8_t)out;
}

// The notes a Scale or Auto Chord route plays for an input note. Returns how many.
uint8_t notesForInput(const RouteData &r, uint8_t in, uint8_t out[CHORD_MAX]) {
  const uint8_t base = (r.features & FEATURE_SCALE) ? scaleTransform(in, r.scaleLayout) : in;
  if (base == NO_NOTE) return 0;
  if (!(r.features & FEATURE_AUTO_CHORD) || (r.flags2 & 0x01)) { // Mono Retrig plays single notes
    out[0] = base;
    return 1;
  }
  uint8_t n = 0;
  if (r.features & FEATURE_SCALE) {
    // Scale positions above the played note's own position, so the chord stays in key.
    const int16_t si = scaleIndexAtOrAbove(base);
    for (uint8_t k = 0; k < currentScale().size; ++k) {
      if (!(r.chordMask & (1u << k))) continue;
      const int16_t note = scaleIndexToNote(si + k);
      if (note >= 0 && note <= 127) out[n++] = (uint8_t)note;
    }
  } else {
    for (uint8_t k = 0; k < 12; ++k) {
      if (!(r.chordMask & (1u << k))) continue;
      if (base + k <= 127) out[n++] = base + k;
    }
  }
  return n;
}

// ============================================================================================
// Arp. Ported from ARPnMIDI's classic arp: Up, Down, Up-Down 1 (ends once), Up-Down 2 (ends
// twice), and Drum (every note at once, ARPnMIDI's trigger). Each full pass through the notes moves
// up an octave. All slots step on the clock engine's pulses, 96 per quarter, so they share one grid,
// as master or following. The notes a route lets through (after Launchpad, Scale, Auto Chord) are
// held into its slot, and the arp's steps go out through the route like ordinary notes.
// ============================================================================================
constexpr uint32_t ARP_CAPTURE_US = 6000;  // a fresh start waits this long to catch the whole chord
constexpr uint32_t ARP_MIN_GATE_US = 15000;

uint8_t arpModeOf(const ArpSettings &a) { return a.mode < ARP_MODE_COUNT ? a.mode : ARP_UP; }

// ARPnMIDI's pattern tokens: note index into the held notes (low to high), octave and semitone
// offsets. Copied from its kPatterns.
constexpr int8_t TOK_REST = -3, TOK_ALL = -2;
struct ArpToken { int8_t index, octave, semitone; };
const ArpToken ARP_PATTERNS[7][16] = {
  { {0,0,0},{1,0,0},{2,0,0},{3,0,0},{0,0,0},{1,0,0},{2,0,0},{3,0,0},
    {0,0,0},{1,0,0},{2,0,0},{3,0,0},{0,0,0},{1,0,0},{2,0,0},{3,0,0} }, // Up 4-Step (ARPnMIDI's Up 1-Oct)
  { {0,0,0},{TOK_REST,0,0},{0,0,0},{TOK_REST,0,0},{1,0,0},{TOK_REST,0,0},{1,0,0},{TOK_REST,0,0},
    {0,0,0},{TOK_REST,0,0},{2,0,0},{TOK_REST,0,0},{0,0,0},{TOK_REST,0,0},{3,0,0},{TOK_REST,0,0} }, // Rhythm
  { {0,0,0},{1,0,0},{2,0,0},{0,0,0},{1,0,0},{2,0,0},{0,0,0},{1,0,0},
    {2,0,0},{0,0,0},{1,0,0},{2,0,0},{0,0,0},{1,0,0},{2,0,0},{0,0,0} }, // Ostinato
  { {0,0,0},{0,1,0},{0,2,0},{0,3,0},{1,0,0},{1,1,0},{1,2,0},{1,3,0},
    {2,0,0},{2,1,0},{2,2,0},{2,3,0},{3,0,0},{3,1,0},{3,2,0},{3,3,0} }, // Octave Walk
  { {0,0,0},{0,0,7},{1,0,0},{1,0,7},{2,0,0},{2,0,7},{3,0,0},{3,0,7},
    {0,0,0},{0,0,7},{1,0,0},{1,0,7},{2,0,0},{2,0,7},{3,0,0},{3,0,7} }, // Fifth
  { {0,0,0},{TOK_ALL,0,0},{0,0,0},{TOK_ALL,0,0},{1,0,0},{TOK_ALL,0,0},{1,0,0},{TOK_ALL,0,0},
    {2,0,0},{TOK_ALL,0,0},{2,0,0},{TOK_ALL,0,0},{3,0,0},{TOK_ALL,0,0},{3,0,0},{TOK_ALL,0,0} }, // Bass+Chord
  { {TOK_ALL,0,0},{0,0,0},{1,0,0},{2,0,0},{3,0,0},{TOK_ALL,0,0},{3,0,0},{2,0,0},
    {1,0,0},{0,0,0},{TOK_ALL,0,0},{0,0,0},{1,0,0},{2,0,0},{3,0,0},{TOK_ALL,0,0} }  // Chord+Run
};
uint8_t arpOctavesOf(const ArpSettings &a) { return a.octaves >= 1 && a.octaves <= 4 ? a.octaves : 1; }
uint8_t arpGateOf(const ArpSettings &a) { return a.gatePercent >= 5 && a.gatePercent <= 100 ? a.gatePercent : 50; }

fw::MusicalDivision arpDivisionFrom(uint8_t divisionPlus1) {
  if (divisionPlus1 == 0 || divisionPlus1 > fw::kMusicalDivisionCount) return fw::MusicalDivision::Sixteenth;
  return static_cast<fw::MusicalDivision>(divisionPlus1 - 1);
}

// The division playing now: the last held division square, else the default square.
fw::MusicalDivision arpCurrentDivision(uint8_t slot) {
  const ArpRuntime &r = arps[slot];
  const ArpSettings &a = arpSettings[slot];
  if (r.momentCount) {
    const ArpSquare &sq = a.squares[r.moment[r.momentCount - 1]];
    if (sq.divisionPlus1) return arpDivisionFrom(sq.divisionPlus1);
  }
  return arpDivisionFrom(a.divisionPlus1);
}

fw::MusicalDivision arpDivisionOf(const ArpSettings &a) {
  if (a.divisionPlus1 == 0 || a.divisionPlus1 > fw::kMusicalDivisionCount) return fw::MusicalDivision::Sixteenth;
  return static_cast<fw::MusicalDivision>(a.divisionPlus1 - 1);
}

bool arpSounding(const ArpRuntime &r, uint8_t note) { return r.hold ? r.latched[note] : r.physical[note]; }

bool arpHasNotes(const ArpRuntime &r) {
  for (uint8_t n = 0; n < 128; ++n) if (arpSounding(r, n)) return true;
  return false;
}

bool otherArpRunning(uint8_t slot) {
  for (uint8_t s = 0; s < ARP_SLOTS; ++s) if (s != slot && arps[s].running) return true;
  return false;
}

void arpReleaseActive(uint8_t slot) {
  ArpRuntime &r = arps[slot];
  if (r.routeIndex < routeCount) {
    for (uint8_t k = 0; k < r.activeCount; ++k) sendRouteNote(r.routeIndex, routes[r.routeIndex], false, r.active[k], 0);
  }
  r.activeCount = 0;
  r.gateOffUs = 0;
}

// A note on one channel through the route's outputs, with its transpose and velocity scale.
void sendChannelNote(const RouteData &r, uint8_t channel, bool on, uint8_t note, uint8_t velocity) {
  const int16_t outNote = (int16_t)note + (int16_t)r.transpose - 64;
  if (outNote < 0 || outNote > 127) return;
  uint16_t scaled = ((uint16_t)velocity * r.velocityScale) / 100;
  PendingMsg msg;
  msg.used = true;
  msg.channel = channel & 0x0F;
  msg.kind = MK_NOTE;
  msg.statusHi = on ? 0x90 : 0x80;
  msg.number = (uint8_t)outNote;
  msg.data1 = (uint8_t)outNote;
  msg.data2 = on ? max<uint8_t>(1, (uint8_t)min<uint16_t>(127, scaled)) : 0;
  msg.len = 3;
  upsertPendingMask(routeOutputsNow(r), msg);
}

// Bypass and bass follow the notes the arp holds (hold included): whatever changed since last time
// is sent. releaseAll lets go of everything.
void arpSyncSide(uint8_t slot, bool releaseAll = false) {
  ArpRuntime &r = arps[slot];
  const ArpSettings &a = arpSettings[slot];
  if (r.routeIndex >= routeCount) return;
  const RouteData &rt = routes[r.routeIndex];
  uint8_t lowest = 0xFF;
  for (uint8_t n = 0; n < 128; ++n) {
    const bool sounding = !releaseAll && arpSounding(r, n);
    if (sounding && lowest == 0xFF) lowest = n;
    const uint8_t want = (sounding && a.bypassChannel) ? (r.sideOn[n] ? r.sideOn[n] : a.bypassChannel) : 0;
    if (want == r.sideOn[n]) continue;
    if (r.sideOn[n]) sendChannelNote(rt, r.sideOn[n] - 1, false, n, 0);
    if (want) sendChannelNote(rt, want - 1, true, n, r.velocity[n]);
    r.sideOn[n] = want;
  }
  const uint8_t wantBass = (a.bassChannel && lowest != 0xFF && lowest >= 12) ? lowest - 12 + 1 : 0;
  if (wantBass != r.bassNote) {
    if (r.bassNote) sendChannelNote(rt, r.bassCh, false, r.bassNote - 1, 0);
    if (wantBass) {
      r.bassCh = a.bassChannel - 1;
      sendChannelNote(rt, r.bassCh, true, wantBass - 1, r.velocity[lowest]);
    }
    r.bassNote = wantBass;
  }
  flushPending();
}

void arpStop(uint8_t slot) {
  arpReleaseActive(slot);
  arps[slot].running = false;
  arps[slot].pendingStartUs = 0;
  flushPending();
}

// After routes change: release what the arps are sounding through the routes as they were, then
// start clean. The clock picks up arp routes appearing or going away.
// Panic: every arp lets go of its notes, held keys and hold included.
void arpPanic() {
  for (uint8_t s = 0; s < ARP_SLOTS; ++s) {
    arpStop(s);
    ArpRuntime &r = arps[s];
    memset(r.physical, 0, sizeof(r.physical));
    memset(r.latched, 0, sizeof(r.latched));
    r.physicalCount = 0;
    r.hold = false;
    r.momentCount = 0;
    arpSyncSide(s);
  }
}

void arpForgetAll() {
  for (uint8_t s = 0; s < ARP_SLOTS; ++s) {
    arpStop(s);
    arpSyncSide(s, true);
    memset(&arps[s], 0, sizeof(ArpRuntime));
    arps[s].pressure = 127; // full velocity until the keyboard sends pressure
  }
  applyClockConfig();
}

// Called with each note the arp route lets through, in place of sending it.
void arpInput(uint16_t routeIndex, uint8_t note, uint8_t velocity, bool on) {
  const uint8_t slot = routes[routeIndex].arpSlot - 1;
  if (slot >= ARP_SLOTS) return;
  ArpRuntime &r = arps[slot];
  r.routeIndex = routeIndex;
  if (on) {
    if (r.hold && r.physicalCount == 0) memset(r.latched, 0, sizeof(r.latched)); // a new phrase replaces the held one
    if (r.physicalCount == 0 || r.pressCount == 0xFFFF) r.pressCount = 0;       // press order restarts with each phrase
    if (!r.physical[note]) ++r.physicalCount;
    r.physical[note] = true;
    r.latched[note] = true;
    r.velocity[note] = velocity;
    r.pressOrder[note] = ++r.pressCount;
    if (!r.running && !r.pendingStartUs) r.pendingStartUs = time_us_64() + ARP_CAPTURE_US;
  } else {
    if (r.physical[note] && r.physicalCount) --r.physicalCount;
    r.physical[note] = false;
    if (!r.hold) r.latched[note] = false;
    if (!arpHasNotes(r)) arpStop(slot);
  }
  arpSyncSide(slot);
}

void arpSetHold(uint8_t slot, bool hold) {
  if (slot >= ARP_SLOTS) return;
  ArpRuntime &r = arps[slot];
  if (r.hold == hold) return;
  r.hold = hold;
  if (!hold) {
    memcpy(r.latched, r.physical, sizeof(r.latched)); // only the keys still down keep playing
    if (!arpHasNotes(r)) arpStop(slot);
  }
  arpSyncSide(slot);
}

// The division square a note (type 1) or CC (type 2) triggers on this route's arp, -1 none.
int8_t arpSquareFor(const RouteData &rt, uint8_t inputDevice, uint8_t type, uint8_t channel, uint8_t number) {
  if (!rt.arpSlot || rt.arpSlot > ARP_SLOTS) return -1;
  const ArpSettings &a = arpSettings[rt.arpSlot - 1];
  for (uint8_t k = 0; k < ARP_SQUARES; ++k) {
    const ArpSquare &sq = a.squares[k];
    if (!sq.divisionPlus1 || sq.triggerType != type || sq.triggerNumber != number) continue;
    if (sq.triggerChannel <= 15 && sq.triggerChannel != channel) continue;
    if (!routeAcceptsSource(rt, inputDevice, channel)) return -1;
    return (int8_t)k;
  }
  return -1;
}

// A division square pressed or released. The new division starts on its own next grid step.
void arpMomentary(uint8_t slot, uint8_t square, bool down) {
  ArpRuntime &r = arps[slot];
  uint8_t n = 0;
  for (uint8_t k = 0; k < r.momentCount; ++k) if (r.moment[k] != square) r.moment[n++] = r.moment[k];
  r.momentCount = n;
  if (down && r.momentCount < ARP_SQUARES) r.moment[r.momentCount++] = square;
  if (r.running) {
    const uint16_t div = fw::divisionPulses(arpCurrentDivision(slot));
    r.nextStepPulse = ((clockEngine.internalPulse() / div) + 1) * div;
  }
}

bool arpHoldMatches(const RouteData &rt, uint8_t inputDevice, uint8_t channel, uint8_t cc) {
  if (!rt.arpSlot || rt.arpSlot > ARP_SLOTS) return false;
  const ArpSettings &a = arpSettings[rt.arpSlot - 1];
  if (a.holdType != 2 || a.holdNumber != cc) return false;
  if (a.holdChannel <= 15 && a.holdChannel != channel) return false;
  return routeAcceptsSource(rt, inputDevice, channel);
}

void arpStep(uint8_t slot, uint64_t nowUs) {
  ArpRuntime &r = arps[slot];
  const ArpSettings &a = arpSettings[slot];
  arpReleaseActive(slot);
  if (r.routeIndex >= routeCount) return;
  uint8_t notes[128];
  uint8_t n = 0;
  for (uint8_t note = 0; note < 128; ++note) if (arpSounding(r, note)) notes[n++] = note;
  if (n == 0) return;

  const uint8_t octaves = arpOctavesOf(a);
  const uint8_t mode = arpModeOf(a);
  auto play = [&](uint8_t note, uint8_t vel) {
    if (r.activeCount >= ARP_MAX_OUT) return;
    for (uint8_t k = 0; k < r.activeCount; ++k) if (r.active[k] == note) return;
    r.active[r.activeCount++] = note;
    sendRouteNote(r.routeIndex, routes[r.routeIndex], true, note, vel);
  };
  // Aftertouch velocity maps pressure to 33-100%, as ARPnMIDI's drum roll does.
  const uint8_t pressureVel = (uint8_t)map(r.pressure, 0, 127, 42, 127);
  auto velOf = [&](uint8_t note) -> uint8_t {
    if (a.aftertouchVelocity) return pressureVel;
    return a.velocity ? a.velocity : max<uint8_t>(1, r.velocity[note]);
  };
  // Pattern offsets can leave the key, so a Scale route snaps them up to the next scale note.
  const bool snap = routes[r.routeIndex].features & FEATURE_SCALE;
  auto playAt = [&](int16_t note, uint8_t vel) {
    if (note < 0 || note > 127) return;
    if (snap) note = scaleIndexToNote(scaleIndexAtOrAbove((uint8_t)note));
    if (note >= 0 && note <= 127) play((uint8_t)note, vel);
  };
  auto playAll = [&]() {
    for (uint8_t o = 0; o < octaves; ++o) {
      for (uint8_t i = 0; i < n; ++i) playAt(notes[i] + o * 12, velOf(notes[i]));
    }
  };

  const ArpToken *pattern = (mode >= ARP_PAT_UP_4STEP && mode <= ARP_PAT_CHORD_RUN) ? ARP_PATTERNS[mode - ARP_PAT_UP_4STEP] : nullptr;
  const ArpToken token = pattern ? pattern[r.patternStep % 16] : ArpToken{0, 0, 0};
  if (pattern && token.index == TOK_REST) {
    // a rest
  } else if (mode == ARP_DRUM || (pattern && token.index == TOK_ALL)) {
    playAll();
  } else if (mode >= ARP_AS_PLAYED) {
    // Each mode is an order of the held notes, played round; a full pass moves up an octave.
    uint8_t seq[256];
    uint16_t len = 0;
    if (mode == ARP_AS_PLAYED) {
      for (uint8_t i = 0; i < n; ++i) seq[len++] = i;
      for (uint8_t i = 1; i < n; ++i) { // insertion sort by press order
        const uint8_t v = seq[i];
        uint8_t j = i;
        while (j > 0 && r.pressOrder[notes[seq[j - 1]]] > r.pressOrder[notes[v]]) { seq[j] = seq[j - 1]; --j; }
        seq[j] = v;
      }
    } else if (mode == ARP_CONVERGE || mode == ARP_DIVERGE) {
      for (uint8_t k = 0; k < n; ++k) seq[len++] = (k & 1) ? n - 1 - k / 2 : k / 2;
      if (mode == ARP_DIVERGE) for (uint8_t a2 = 0, b2 = n - 1; a2 < b2; ++a2, --b2) { const uint8_t t = seq[a2]; seq[a2] = seq[b2]; seq[b2] = t; }
    } else if (n == 1) {
      seq[len++] = 0;
    } else if (mode == ARP_THUMB) {
      for (uint8_t i = 1; i < n; ++i) { seq[len++] = 0; seq[len++] = i; }
    } else { // ARP_PINKY
      for (uint8_t i = 0; i < n - 1; ++i) { seq[len++] = i; seq[len++] = n - 1; }
    }
    const uint8_t idx = seq[r.sequenceStep % len];
    const uint8_t octave = (r.sequenceStep / len) % octaves;
    playAt(notes[idx] + octave * 12, velOf(notes[idx]));
  } else if (mode == ARP_PAT_FIFTH) {
    // Each held note then a fifth above it, through however many notes are held. ARPnMIDI's table
    // assumed 4 notes. The fifth is 4 scale positions up on a Scale route, so it stays in key,
    // otherwise a perfect fifth.
    const uint16_t len = n * 2;
    const uint16_t pos = r.sequenceStep % len;
    const uint8_t base = notes[pos / 2];
    const uint8_t octave = (r.sequenceStep / len) % octaves;
    int16_t note = base;
    if (pos & 1) note = snap ? scaleIndexToNote(scaleIndexAtOrAbove(base) + 4) : base + 7;
    playAt(note + octave * 12, velOf(base));
  } else if (pattern) {
    const uint8_t octave = (r.sequenceStep / n) % octaves;
    const uint8_t base = notes[token.index % n];
    playAt(base + token.semitone + (token.octave + octave) * 12, velOf(base));
  } else {
    const uint32_t phase = r.sequenceStep;
    uint8_t idx;
    if (n == 1) {
      idx = 0;
    } else if (mode == ARP_RANDOM) {
      idx = random(n);
    } else if (mode == ARP_DOWN) {
      idx = n - 1 - (phase % n);
    } else if (mode == ARP_UPDOWN1) {
      const uint8_t period = n * 2 - 2, pos = phase % period;
      idx = pos < n ? pos : period - pos;
    } else if (mode == ARP_UPDOWN2) {
      const uint8_t period = n * 2, pos = phase % period;
      idx = pos < n ? pos : period - 1 - pos;
    } else {
      idx = phase % n;
    }
    const uint8_t octave = (phase / n) % octaves;
    const uint16_t note = notes[idx] + octave * 12;
    if (note <= 127) play((uint8_t)note, velOf(notes[idx]));
  }
  flushPending();
  ++r.sequenceStep;
  r.patternStep = (r.patternStep + 1) % 16;

  const uint64_t stepUs = fw::divisionDurationUs(arpCurrentDivision(slot), clockEngine.bpm());
  const uint64_t gateUs = stepUs * arpGateOf(a) / 100;
  r.gateOffUs = nowUs + (gateUs > ARP_MIN_GATE_US ? gateUs : ARP_MIN_GATE_US);
}

void serviceArps() {
  const uint64_t now = time_us_64();
  for (uint8_t s = 0; s < ARP_SLOTS; ++s) {
    ArpRuntime &r = arps[s];
    if (r.gateOffUs && now >= r.gateOffUs) {
      arpReleaseActive(s);
      flushPending();
    }
    if (r.pendingStartUs && now >= r.pendingStartUs) {
      r.pendingStartUs = 0;
      if (!arpHasNotes(r)) continue;
      const uint16_t div = fw::divisionPulses(arpCurrentDivision(s));
      // A fresh start restarts the master grid. Joining a running arp, or following another clock,
      // lines up with that grid's next step instead.
      if (!otherArpRunning(s) && !clockFollowPort()) clockEngine.start(now);
      const uint32_t pulse = clockEngine.internalPulse();
      r.nextStepPulse = ((pulse + div - 1) / div) * div;
      r.sequenceStep = 0;
      r.patternStep = 0;
      r.running = true;
    }
    if (!r.running) continue;
    if (!clockEngine.synchronizedAdvanceAllowed(now)) continue;
    const uint32_t pulse = clockEngine.internalPulse();
    if (pulse >= r.nextStepPulse) {
      const uint16_t div = fw::divisionPulses(arpCurrentDivision(s));
      arpStep(s, now);
      // One step per pass. After a stall, skip ahead rather than burst through missed steps.
      r.nextStepPulse += div;
      if (pulse >= r.nextStepPulse) r.nextStepPulse = ((pulse / div) + 1) * div;
    }
  }
}

void dispatchNoteEvent(uint8_t inputDevice, uint8_t channel, bool isNoteOn, uint8_t rawNote, uint8_t velocity) {
  bool matchedAny = false;
  for (uint16_t i = 0; i < routeCount; ++i) {
    uint8_t note = rawNote;
    launchpadExcludePorts = 0;
    const int8_t square = arpSquareFor(routes[i], inputDevice, 1, channel, rawNote);
    if (square >= 0) { // a division trigger, it doesn't pass through
      matchedAny = true;
      arpMomentary(routes[i].arpSlot - 1, square, isNoteOn);
      continue;
    }
    if (routes[i].features & FEATURE_LAUNCHPAD) {
      if (!routeAcceptsSource(routes[i], inputDevice, channel)) continue;
      launchpadExcludePorts = sourcePort(inputDevice);
      sendToOutputs(launchpadExcludePorts, isNoteOn ? 0x90 : 0x80, channel, rawNote, velocity, 3);
      const uint8_t side = launchpadSideButton(rawNote);
      if (side != 0xFF) {
        matchedAny = true;
        handleLaunchpadButton(routes[i], side, isNoteOn);
        continue;
      }
      note = launchpadGridNote(rawNote);
      if (note == LP_NOT_GRID) continue;
    }
    if (routeMatches(routes[i], inputDevice, channel, MK_NOTE, note)) {
      matchedAny = true;
      if (handleNoteToCc(routes[i], isNoteOn, note, velocity)) continue;
      if (routes[i].features & (FEATURE_SCALE | FEATURE_AUTO_CHORD)) {
        // A Note Off releases exactly what its Note On played, even if the settings changed between.
        uint8_t outs[CHORD_MAX];
        uint8_t count;
        if (isNoteOn) {
          if (findHeldKey(i, note)) continue; // already held (a repeat Note On), keep what's sounding
          count = notesForInput(routes[i], note, outs);
          if (count == 0) continue;
        } else {
          HeldKey *h = findHeldKey(i, note);
          if (!h) continue;
          count = h->count;
          memcpy(outs, h->out, count);
          h->used = false;
        }
        if (routes[i].arpSlot) {
          if (isNoteOn) holdKey(i, note, outs, count);
          for (uint8_t k = 0; k < count; ++k) arpInput(i, outs[k], velocity, isNoteOn);
          continue;
        }
        if (routes[i].flags2 & 0x01) {
          if (isNoteOn) holdKey(i, note, outs, count);
          handleMonoRetrigNote(i, routes[i], outs[0], velocity, isNoteOn);
          continue;
        }
        for (uint8_t k = 0; k < count; ++k) {
          const uint8_t out = outs[k];
          if (heldOutSounding(i, out)) continue; // another held key on this route already plays it
          sendRouteNote(i, routes[i], isNoteOn, out, isNoteOn ? velocity : 0);
          if (!isNoteOn) routeNoteFreeSlot(routeNoteTracks[i], out);
        }
        if (isNoteOn) holdKey(i, note, outs, count);
        continue;
      }
      if (routes[i].arpSlot) {
        arpInput(i, note, velocity, isNoteOn);
        continue;
      }
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
  launchpadExcludePorts = 0;
  flushPending();

  if (!matchedAny) {
    applyModeDefault(inputDevice, isNoteOn ? 0x90 : 0x80, channel, rawNote, velocity, 3);
  } else {
    // still count as "seen" on the input side for the IN LED even if fully consumed by routes
    if (inputDevice == DEV_USB) noteActivity(ledUsb); else noteActivity(ledIn);
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
  if (captureLearnOrTap(inputDevice, kind, isNoteOn, channel, data1, data2)) return;

  if (kind == MK_NOTE && (inputDevice == DEV_HOST || inputDevice == DEV_TRS2)) {
    sidecarHeld[channel][data1] = !isNoteOn ? 0 : (inputDevice == DEV_HOST ? currentHostPort : 0x80);
  }

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
    NoteHoldStack &stack = noteStackFor(inputDevice, channel);
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
    if (kind == MK_CC && (routes[i].features & FEATURE_LAUNCHPAD) && number >= LP_TOP_BUTTON_CC && number < LP_TOP_BUTTON_CC + 8) {
      if (routeAcceptsSource(routes[i], inputDevice, channel)) {
        matchedAny = true;
        launchpadExcludePorts = sourcePort(inputDevice);
        sendToOutputs(launchpadExcludePorts, 0xB0, channel, number, data2, 3); // lights the button
        handleLaunchpadButton(routes[i], number - LP_TOP_BUTTON_CC, data2 > 0);
        flushPending();
        launchpadExcludePorts = 0;
      }
      continue;
    }
    if (kind == MK_CC) {
      const int8_t square = arpSquareFor(routes[i], inputDevice, 2, channel, number);
      if (square >= 0) {
        matchedAny = true;
        arpMomentary(routes[i].arpSlot - 1, square, data2 >= 64);
        continue;
      }
    }
    // Aftertouch velocity follows channel pressure (value in the first data byte) or poly aftertouch
    // (second byte), whichever the controller sends. Pads often send poly. Both still pass through below.
    if ((kind == MK_CHAN_PRESSURE || kind == MK_POLY_AT) && routes[i].arpSlot && routes[i].arpSlot <= ARP_SLOTS &&
        arpSettings[routes[i].arpSlot - 1].aftertouchVelocity && routeAcceptsSource(routes[i], inputDevice, channel)) {
      arps[routes[i].arpSlot - 1].pressure = kind == MK_POLY_AT ? data2 : data1;
    }
    if (kind == MK_CC && arpHoldMatches(routes[i], inputDevice, channel, number)) {
      matchedAny = true;
      arpSetHold(routes[i].arpSlot - 1, data2 >= 64);
      continue;
    }
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
    if (inputDevice == DEV_USB) noteActivity(ledUsb); else noteActivity(ledIn);
  }
}

// Shared by Sysex, System Common, and System Realtime — none of them are channel-addressed, so
// there's no per-channel dedup table the way Note/CC/etc get; the last matching route's outDevice
// simply wins as the destination, same idea as route-order priority elsewhere, just resolved once
// up front instead of per-message via upsertPending.
uint16_t resolveDestination(uint8_t inputDevice, MsgKind kind) {
  bool matchedAny = false;
  uint16_t chosen = 0;
  for (uint16_t i = 0; i < routeCount; ++i) {
    if (routeMatches(routes[i], inputDevice, 0, kind, 0)) {
      matchedAny = true;
      chosen = routeOutputPorts(routes[i]);
    }
  }
  return matchedAny ? chosen : modeDefaultPorts(inputDevice);
}

// System Common (0xF1-0xF7 minus sysex framing) and System Realtime (0xF8-0xFF, split into Clock
// vs. everything else) carry no channel nibble and no note/cc number, so — unlike Note/CC/etc —
// they never go through applyRouteOutput's per-channel PendingMsg/dedup machinery, just straight to
// whichever physical port(s) resolveDestination() lands on, mirroring exactly how Sysex is already
// handled. The message kind is inferred from the status byte itself so every call site can just
// pass the raw byte through without also having to classify it themselves.
void forwardCommonOrRealtime(uint8_t inputDevice, uint8_t statusByte, uint8_t data1, uint8_t data2, uint8_t len) {
  if (!acceptClockMessage(inputDevice, statusByte)) return;
  MsgKind kind;
  if (statusByte == 0xF8) kind = MK_CLOCK;
  else if (statusByte >= 0xF8) kind = MK_REALTIME;
  else kind = MK_COMMON;
  sendToOutputs(resolveDestination(inputDevice, kind), statusByte, 0, data1, data2, len);
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
    usbSend(pkt);
    accum.len = 0;
    noteActivity(ledUsb);
    return;
  }
  if (accum.len == 3) {
    uint8_t pkt[4] = {0x04, accum.buf[0], accum.buf[1], accum.buf[2]};
    usbSend(pkt);
    accum.len = 0;
    noteActivity(ledUsb);
  }
}

// Same chunking as usbSysexByte, into link frames for the sidecar. Each finished packet goes to
// every port in ports.
void linkSysexByte(UsbOutSysexAccum &accum, const uint8_t *ports, uint8_t portCount, uint8_t b) {
  accum.buf[accum.len++] = b;
  if (b != 0xF7 && accum.len < 3) return;
  uint8_t pkt[4] = {(uint8_t)(b == 0xF7 ? 0x04 + accum.len : 0x04), 0, 0, 0};
  for (uint8_t i = 0; i < accum.len; ++i) pkt[1 + i] = accum.buf[i];
  for (uint8_t i = 0; i < portCount; ++i) linkSend(ports[i], pkt);
  accum.len = 0;
}

void forwardSysexByte(uint8_t fromInputDevice, uint16_t destPorts, uint8_t b) {
  if (destPorts & PORT_TRS) {
    trsSend(&b, 1);
    noteActivity(ledOut);
  }
  if (destPorts & PORT_USB) usbSysexByte(usbOutFrom[fromInputDevice], b);
  uint8_t ports[9];
  uint8_t n = linkPortsFor(destPorts, ports);
  if (n) {
    linkSysexByte(linkOutFrom[fromInputDevice], ports, n, b);
    noteActivity(ledOut);
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
      forwardSysexByte(inputDevice, s.fwdPorts, b);
    } else if (s.phase == SysexState::PEEK1) {
      // F0 F7 — empty/malformed message, still forward transparently
      s.fwdPorts = resolveDestination(inputDevice, MK_SYSEX);
      forwardSysexByte(inputDevice, s.fwdPorts, 0xF0);
      forwardSysexByte(inputDevice, s.fwdPorts, b);
    } else if (s.phase == SysexState::PEEK2) {
      s.fwdPorts = resolveDestination(inputDevice, MK_SYSEX);
      forwardSysexByte(inputDevice, s.fwdPorts, 0xF0);
      forwardSysexByte(inputDevice, s.fwdPorts, 0x7D);
      forwardSysexByte(inputDevice, s.fwdPorts, b);
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
        s.fwdPorts = resolveDestination(inputDevice, MK_SYSEX);
        forwardSysexByte(inputDevice, s.fwdPorts, 0xF0);
        forwardSysexByte(inputDevice, s.fwdPorts, b);
        s.phase = SysexState::FORWARDING;
      }
      break;
    case SysexState::PEEK2:
      if (b == SYSEX_DEVICE_ID) {
        s.phase = SysexState::OURS;
        s.cmdLen = 0;
      } else {
        s.fwdPorts = resolveDestination(inputDevice, MK_SYSEX);
        forwardSysexByte(inputDevice, s.fwdPorts, 0xF0);
        forwardSysexByte(inputDevice, s.fwdPorts, SYSEX_MFR_ID);
        forwardSysexByte(inputDevice, s.fwdPorts, b);
        s.phase = SysexState::FORWARDING;
      }
      break;
    case SysexState::OURS:
      if (s.cmdLen < sizeof(s.cmdBuf)) s.cmdBuf[s.cmdLen++] = b;
      break;
    case SysexState::FORWARDING:
      forwardSysexByte(inputDevice, s.fwdPorts, b);
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
    trsSend(replyBuf, replyLen);
    noteActivity(ledOut);
  } else if (sourcePort == DEV_HOST || sourcePort == DEV_TRS2) {
    UsbOutSysexAccum accum; // reply goes back out the sidecar port the command came in on
    const uint8_t port = (sourcePort == DEV_HOST) ? currentHostPort : LINK_PORT_DIN;
    for (uint8_t i = 0; i < replyLen; ++i) linkSysexByte(accum, &port, 1, replyBuf[i]);
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

// A route from ADD_ROUTE/SET_ROUTE. Protocol 1 hosts send 41 bytes, the missing port masks stay zero
// with flags bit6 clear, so the route keeps its legacy meaning.
RouteData unpackRoute(const uint8_t *packed, uint8_t packedLen) {
  uint8_t raw[sizeof(((SysexState *)nullptr)->cmdBuf)];
  uint8_t rawLen = unpack7(packed, packedLen, raw);
  RouteData r;
  memset(&r, 0, sizeof(r));
  memcpy(&r, raw, min((size_t)rawLen, sizeof(r)));
  return r;
}

// Clears a half-sent update so a new one can start. One with every byte written but no FW_END
// can't be cleared without staging it, so that case reports failure and needs a power cycle.
bool fwResetUpdate() {
  if (!Update.isRunning()) return true;
  if (Update.isFinished()) return false;
  Update.end(false);
  return !Update.isRunning();
}

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

    case CMD_GET_FEATURE: {
      if (payloadLen < 1) { sendAck(sourcePort, cmd, ACK_ERR_BAD_DATA, 0, 0); break; }
      uint8_t size;
      const uint8_t *data = featureBlockData(payload[0], size);
      if (!data) { sendAck(sourcePort, cmd, ACK_ERR_BAD_INDEX, 0, 0); break; }
      beginReply(CMD_FEATURE_DATA);
      appendReply(payload[0]);
      uint8_t packed[sizeof(replyBuf)];
      const uint8_t packedLen = pack7(data, size, packed);
      for (uint8_t i = 0; i < packedLen; ++i) appendReply(packed[i]);
      sendReply(sourcePort);
      break;
    }

    case CMD_SET_FEATURE: {
      if (payloadLen < 1) { sendAck(sourcePort, cmd, ACK_ERR_BAD_DATA, 0, 0); break; }
      uint8_t size;
      uint8_t *data = featureBlockData(payload[0], size);
      if (!data) { sendAck(sourcePort, cmd, ACK_ERR_BAD_INDEX, 0, 0); break; }
      uint8_t raw[sizeof(((SysexState *)nullptr)->cmdBuf)];
      const uint8_t rawLen = unpack7(payload + 1, payloadLen - 1, raw);
      memset(data, 0, size);
      memcpy(data, raw, min(rawLen, size));
      if (payload[0] == FEATURE_TYPE_NOTE_TO_CC) memset(noteCcToggleState, 0, sizeof(noteCcToggleState));
      sendAck(sourcePort, cmd, ACK_OK, checksum7(data, size), payload[0]);
      break;
    }

    case CMD_LEARN: {
      // The next Note On or CC from any input is captured, consumed, and sent back as LEARN_DATA.
      learnActive = true;
      learnReplyPort = sourcePort;
      learnUntilMs = millis() + LEARN_TIMEOUT_MS;
      break;
    }

    case CMD_GET_STATUS: {
      beginReply(CMD_STATUS_DATA);
      appendReply(hostPresent() ? 1 : 0);
      appendReply(hostPresent() ? (hostPortMask & 0x0F) : 0);
      appendReply(hostDropCount & 0x7F);
      appendReply(hostRestartReason & 0x7F);
      appendReply(hostRestartPort & 0x7F);
      appendReply(hostRestartCount & 0x7F);
      const uint16_t bpmNow = (uint16_t)(clockEngine.bpm() + 0.5f);
      appendReply(bpmNow & 0x7F);
      appendReply((bpmNow >> 7) & 0x7F);
      appendReply(clockStatus());
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
      appendReply(globalSettings.clockSource);
      // The values in use: saves from before these fields existed hold zeros, which mean the defaults.
      appendReply(effectiveBpm() & 0x7F);
      appendReply((effectiveBpm() >> 7) & 0x7F);
      appendReply(globalSettings.beatsPerBar ? globalSettings.beatsPerBar : 4);
      appendReply(globalSettings.beatUnit ? globalSettings.beatUnit : 4);
      appendReply(globalSettings.clockOut);
      appendReply(globalSettings.tapType);
      appendReply(globalSettings.tapChannel);
      appendReply(globalSettings.tapNumber);
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
      // Protocol 1-2 hosts send only the first 6 bytes, the clock settings stay as they are.
      const uint8_t applied = (payloadLen >= GLOBAL_WIRE_BYTES) ? GLOBAL_WIRE_BYTES : 6;
      if (applied == GLOBAL_WIRE_BYTES) {
        globalSettings.clockSource = payload[6];
        globalSettings.bpm = (uint16_t)payload[7] | ((uint16_t)payload[8] << 7);
        globalSettings.beatsPerBar = payload[9];
        globalSettings.beatUnit = payload[10];
        globalSettings.clockOut = payload[11];
        globalSettings.tapType = payload[12];
        globalSettings.tapChannel = payload[13];
        globalSettings.tapNumber = payload[14];
      }
      applyBrightnessToLeds();
      resetButtonRuntimeState();
      applyClockConfig();
      sendAck(sourcePort, cmd, ACK_OK, checksum7(payload, applied), 0);
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
      uint8_t packed[64]; // pack7(53 raw bytes) = 61 packed bytes
      uint8_t packedLen = pack7((const uint8_t *)&routes[idx], ROUTE_RAW_BYTES, packed);
      for (uint8_t i = 0; i < packedLen; ++i) appendReply(packed[i]);
      sendReply(sourcePort);
      break;
    }

    case CMD_ADD_ROUTE: {
      if (routeCount >= MAX_ROUTES) { sendAck(sourcePort, cmd, ACK_ERR_FULL, 0, 0); break; }
      RouteData r = unpackRoute(payload, payloadLen);
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
      RouteData r = unpackRoute(payload + 2, payloadLen - 2);
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
      applyClockConfig();
      saveToFlash();
      sendAck(sourcePort, cmd, ACK_OK, 0, 0);
      allLedConfirmationFlashes();
      break;
    }

    // Firmware update. Nothing is installed until FW_END checks the size and MD5 of the whole
    // file, so a failed or abandoned transfer leaves the running firmware untouched.
    case CMD_FW_BEGIN: {
      if (payloadLen < 35) { sendAck(sourcePort, cmd, ACK_ERR_BAD_DATA, 0, 0); break; }
      uint32_t size = (uint32_t)payload[0] | ((uint32_t)payload[1] << 7) | ((uint32_t)payload[2] << 14);
      char md5[33];
      memcpy(md5, payload + 3, 32);
      md5[32] = '\0';
      rp2040.wdt_reset(); // the first update may format the update partition
      if (!fwResetUpdate() || !Update.begin(size) || !Update.setMD5(md5)) {
        uint8_t error = Update.getError();
        fwResetUpdate();
        sendAck(sourcePort, cmd, ACK_ERR_UPDATE, 0, error);
        break;
      }
      fwNextChunk = 0;
      sendAck(sourcePort, cmd, ACK_OK, checksum7(payload, 35), 0);
      break;
    }

    case CMD_FW_DATA: {
      if (payloadLen < 3 || !Update.isRunning()) { sendAck(sourcePort, cmd, ACK_ERR_UPDATE, 0, 0); break; }
      uint16_t idx = read16(payload);
      if (fwNextChunk > 0 && idx == fwNextChunk - 1) {
        sendAck(sourcePort, cmd, ACK_OK, fwLastChecksum, idx);
        break;
      }
      if (idx != fwNextChunk) { sendAck(sourcePort, cmd, ACK_ERR_BAD_INDEX, 0, fwNextChunk); break; }
      uint8_t packedLen = payloadLen - 2;
      if (packedLen > 147) { sendAck(sourcePort, cmd, ACK_ERR_BAD_DATA, 0, idx); break; }
      uint8_t raw[FW_CHUNK_BYTES];
      uint8_t rawLen = unpack7(payload + 2, packedLen, raw);
      if (rawLen == 0 || Update.write(raw, rawLen) != rawLen) {
        sendAck(sourcePort, cmd, ACK_ERR_UPDATE, 0, Update.getError());
        break;
      }
      fwLastChecksum = checksum7(raw, rawLen);
      fwNextChunk++;
      sendAck(sourcePort, cmd, ACK_OK, fwLastChecksum, idx);
      break;
    }

    case CMD_FW_END: {
      if (!Update.isRunning()) { sendAck(sourcePort, cmd, ACK_ERR_UPDATE, 0, 0); break; }
      rp2040.wdt_reset(); // Update.end() re-reads the whole file for its MD5
      if (!Update.end()) { // checks size and MD5, then stages the file for the OTA bootloader
        sendAck(sourcePort, cmd, ACK_ERR_UPDATE, 0, Update.getError());
        break;
      }
      sendAck(sourcePort, cmd, ACK_OK, 0, 0);
      fwRebootPending = true;
      fwRebootAtMs = millis() + FW_REBOOT_DELAY_MS;
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
  uint8_t budget = INPUT_BUDGET;
  if (!outputsHaveRoom()) return;
  while (budget-- && Serial1.available() > 0) {
    processDinByte(static_cast<uint8_t>(Serial1.read()));
  }
}

// ============================================================================================
// USB input parsing (4-byte USB-MIDI packets) -> processIncomingEvent / feedSysexByte
// ============================================================================================
// Shared by the USB device port and the host link, which both carry USB-MIDI packets.
void handleUsbMidiPacket(uint8_t inputDevice, SysexState &sysex, const uint8_t packet[4]) {
  uint8_t cin = packet[0] & 0x0F;
  noteActivity(inputDevice == DEV_USB ? ledUsb : ledIn);

  if (cin == 0x04) {
    feedSysexByte(inputDevice, sysex, packet[1]);
    feedSysexByte(inputDevice, sysex, packet[2]);
    feedSysexByte(inputDevice, sysex, packet[3]);
    return;
  }
  if (cin == 0x06) {
    feedSysexByte(inputDevice, sysex, packet[1]);
    feedSysexByte(inputDevice, sysex, packet[2]);
    return;
  }
  if (cin == 0x07) {
    feedSysexByte(inputDevice, sysex, packet[1]);
    feedSysexByte(inputDevice, sysex, packet[2]);
    feedSysexByte(inputDevice, sysex, packet[3]);
    return;
  }
  if (cin == 0x05) {
    if (sysex.phase != SysexState::IDLE) {
      feedSysexByte(inputDevice, sysex, packet[1]); // lone 0xF7 closing a sysex
    } else {
      // Realtime (>=0xF8) or a 1-byte System Common message (Tune Request 0xF6) — both route
      // through forwardCommonOrRealtime, which tells them apart by the status byte itself.
      forwardCommonOrRealtime(inputDevice, packet[1], 0, 0, 1);
    }
    return;
  }
  if (cin == 0x02) { // 2-byte System Common: MTC Quarter Frame (0xF1) or Song Select (0xF3)
    forwardCommonOrRealtime(inputDevice, packet[1], packet[2], 0, 2);
    return;
  }
  if (cin == 0x03) { // 3-byte System Common: Song Position Pointer (0xF2)
    forwardCommonOrRealtime(inputDevice, packet[1], packet[2], packet[3], 3);
    return;
  }

  uint8_t statusHi = packet[1] & 0xF0;
  uint8_t channel = packet[1] & 0x0F;
  switch (cin) {
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0E:
      processIncomingEvent(inputDevice, statusHi, channel, packet[2], packet[3], 3);
      break;
    case 0x0C: case 0x0D:
      processIncomingEvent(inputDevice, statusHi, channel, packet[2], 0, 2);
      break;
    case 0x0F:
      // Same "Single Byte" convention as CIN 0x05 above — some hosts use 0x0F instead of 0x05 for
      // realtime/single-byte-common; forwardCommonOrRealtime handles either the same way.
      forwardCommonOrRealtime(inputDevice, packet[1], 0, 0, 1);
      break;
    default:
      break;
  }
}

void pumpUsbIn() {
  uint8_t packet[4];
  uint8_t budget = INPUT_BUDGET; // a flood (a feedback loop in a DAW) can't starve everything else
  if (!outputsHaveRoom()) return; // an output backed up: the computer waits, nothing is dropped
  while (budget-- && usb_midi.readPacket(packet)) handleUsbMidiPacket(DEV_USB, usbSysex, packet);
}

// Host brain frames: ports 1-4 are hub ports, 5 is its DIN IN, 15 is control. All parsing is
// bounded per loop and reads only what the PIO receive interrupt has already buffered.
// Releases every note still held from the host brain's inputs, as if its Note Off had arrived. With
// onlyPort (1-4), just the notes from that hub port.
void releaseSidecarNotes(uint8_t onlyPort = 0) {
  for (uint8_t ch = 0; ch < 16; ++ch) {
    for (uint8_t note = 0; note < 128; ++note) {
      const uint8_t held = sidecarHeld[ch][note];
      if (!held || (onlyPort && held != onlyPort)) continue;
      uint8_t device = DEV_TRS2;
      if (held != 0x80) {
        device = DEV_HOST;
        currentHostPort = held;
      }
      processIncomingEvent(device, 0x80, ch, note, 0, 3); // clears sidecarHeld itself
      sidecarHeld[ch][note] = 0;
    }
  }
}

// After the host brain restarts or comes back, its outputs may have missed Note Offs while it was
// gone. All Notes Off on its ports only, the main brain's own ports are left alone.
void silenceSidecarOutputs() {
  for (uint8_t ch = 0; ch < 16; ++ch) sendToOutputs(PORT_HOST_ALL | PORT_TRS2_6, 0xB0, ch, 123, 0, 3);
}

void serviceStuckNoteGuard() {
  const bool present = hostPresent();
  if (hostRestarted) {
    hostRestarted = false;
    releaseSidecarNotes();
    silenceSidecarOutputs();
  } else if (hostWasPresent && !present) {
    releaseSidecarNotes();
  } else if (!hostWasPresent && present && hostEverPresent) {
    silenceSidecarOutputs();
  }
  hostWasPresent = present;
  hostEverPresent |= present;
}

void pumpHostIn() {
  uint16_t budget = LINK_RX_BUDGET;
  if (!outputsHaveRoom()) return; // backlog builds, so the host brain is asked to pause
  while (budget-- && linkSerial.available() > 0) {
    if (!linkReader.feed((uint8_t)linkSerial.read())) continue;
    uint8_t packet[4];
    uint8_t port = decodeLinkFrame(linkReader.frame, packet);

    if (port == LINK_PORT_CONTROL) {
      uint8_t type = packet[0] & 0x0F;
      if (type == CONTROL_PORT_MAP) {
        // A device unplugged mid-note never sends its Note Offs, so its held notes (and any arp they
        // feed) are released here.
        const uint8_t gone = hostPortMask & ~packet[1];
        hostPortMask = packet[1];
        for (uint8_t p = 1; p <= 4; ++p) if (gone & (1 << (p - 1))) releaseSidecarNotes(p);
        hostFwVersion = packet[2];
        hostDropCount = packet[3];
        hostLastStatusMs = millis();
      } else if (type == CONTROL_HOST_RESTART) {
        hostRestartReason = packet[1];
        hostRestartPort = packet[2];
        if (hostRestartSeen && packet[3] != hostRestartCount) hostRestarted = true; // any change, a power cycle resets it to 0
        hostRestartCount = packet[3];
        hostRestartSeen = true;
      } else if (type == CONTROL_FLOW) {
        hostAskedPause = packet[1] == 1;
        hostFlowMs = millis();
      }
      continue;
    }
    if (!hostPresent()) continue; // MIDI only counts once the sidecar has identified itself
    if (port >= 1 && port <= 4) {
      currentHostPort = port;
      handleUsbMidiPacket(DEV_HOST, hostSysex[port], packet);
    } else if (port == LINK_PORT_DIN) {
      handleUsbMidiPacket(DEV_TRS2, hostSysex[port], packet);
    }
    // Other ports are reserved (10-14: C3 BLE MIDI, planned).
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
  arpPanic(); // otherwise a held arp keeps stepping right after
  // Deliberately NOT sending an individual Note Off for every note on every channel here — that's
  // 16 x 128 x 2 ports = 4096+ messages, which over a 31250-baud DIN line takes close to 2 seconds
  // of real transmit time and makes "panic" feel broken instead of instant. CC 120 (All Sound Off)
  // and CC 123 (All Notes Off) already silence essentially all MIDI gear immediately; the full
  // per-note sweep is a legacy fallback for very old non-compliant synths that's not worth trading
  // away a responsive panic button for. 96 messages total here, well under 100ms even on DIN.
  for (uint8_t channel = 0; channel < 16; ++channel) {
    sendToOutputs(availablePorts(), 0xB0, channel, 121, 0, 3); // Reset All Controllers
    sendToOutputs(availablePorts(), 0xB0, channel, 123, 0, 3); // All Notes Off
    sendToOutputs(availablePorts(), 0xB0, channel, 120, 0, 3); // All Sound Off
    serviceLeds();
  }
  drainTrsOut(); // panic goes out before the confirmation flashes
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
      sendToOutputs(availablePorts(), 0xE0, channel, centered & 0x7F, (centered >> 7) & 0x7F, 3);
    } else {
      sendToOutputs(availablePorts(), 0xB0, channel, 1, (uint8_t)rampValue, 3);
    }
  }

  if (done && !rampRising) rampActive = false;
}

void sendCc(uint8_t value) {
  uint8_t channel = resolveButtonChannel();
  sendToOutputs(availablePorts(), 0xB0, channel, globalSettings.buttonParamA, value, 3);
}

// Note momentary/toggle reuse buttonParamA as the note number (0-127, same field cc actions use
// for the cc number) and buttonParamB as the channel — no protocol/struct changes needed.
void sendNote(bool on) {
  uint8_t channel = resolveButtonChannel();
  uint8_t note = globalSettings.buttonParamA;
  uint8_t statusHi = on ? 0x90 : 0x80;
  uint8_t velocity = on ? 127 : 0;
  sendToOutputs(availablePorts(), statusHi, channel, note, velocity, 3);
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
    case BTN_TAP_TEMPO: tapTempo(); break;
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
  Serial1.setFIFOSize(256); // room for TRS/DIN input while the loop holds off reading it
  Serial1.begin(DIN_BAUD);

  linkSerial.begin(LINK_BAUD);
  gpio_pull_up(PIN_LINK_RX); // holds the line idle with no host brain attached, so it reads nothing

  if (factoryResetRequested) {
    setFactoryDefaults();
    saveToFlash();
  } else {
    loadFromFlash();
  }
  applyBrightnessToLeds();
  resetButtonRuntimeState();

  clockEngine.begin(clockConfig(), time_us_64());
  startupSequence();
  rp2040.wdt_begin(WATCHDOG_MS); // last resort: a hang anywhere reboots instead of staying frozen
}

void loop() {
  rp2040.wdt_reset();

  // The OTA bootloader installs the staged firmware during this reboot, then starts it.
  if (fwRebootPending && (int32_t)(millis() - fwRebootAtMs) >= 0) rp2040.reboot();

#ifdef TINYUSB_NEED_POLLING_TASK
  TinyUSBDevice.task();
#endif

  pumpUsbIn();
  pumpDinIn();
  pumpHostIn();
  serviceStuckNoteGuard();
  serviceButton();
  serviceRamp();
  serviceLeds();
  serviceClock();
  serviceArps();
  serviceLearnTimeout();
  pumpTrsOut();
  pumpUsbOut();
  serviceLinkFlow();
  pumpLinkOut();
}
