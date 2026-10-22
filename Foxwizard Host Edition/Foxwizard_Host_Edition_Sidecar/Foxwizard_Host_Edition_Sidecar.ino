/*
  Foxwizard_Host_Edition_Sidecar.ino by woz.lol
  FoxWizard Host Edition host brain: RP2040 native USB host for a 4-port hub, plus one DIN/TRS
  MIDI IN and five DIN/TRS MIDI OUTs, all bridged to the FoxWizard main brain over one serial link.

  This brain does no routing. Everything it hears goes to the main brain tagged with the port it
  came from, and everything the main brain sends comes back tagged with the port it goes to. See
  ../README.md for the link frame format and the buffering rules.

  Wiring:
  - GP28 = link TX to main brain GP27
  - GP29 = link RX from main brain GP26
  - GP4  = DIN/TRS MIDI OUT 1 (UART1 TX)
  - GP5  = DIN/TRS MIDI IN (UART1 RX)
  - GP6, GP7, GP8, GP9 = DIN/TRS MIDI OUT 2-5 (PIO serial TX)
  - GP10, GP11, GP12, GP13 = hub port 1-4 status lights, active HIGH
  - Native USB = USB host, to the hub upstream port (or an OTG adapter for testing)

  Arduino setup:
  - Board: Waveshare RP2040-Zero
  - Tools -> USB Stack = Adafruit TinyUSB Host (native)

  Status lights: on while a USB MIDI device is mounted on that hub port, blink off briefly on traffic.

  Diagnostic light, the RP2040-Zero's onboard RGB LED on GP16 (nothing is on GP16 on the sidecar
  board). Off by default, no baseline color, same no-blink ethic as the main brain's own activity
  lights (see ../Mini_MIDI_Multitool/Mini_MIDI_Multitool.ino):
  - blue: fades up on any MIDI activity through the sidecar (either hub USB port, either DIN/TRS
    direction) and fades back down when it stops, 1 s up and 1/2 s down, the same curve as the main
    brain's. Follows the main brain's own LED brightness knob.
  - white: the same fade, instead for a new USB device (MIDI or not) just being plugged in.
  - purple, instant, no fade: shown for two flashes right after a restart caused by a USB fault, see
    Recovery in ../README.md. Nothing to show live, a fault reboots at once.
  - purple, blinking fast: nothing heard from the main brain for over 1 second (link wiring, ground,
    or the main brain isn't running the Host Edition firmware). The one deliberate exception to the
    no-blink ethic, since it means there's nothing else to show.
*/

#include <Arduino.h>
// TinyUSB is vendored in src/, with the RP2040 host fixes this sidecar needs, see
// src/Adafruit_TinyUSB/VENDORED.md. It compiles the same on any machine, whatever TinyUSB is installed.
#include "src/Adafruit_TinyUSB/Adafruit_TinyUSB.h"
#include "src/Adafruit_TinyUSB/foxwizard_usb_fault.h"
#include <hardware/pio.h>
#include <hardware/clocks.h>
#include <hardware/watchdog.h>
#include "foxwizard_link.h"

#ifndef USE_TINYUSB_HOST
#error Set Tools -> USB Stack -> Adafruit TinyUSB Host (native)
#endif

// ============================================================================================
// Constants
// ============================================================================================
constexpr uint8_t PIN_LINK_TX = 28;
constexpr uint8_t PIN_LINK_RX = 29;
constexpr uint8_t PIN_DIN_OUT1 = 4;
constexpr uint8_t PIN_DIN_IN = 5;
constexpr uint8_t PIN_DIN_OUT2 = 6;
constexpr uint8_t PIN_DIN_OUT3 = 7;
constexpr uint8_t PIN_DIN_OUT4 = 8;
constexpr uint8_t PIN_DIN_OUT5 = 9;
constexpr uint8_t PIN_PORT_LED[4] = {10, 11, 12, 13};
constexpr uint8_t PIN_STATUS_RGB = 16;
constexpr uint8_t PIN_TRS_BOARD = 14; // floating (its internal pull-up): a bare sidecar module, USB
                                       // hub ports only, no TRS. Grounded: a board with TRS 2-6 added.
                                       // Tells the main brain whether TRS 2-6 physically exist.

constexpr uint32_t DIN_BAUD = 31250;
constexpr uint8_t HOST_FW_VERSION = 7; // 3: the host report. 4: chained hubs, hub and spare device reporting. 5: 64 endpoint slots, stack trace events. 6: 7 USB Host slots. 7: steadier TRS board pin read

constexpr uint8_t HUB_PORTS = HOST_SLOTS; // USB Host slots, 7 from host firmware 6
constexpr uint8_t LED_PORTS = 4;           // only the first four have a light on the board
constexpr uint8_t DIN_OUTS = 5;

constexpr uint32_t PORT_MAP_HEARTBEAT_MS = 500;
constexpr uint32_t LED_BLINK_MS = 25;
constexpr uint8_t STATUS_RGB_LEVEL = 40;       // diagnostic light brightness ceiling, 0-255, before the brightness knob
constexpr uint32_t STATUS_FADE_IN_MS = 1000;   // same curve as the main brain's activity lights
constexpr uint32_t STATUS_FADE_OUT_MS = 500;
constexpr uint32_t STATUS_HOLD_MS = 12;        // a re-trigger inside this window doesn't restart the fade-in
constexpr uint32_t STATUS_FAULT_FLASH_MS = 300;
constexpr uint32_t STATUS_LINK_SILENT_MS = 1000; // no main brain heard from this long: blink purple
constexpr uint32_t STATUS_LINK_BLINK_MS = 150;   // fast, on/off, no fade
constexpr uint32_t USB_STALL_MS = 1000;   // a device that accepts nothing this long stops holding up the others
constexpr uint32_t USB_WEDGE_MS = 3000;   // still nothing this long: the USB host is wedged, reboot to recover
constexpr uint32_t WATCHDOG_MS = 2000;    // reboots if the loop ever stops running this long, a hot-plug hang included
constexpr uint16_t LINK_RX_BUDGET = 1024; // link bytes read per loop
constexpr uint16_t USB_READ_PACKETS = 16; // one full-speed bulk packet

// Frames from the main brain wait here until every output they go to has room.
constexpr uint16_t FROM_MAIN_FRAMES = 1024;
constexpr uint16_t FROM_MAIN_PAUSE_AT = 512;  // ask the main brain to pause above this
constexpr uint16_t FROM_MAIN_RESUME_AT = 256; // and to resume below this

// Bytes per USB-MIDI packet for each CIN, 0 = reserved/ignored.
const uint8_t CIN_LENGTH[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};

// ============================================================================================
// Types
// ============================================================================================
struct HubPort {
  bool mounted = false;
  uint8_t daddr = 0; // the USB device in this slot, so another MIDI interface of it isn't a second port
  bool writable = false; // device has a MIDI OUT endpoint
  uint8_t midiIdx = 0xFF;
  MidiQueue<MidiPacket, 32, 0> rt;
  MidiQueue<MidiPacket, 256, 32> normal;
  uint32_t lastProgressMs = 0;
  uint32_t blinkUntilMs = 0;
};

struct DinOut {
  HardwareSerial *serial;
  MidiQueue<MidiPacket, 32, 0> rt;
  MidiQueue<MidiPacket, 256, 32> normal;
  MidiPacket current;
  uint8_t pos = 0;
  uint8_t len = 0;
};

struct DinInParser {
  uint8_t running = 0;
  uint8_t need = 0;
  uint8_t have = 0;
  uint8_t data[2] = {0, 0};
  bool inSysex = false;
  uint8_t sx[3];
  uint8_t sxLen = 0;
};

// ============================================================================================
// Globals
// ============================================================================================
Adafruit_USBH_Host USBHost;
// The diagnostic light is one WS2812 LED, driven by the 4-instruction ws2812 PIO program from
// pico-examples (800 kHz, 10 PIO cycles per bit).
const uint16_t WS2812_INSTRUCTIONS[] = {0x6221, 0x1123, 0x1400, 0xa442};
const pio_program_t WS2812_PROGRAM = {WS2812_INSTRUCTIONS, 4, -1};
PIO statusRgbPio = nullptr;
uint statusRgbSm = 0;
uint8_t usbDevicesMounted = 0; // every device except hubs, MIDI or not
uint32_t lastStatusRgb = 0xFFFFFFFF;

// One fade engine for the status light, ported from the main brain's activity LEDs (same phases,
// same timing), generalized to pick a color per trigger instead of driving one fixed hue.
enum StatusColor : uint8_t { STATUS_BLUE = 0, STATUS_WHITE = 1, STATUS_PURPLE = 2 };
enum StatusPhase : uint8_t { STATUS_OFF = 0, STATUS_RISING = 1, STATUS_ON = 2, STATUS_FALLING = 3 };
struct StatusLed {
  StatusPhase phase = STATUS_OFF;
  uint32_t phaseStartMs = 0;
  uint32_t lastActivityMs = 0;
  uint8_t currentLevel = 0;
  uint8_t fadeStartLevel = 0;
  bool activitySeen = false;
  uint8_t color = STATUS_BLUE;
  uint8_t pendingColor = STATUS_BLUE;
};
StatusLed statusLed;
uint8_t ledBrightnessStep = 10; // 0-10, from the main brain's brightness knob (CONTROL_BRIGHTNESS)
uint32_t lastMainFrameMs = 0; // any frame heard from the main brain, link-down detection only

uint32_t rgbColor(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b; }

// ============================================================================================
// Black box. Kept in RAM the startup code leaves alone, so it survives every restart but a power
// cycle. It's kept up as things happen, so even a watchdog restart (nothing gets to run first)
// leaves it telling what each port was doing and where the loop was. At the next start it becomes
// the host report (layout in foxwizard_link.h), sent to the main brain for Patchling to show.
// ============================================================================================
constexpr uint32_t BLACK_BOX_MAGIC = 0xB1AC0B04;
struct BlackBoxPort {
  uint16_t vid, pid;
  uint32_t sent, received;  // packets the device accepted, and packets read from it
  uint8_t last[4];          // the last packet it accepted
  uint8_t stuck[4];         // at a recovery restart: the packet waiting at the front of its queue
  uint16_t queued;          // at a recovery restart: packets waiting
  uint16_t idleMs;          // at a recovery restart: time since it last took anything
  bool mounted, writable, skipLogged;
  uint8_t hubPort, tier; // where it's really plugged in: its hub's port number, and 0 straight in, 1 first hub, 2 behind another hub
};
struct BlackBoxEvent { uint32_t ms; uint8_t type, port, a, b; };
struct BlackBox {
  uint32_t magic;
  uint32_t uptimeMs;
  uint32_t maxLoopUs;
  uint32_t stallDrops;
  uint8_t stage, devices, eventHead, eventCount;
  uint8_t jacks; // downstream jacks of the hubs attached, 0 with no hub
  BlackBoxPort port[HUB_PORTS];
  BlackBoxEvent events[HOST_REPORT_EVENTS];
};
BlackBox __uninitialized_ram(blackBox);
uint8_t hostReport[HOST_REPORT_BYTES];
bool haveHostReport = false;
uint8_t reportPassesLeft = 0; // passes still to send, one per heartbeat
uint16_t reportNext = 0xFFFF;  // next frame of the pass being sent, 0xFFFF none

void logEvent(uint8_t type, uint8_t port, uint8_t a = 0, uint8_t b = 0) {
  BlackBoxEvent &e = blackBox.events[blackBox.eventHead];
  e = BlackBoxEvent{millis(), type, port, a, b};
  blackBox.eventHead = (blackBox.eventHead + 1) % HOST_REPORT_EVENTS;
  if (blackBox.eventCount < HOST_REPORT_EVENTS) ++blackBox.eventCount;
}

void put16(uint8_t *p, uint32_t v) { const uint16_t c = v > 0xFFFF ? 0xFFFF : v; p[0] = c & 0xFF; p[1] = c >> 8; }
void put32(uint8_t *p, uint32_t v) { for (uint8_t i = 0; i < 4; ++i) p[i] = (v >> (8 * i)) & 0xFF; }

void buildHostReport(uint8_t reason, uint8_t port, uint32_t count) {
  uint8_t *r = hostReport;
  memset(r, 0, HOST_REPORT_BYTES);
  r[0] = HOST_REPORT_LAYOUT;
  r[1] = reason;
  r[2] = port;
  r[3] = blackBox.stage;
  put32(r + 4, blackBox.uptimeMs);
  put16(r + 8, blackBox.maxLoopUs);
  put16(r + 10, blackBox.stallDrops);
  r[12] = blackBox.devices;
  r[13] = count > 255 ? 255 : count;
  r[14] = HOST_FW_VERSION;
  r[15] = blackBox.jacks;
  for (uint8_t p = 0; p < HUB_PORTS; ++p) {
    const BlackBoxPort &b = blackBox.port[p];
    uint8_t *q = r + 16 + p * HOST_REPORT_PORT_BYTES;
    q[0] = (b.mounted ? 1 : 0) | (b.writable ? 2 : 0) | ((b.hubPort & 7) << 2) | ((b.tier & 3) << 5);
    put16(q + 1, b.vid);
    put16(q + 3, b.pid);
    put32(q + 5, b.sent);
    put32(q + 9, b.received);
    memcpy(q + 13, b.last, 4);
    memcpy(q + 17, b.stuck, 4);
    put16(q + 21, b.queued);
    q[23] = b.idleMs / 100 > 255 ? 255 : b.idleMs / 100;
  }
  const uint8_t n = blackBox.eventCount > HOST_REPORT_EVENTS ? HOST_REPORT_EVENTS : blackBox.eventCount;
  for (uint8_t i = 0; i < n; ++i) {
    const BlackBoxEvent &e = blackBox.events[(blackBox.eventHead + HOST_REPORT_EVENTS - n + i) % HOST_REPORT_EVENTS];
    uint8_t *q = r + 16 + HUB_PORTS * HOST_REPORT_PORT_BYTES + i * 8;
    put32(q, e.ms);
    q[4] = e.type; q[5] = e.port; q[6] = e.a; q[7] = e.b;
  }
}

void resetBlackBox() {
  memset(&blackBox, 0, sizeof(blackBox));
  blackBox.magic = BLACK_BOX_MAGIC;
}

// A recovery restart notes each port's queue first, which a watchdog restart can't.
void snapshotPortsForRestart();

// Restart reporting. A recovery reboot tags watchdog scratch register 0 with its reason first.
// Scratch registers survive a watchdog reboot but not power-up. A watchdog reboot without a tag
// means the loop stopped: usually the USB driver halting on a device error.
constexpr uint32_t RESTART_TAG = 0xF0C5A000;
uint8_t restartReason = RESTART_NONE;
uint8_t restartPort = 0;
uint32_t restartCount = 0;

void readRestartReason() {
  const bool boxValid = blackBox.magic == BLACK_BOX_MAGIC;
  if (watchdog_caused_reboot()) {
    const uint32_t tag = watchdog_hw->scratch[0];
    if ((tag & 0xFFFFF000) == RESTART_TAG) {
      restartReason = (tag >> 8) & 0x0F;
      restartPort = tag & 0xFF;
    } else {
      restartReason = RESTART_HANG;
    }
    restartCount = watchdog_hw->scratch[1] + 1;
  }
  watchdog_hw->scratch[0] = 0;
  watchdog_hw->scratch[1] = restartCount;
  if (restartReason != RESTART_NONE && boxValid) {
    buildHostReport(restartReason, restartPort, restartCount);
    haveHostReport = true;
    reportPassesLeft = 6; // three seconds of heartbeats, in case the main brain is still starting
  }
  resetBlackBox();
  logEvent(HOST_EVENT_BOOT, 0, restartReason);
}

void recoveryRestart(uint8_t reason, uint8_t port) {
  snapshotPortsForRestart();
  logEvent(HOST_EVENT_RESTART, port, reason);
  watchdog_hw->scratch[0] = RESTART_TAG | ((uint32_t)reason << 8) | port;
  rp2040.reboot();
}

// Things the USB stack did that nothing else reports (see foxwizard_usb_fault.h).
extern "C" void foxwizard_usb_trace(uint8_t code, uint8_t a, uint8_t b) {
  logEvent(HOST_EVENT_TRACE, code, a, b);
}

// USB driver faults, from the vendored driver where stock TinyUSB halts with panic(). Called from
// the USB interrupt. Restarts at once with the fault as the reason, instead of halting until the
// watchdog fires.
extern "C" void foxwizard_usb_fault(uint8_t fault) {
  logEvent(HOST_EVENT_FAULT, 0, fault);
  switch (fault) {
    case FOXWIZARD_FAULT_DATA_SEQ: recoveryRestart(RESTART_DATA_SEQ, 0); break;
    case FOXWIZARD_FAULT_STALL: recoveryRestart(RESTART_STALL, 0); break;
    default: recoveryRestart(RESTART_BUF_CTRL, 0); break;
  }
}

SerialPIO dinOut2Serial(PIN_DIN_OUT2, NOPIN);
SerialPIO dinOut3Serial(PIN_DIN_OUT3, NOPIN);
SerialPIO dinOut4Serial(PIN_DIN_OUT4, NOPIN);
SerialPIO dinOut5Serial(PIN_DIN_OUT5, NOPIN);

DinOut dinOuts[DIN_OUTS];
HubPort hubPorts[HUB_PORTS];
uint8_t midiIdxToPort[CFG_TUH_MIDI]; // 0 = not one of ours, else hub port 1-4

MidiQueue<LinkFrame, 64, 0> toMainRt;
MidiQueue<LinkFrame, 2048, 128> toMainNormal;
MidiQueue<LinkFrame, FROM_MAIN_FRAMES, 64> fromMain;

LinkFrame linkTxFrame;
uint8_t linkTxPos = 5; // 5 = no frame in progress
LinkFrameReader linkReader;

bool mainAskedPause = false;
uint32_t mainFlowMs = 0;
bool pausingMain = false;
uint32_t lastFlowSentMs = 0;

DinInParser dinIn;

bool portMapDirty = true;
bool trsBoard = false; // read once in setup(): PIN_TRS_BOARD grounded, so TRS 2-6 are wired
uint32_t lastPortMapMs = 0;
uint32_t usbStallDrops = 0;


// ============================================================================================
// Link out to the main brain. Realtime and control frames always go first. Ordinary frames
// stop while the main brain has asked for a pause and wait in toMainNormal.
// ============================================================================================
void sendToMain(uint8_t port, const uint8_t pkt[4]) {
  LinkFrame f;
  encodeLinkFrame(port, pkt, f);
  if (port == LINK_PORT_CONTROL || isRealtimePacket(pkt)) toMainRt.push(f, true);
  else toMainNormal.push(f, isCriticalPacket(pkt));
}

bool mainPaused() {
  return mainAskedPause && millis() - mainFlowMs < FLOW_EXPIRY_MS;
}

void pumpLinkOut() {
  while (true) {
    if (linkTxPos >= 5) {
      LinkFrame *next = toMainRt.peek();
      if (next) {
        linkTxFrame = *next;
        toMainRt.pop();
      } else if (!mainPaused() && (next = toMainNormal.peek()) != nullptr) {
        linkTxFrame = *next;
        toMainNormal.pop();
      } else {
        return;
      }
      linkTxPos = 0;
    }
    while (linkTxPos < 5 && Serial1.availableForWrite() > 0) Serial1.write(linkTxFrame.b[linkTxPos++]);
    if (linkTxPos < 5) return; // UART FIFO full, carry on next loop
  }
}

uint32_t totalDrops() {
  uint32_t n = toMainRt.dropped() + toMainNormal.dropped() + fromMain.dropped() + usbStallDrops;
  for (const HubPort &hp : hubPorts) n += hp.rt.dropped() + hp.normal.dropped();
  for (const DinOut &out : dinOuts) n += out.rt.dropped() + out.normal.dropped();
  return n;
}

void serviceControlOut() {
  const uint32_t now = millis();

  if (portMapDirty || now - lastPortMapMs >= PORT_MAP_HEARTBEAT_MS) {
    uint8_t mask = 0;
    for (uint8_t p = 0; p < HUB_PORTS; ++p) if (hubPorts[p].mounted) mask |= 1 << hostMaskBit(p + 1);
    uint32_t drops = totalDrops();
    if (trsBoard) mask |= 0x10; // bit 4: this board has TRS 2-6 wired, bits 0-3 stay hub ports only
    uint8_t pkt[4] = {CONTROL_PORT_MAP, mask, HOST_FW_VERSION, (uint8_t)(drops > 127 ? 127 : drops)};
    sendToMain(LINK_PORT_CONTROL, pkt);
    uint8_t restart[4] = {CONTROL_HOST_RESTART, restartReason, restartPort, (uint8_t)(restartCount > 127 ? 127 : restartCount)};
    sendToMain(LINK_PORT_CONTROL, restart);
    blackBox.jacks = hubJacks();
    uint8_t hub[4] = {CONTROL_HOST_HUB, blackBox.jacks, hubCount(), waitingDeviceCount()};
    sendToMain(LINK_PORT_CONTROL, hub);
    portMapDirty = false;
    lastPortMapMs = now;
    if (haveHostReport && reportPassesLeft && reportNext == 0xFFFF) { --reportPassesLeft; reportNext = 0; }
  }
  // The report goes a little at a time, leaving room for clock and control frames.
  while (reportNext < HOST_REPORT_BYTES / 2 && toMainRt.freeSlots(false) > 16) {
    uint8_t pkt[4] = {CONTROL_HOST_REPORT, (uint8_t)reportNext, hostReport[reportNext * 2], hostReport[reportNext * 2 + 1]};
    sendToMain(LINK_PORT_CONTROL, pkt);
    if (++reportNext >= HOST_REPORT_BYTES / 2) reportNext = 0xFFFF;
  }

  // Bytes still in the UART buffer count as 5 to a frame.
  uint16_t backlog = fromMain.size() + Serial1.available() / 5;
  bool wantPause = pausingMain ? backlog > FROM_MAIN_RESUME_AT : backlog > FROM_MAIN_PAUSE_AT;
  if (wantPause != pausingMain || now - lastFlowSentMs >= FLOW_REPEAT_MS) {
    pausingMain = wantPause;
    uint8_t pkt[4] = {CONTROL_FLOW, (uint8_t)(wantPause ? 1 : 0), 0, 0};
    sendToMain(LINK_PORT_CONTROL, pkt);
    lastFlowSentMs = now;
  }
}

// ============================================================================================
// Outputs
// ============================================================================================
bool usbTarget(uint8_t port) {
  const HubPort &hp = hubPorts[port - 1];
  return hp.mounted && hp.writable;
}

bool usbStalled(const HubPort &hp) {
  return !hp.normal.empty() && millis() - hp.lastProgressMs > USB_STALL_MS;
}

// Realtime goes straight to each output's realtime queue, never behind other traffic.
void dispatchRealtime(uint8_t port, const uint8_t pkt[4]) {
  MidiPacket p;
  memcpy(p.b, pkt, 4);
  for (uint8_t i = 1; i <= HUB_PORTS; ++i) {
    if ((port == LINK_PORT_ALL || port == hostLinkPort(i)) && usbTarget(i)) {
      hubPorts[i - 1].rt.push(p, true);
      hubPorts[i - 1].blinkUntilMs = millis() + LED_BLINK_MS;
    }
  }
  for (uint8_t i = 0; i < DIN_OUTS; ++i) {
    if (port == LINK_PORT_ALL || port == LINK_PORT_DIN + i) dinOuts[i].rt.push(p, true);
  }
}

// Moves frames from fromMain to the outputs in order. A frame waits until every output it goes
// to has room, so nothing is dropped while outputs are just slow. Only a USB device that has
// accepted nothing for USB_STALL_MS is skipped, so one stuck device can't hold up the rest.
void dispatchFromMain() {
  LinkFrame *f;
  while ((f = fromMain.peek()) != nullptr) {
    uint8_t pkt[4];
    uint8_t port = decodeLinkFrame(*f, pkt);
    bool critical = isCriticalPacket(pkt);
    bool toUsb[HUB_PORTS] = {};
    bool toDin[DIN_OUTS] = {false, false, false, false, false};

    for (uint8_t i = 1; i <= HUB_PORTS; ++i) toUsb[i - 1] = (port == LINK_PORT_ALL || port == hostLinkPort(i)) && usbTarget(i);
    for (uint8_t i = 0; i < DIN_OUTS; ++i) toDin[i] = (port == LINK_PORT_ALL || port == LINK_PORT_DIN + i);

    bool ready = true;
    for (uint8_t i = 0; i < HUB_PORTS && ready; ++i) {
      if (!toUsb[i]) continue;
      HubPort &hp = hubPorts[i];
      if (hp.normal.freeSlots(critical) > 0) continue;
      if (usbStalled(hp)) {
        toUsb[i] = false;
        ++usbStallDrops;
        blackBox.stallDrops = usbStallDrops;
        if (!blackBox.port[i].skipLogged) { blackBox.port[i].skipLogged = true; logEvent(HOST_EVENT_SKIP, i + 1); }
        continue;
      }
      ready = false;
    }
    for (uint8_t i = 0; i < DIN_OUTS && ready; ++i) {
      if (toDin[i] && dinOuts[i].normal.freeSlots(critical) == 0) ready = false;
    }
    if (!ready) return; // try again next loop, the outputs are draining

    MidiPacket p;
    memcpy(p.b, pkt, 4);
    for (uint8_t i = 0; i < HUB_PORTS; ++i) {
      if (!toUsb[i]) continue;
      hubPorts[i].normal.push(p, critical);
      hubPorts[i].blinkUntilMs = millis() + LED_BLINK_MS;
    }
    for (uint8_t i = 0; i < DIN_OUTS; ++i) if (toDin[i]) dinOuts[i].normal.push(p, critical);
    fromMain.pop();
  }
}

// Realtime bytes may sit between the bytes of any other message on DIN, so they go first.
void pumpDinOuts() {
  for (DinOut &out : dinOuts) {
    bool wrote = false;
    while (out.serial->availableForWrite() > 0) {
      MidiPacket *rt = out.rt.peek();
      if (rt) {
        out.serial->write(rt->b[1]);
        out.rt.pop();
        wrote = true;
        continue;
      }
      if (out.pos < out.len) {
        out.serial->write(out.current.b[1 + out.pos++]);
        wrote = true;
        continue;
      }
      MidiPacket *next = out.normal.peek();
      if (!next) break;
      out.current = *next;
      out.normal.pop();
      out.pos = 0;
      out.len = CIN_LENGTH[out.current.b[0] & 0x0F];
    }
    if (wrote) noteStatusActivity(STATUS_BLUE);
  }
}

void pumpUsbOuts() {
  const uint32_t now = millis();
  for (HubPort &hp : hubPorts) {
    if (!hp.mounted || !hp.writable) continue;
    bool wrote = false;
    for (int q = 0; q < 2; ++q) {
      MidiPacket *pkt;
      while ((pkt = (q == 0 ? hp.rt.peek() : hp.normal.peek())) != nullptr) {
        if (!tuh_midi_packet_write(hp.midiIdx, pkt->b)) break; // TinyUSB buffer full, retry next loop
        BlackBoxPort &bb = blackBox.port[&hp - hubPorts];
        memcpy(bb.last, pkt->b, 4);
        ++bb.sent;
        bb.skipLogged = false;
        if (q == 0) hp.rt.pop(); else hp.normal.pop();
        wrote = true;
      }
    }
    if (wrote) tuh_midi_write_flush(hp.midiIdx);
    if (wrote) noteStatusActivity(STATUS_BLUE);
    if (wrote || hp.normal.empty()) hp.lastProgressMs = now;
    // Queued output that never drains means the USB host has wedged (ARPnMIDI's MAX3421E host
    // showed the same sign). A reboot re-enumerates every device in about a second.
    if (now - hp.lastProgressMs > USB_WEDGE_MS) recoveryRestart(RESTART_WEDGE, (uint8_t)(&hp - hubPorts) + 1);
  }
}

// ============================================================================================
// Link input from the main brain
// ============================================================================================
void snapshotPortsForRestart() {
  const uint32_t now = millis();
  for (uint8_t p = 0; p < HUB_PORTS; ++p) {
    HubPort &hp = hubPorts[p];
    BlackBoxPort &bb = blackBox.port[p];
    MidiPacket *head = hp.rt.peek();
    if (!head) head = hp.normal.peek();
    if (head) memcpy(bb.stuck, head->b, 4); else memset(bb.stuck, 0, 4);
    bb.queued = hp.rt.size() + hp.normal.size();
    bb.idleMs = now - hp.lastProgressMs > 0xFFFF ? 0xFFFF : now - hp.lastProgressMs;
  }
}

void pumpLinkIn() {
  uint16_t budget = LINK_RX_BUDGET;
  while (budget-- && Serial1.available() > 0) {
    // With fromMain full, bytes stay in the 4 KB UART buffer instead of being dropped.
    if (fromMain.freeSlots(false) == 0) return;
    if (!linkReader.feed((uint8_t)Serial1.read())) continue;
    uint8_t pkt[4];
    uint8_t port = decodeLinkFrame(linkReader.frame, pkt);
    lastMainFrameMs = millis();

    if (port == LINK_PORT_CONTROL) {
      const uint8_t type = pkt[0] & 0x0F;
      if (type == CONTROL_FLOW) {
        mainAskedPause = pkt[1] == 1;
        mainFlowMs = millis();
      } else if (type == CONTROL_BRIGHTNESS) {
        ledBrightnessStep = pkt[1] > 10 ? 10 : pkt[1];
      }
      continue;
    }
    if (isRealtimePacket(pkt)) {
      dispatchRealtime(port, pkt);
      continue;
    }
    fromMain.push(linkReader.frame, isCriticalPacket(pkt));
  }
}

// ============================================================================================
// DIN IN -> USB-MIDI packets -> link
// ============================================================================================
void sendDinInPacket(uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3) {
  uint8_t pkt[4] = {cin, b1, b2, b3};
  sendToMain(LINK_PORT_DIN, pkt);
  noteStatusActivity(STATUS_BLUE);
}

uint8_t dinDataLength(uint8_t status) {
  if (status >= 0xF0) {
    switch (status) {
      case 0xF1: case 0xF3: return 1;
      case 0xF2: return 2;
      default: return 0;
    }
  }
  return ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) ? 1 : 2;
}

void processDinInByte(uint8_t b) {
  if (b >= 0xF8) {
    sendDinInPacket(0x0F, b, 0, 0);
    return;
  }

  if (b == 0xF0) {
    dinIn.inSysex = true;
    dinIn.sx[0] = 0xF0;
    dinIn.sxLen = 1;
    dinIn.running = 0;
    return;
  }

  if (b == 0xF7) {
    if (!dinIn.inSysex) return;
    dinIn.sx[dinIn.sxLen++] = 0xF7;
    sendDinInPacket(0x04 + dinIn.sxLen, dinIn.sx[0], dinIn.sxLen > 1 ? dinIn.sx[1] : 0, dinIn.sxLen > 2 ? dinIn.sx[2] : 0);
    dinIn.inSysex = false;
    dinIn.sxLen = 0;
    return;
  }

  if (dinIn.inSysex) {
    if (b & 0x80) {
      dinIn.inSysex = false; // any other status aborts SysEx, then is handled below
      dinIn.sxLen = 0;
    } else {
      dinIn.sx[dinIn.sxLen++] = b;
      if (dinIn.sxLen == 3) {
        sendDinInPacket(0x04, dinIn.sx[0], dinIn.sx[1], dinIn.sx[2]);
        dinIn.sxLen = 0;
      }
      return;
    }
  }

  if (b & 0x80) {
    dinIn.running = b;
    dinIn.need = dinDataLength(b);
    dinIn.have = 0;
    if (b >= 0xF0) {
      if (b == 0xF6) sendDinInPacket(0x05, b, 0, 0); // Tune Request
      if (dinIn.need == 0) dinIn.running = 0;        // F4, F5, F6 carry no data and no running status
    }
    return;
  }

  if (!dinIn.running) return;
  dinIn.data[dinIn.have++] = b;
  if (dinIn.have < dinIn.need) return;
  dinIn.have = 0;

  uint8_t status = dinIn.running;
  uint8_t d1 = dinIn.data[0];
  uint8_t d2 = (dinIn.need > 1) ? dinIn.data[1] : 0;
  if (status >= 0xF0) {
    sendDinInPacket(dinIn.need == 1 ? 0x02 : 0x03, status, d1, d2);
    dinIn.running = 0;
  } else {
    sendDinInPacket(status >> 4, status, d1, d2);
  }
}

void pumpDinIn() {
  while (Serial2.available() > 0) processDinInByte((uint8_t)Serial2.read());
}

// ============================================================================================
// USB host: hub port lookup and MIDI traffic
// ============================================================================================
void resetHubPort(HubPort &hp) {
  hp.mounted = false;
  hp.daddr = 0;
  hp.writable = false;
  hp.midiIdx = 0xFF;
  hp.rt.clear();
  hp.normal.clear();
  hp.lastProgressMs = millis();
  hp.blinkUntilMs = 0;
}

// MIDI interfaces that came while all four slots were taken. They wait, and take the next one to free.
struct WaitingMidi { bool used; uint8_t daddr; bool writable; uint8_t cables; };
WaitingMidi waitingMidi[CFG_TUH_MIDI];

uint8_t waitingDeviceCount() {
  uint16_t seen = 0;
  uint8_t n = 0;
  for (const WaitingMidi &w : waitingMidi) {
    if (!w.used || (seen & (1u << (w.daddr & 15)))) continue;
    seen |= 1u << (w.daddr & 15);
    ++n;
  }
  return n;
}

// Puts a MIDI interface in a slot. False if all four are taken.
bool placeMidi(uint8_t idx, uint8_t daddr, bool writable, uint8_t cables) {
  tuh_bus_info_t info;
  if (!tuh_bus_info_get(daddr, &info)) return true; // already gone, nothing to wait for
  for (const HubPort &other : hubPorts) if (other.mounted && other.daddr == daddr) return true; // its other MIDI interface

  // The slot (USB Host 1-4). A device on the first hub keeps its jack number, a device plugged straight
  // in counts as 1. Behind a second hub (a hub made of two chips, or one plugged into a hub) jack
  // numbers repeat, so those prefer the highest free slot and don't take the number a jack on the
  // first hub will want. Any of them falls back to a free slot, so four devices always work.
  uint8_t tier = 0;
  if (info.hub_addr) {
    tuh_bus_info_t hubInfo;
    tier = (tuh_bus_info_get(info.hub_addr, &hubInfo) && !hubInfo.hub_addr) ? 1 : 2;
  }
  const uint8_t jack = info.hub_port ? info.hub_port : 1;
  uint8_t port = 0;
  if (tier <= 1 && jack <= HUB_PORTS && !hubPorts[jack - 1].mounted) port = jack;
  if (!port && tier == 2) for (uint8_t p = HUB_PORTS; p >= 1 && !port; --p) if (!hubPorts[p - 1].mounted) port = p;
  if (!port) for (uint8_t p = 1; p <= HUB_PORTS && !port; ++p) if (!hubPorts[p - 1].mounted) port = p;
  if (!port) return false;

  HubPort &hp = hubPorts[port - 1];
  resetHubPort(hp);
  hp.mounted = true;
  hp.daddr = daddr;
  hp.writable = writable;
  hp.midiIdx = idx;
  midiIdxToPort[idx] = port;
  portMapDirty = true;
  BlackBoxPort &bb = blackBox.port[port - 1];
  bb = BlackBoxPort{};
  tuh_vid_pid_get(daddr, &bb.vid, &bb.pid);
  bb.mounted = true;
  bb.writable = hp.writable;
  bb.hubPort = info.hub_port;
  bb.tier = tier;
  logEvent(HOST_EVENT_MOUNT, port, (uint8_t)tuh_speed_get(daddr), cables);
  return true;
}

void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t *mount_cb_data) {
  if (idx >= CFG_TUH_MIDI) return;
  const bool writable = mount_cb_data->tx_cable_count > 0;
  const uint8_t cables = (uint8_t)((mount_cb_data->rx_cable_count << 4) | (mount_cb_data->tx_cable_count & 0x0F)); // in cables high, out cables low
  if (placeMidi(idx, mount_cb_data->daddr, writable, cables)) return;
  waitingMidi[idx] = WaitingMidi{true, mount_cb_data->daddr, writable, cables};
  portMapDirty = true;
  logEvent(HOST_EVENT_FULL, 0, mount_cb_data->daddr);
}

// A slot freed up: the first device that was waiting takes it.
void placeWaitingMidi() {
  for (uint8_t i = 0; i < CFG_TUH_MIDI; ++i) {
    if (!waitingMidi[i].used) continue;
    if (!placeMidi(i, waitingMidi[i].daddr, waitingMidi[i].writable, waitingMidi[i].cables)) return; // still full
    waitingMidi[i].used = false;
    portMapDirty = true;
  }
}

// The hubs attached, by address, and how many jacks they add up to. A hub chained to another uses
// one jack of it to connect.
bool hubMounted[CFG_TUH_HUB];

uint8_t hubJacks() {
  uint16_t ports = 0;
  uint8_t hubs = 0;
  for (uint8_t h = 0; h < CFG_TUH_HUB; ++h) {
    if (!hubMounted[h]) continue;
    ++hubs;
    ports += foxwizard_hub_port_count(CFG_TUH_DEVICE_MAX + 1 + h);
  }
  if (!hubs) return 0;
  const uint16_t jacks = ports > hubs - 1 ? ports - (hubs - 1) : 0;
  return jacks > 120 ? 120 : jacks;
}

uint8_t hubCount() {
  uint8_t n = 0;
  for (bool m : hubMounted) n += m;
  return n;
}

// Any device at all, so the diagnostic light can tell "nothing plugged in" from "not USB MIDI".
void tuh_mount_cb(uint8_t daddr) {
  // The stack gives hubs the addresses above CFG_TUH_DEVICE_MAX, so that's what makes one a hub.
  tuh_bus_info_t info;
  const bool known = tuh_bus_info_get(daddr, &info);
  logEvent(HOST_EVENT_DEVICE, known ? info.hub_port : 0, daddr, (uint8_t)tuh_speed_get(daddr));
  if (daddr > CFG_TUH_DEVICE_MAX) {
    if (daddr <= CFG_TUH_DEVICE_MAX + CFG_TUH_HUB) hubMounted[daddr - 1 - CFG_TUH_DEVICE_MAX] = true;
    portMapDirty = true;
    return;
  }
  ++usbDevicesMounted;
  blackBox.devices = usbDevicesMounted;
  noteStatusActivity(STATUS_WHITE);
}

void tuh_umount_cb(uint8_t daddr) {
  if (daddr > CFG_TUH_DEVICE_MAX) { // a hub
    if (daddr <= CFG_TUH_DEVICE_MAX + CFG_TUH_HUB) hubMounted[daddr - 1 - CFG_TUH_DEVICE_MAX] = false;
    portMapDirty = true;
    return;
  }
  if (usbDevicesMounted > 0) --usbDevicesMounted;
  blackBox.devices = usbDevicesMounted;
}

void tuh_midi_umount_cb(uint8_t idx) {
  if (idx >= CFG_TUH_MIDI) return;
  waitingMidi[idx].used = false;
  uint8_t port = midiIdxToPort[idx];
  midiIdxToPort[idx] = 0;
  if (port == 0) { portMapDirty = true; return; }
  resetHubPort(hubPorts[port - 1]);
  portMapDirty = true;
  blackBox.port[port - 1].mounted = false; // the rest stays, for a report
  logEvent(HOST_EVENT_UNMOUNT, port);
  placeWaitingMidi();
}

// Reads a device only while the link queue has room for a whole USB packet's worth. Otherwise the
// data stays in TinyUSB and the device itself is held off, so nothing is lost on this side.
void pumpUsbIn() {
  for (uint8_t p = 0; p < HUB_PORTS; ++p) {
    HubPort &hp = hubPorts[p];
    if (!hp.mounted) continue;
    uint8_t buf[USB_READ_PACKETS * 4];
    while (toMainNormal.freeSlots(false) >= USB_READ_PACKETS && toMainRt.freeSlots(true) >= USB_READ_PACKETS) {
      uint32_t n = tuh_midi_packet_read_n(hp.midiIdx, buf, sizeof(buf));
      if (n == 0) break;
      for (uint32_t i = 0; i + 3 < n; i += 4) {
        if ((buf[i] & 0x0F) < 2) continue; // CIN 0/1 reserved, also skips zero padding
        ++blackBox.port[p].received;
        sendToMain(hostLinkPort(p + 1), &buf[i]);
      }
      hp.blinkUntilMs = millis() + LED_BLINK_MS;
      noteStatusActivity(STATUS_BLUE);
    }
  }
}

void beginStatusRgb() {
  uint offset;
  if (!pio_claim_free_sm_and_add_program(&WS2812_PROGRAM, &statusRgbPio, &statusRgbSm, &offset)) {
    statusRgbPio = nullptr; // no free PIO, run without the diagnostic light
    return;
  }
  pio_gpio_init(statusRgbPio, PIN_STATUS_RGB);
  pio_sm_set_consecutive_pindirs(statusRgbPio, statusRgbSm, PIN_STATUS_RGB, 1, true);
  pio_sm_config c = pio_get_default_sm_config();
  sm_config_set_wrap(&c, offset, offset + 3);
  sm_config_set_sideset(&c, 1, false, false);
  sm_config_set_sideset_pins(&c, PIN_STATUS_RGB);
  sm_config_set_out_shift(&c, false, true, 24);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
  sm_config_set_clkdiv(&c, clock_get_hz(clk_sys) / (800000.0f * 10));
  pio_sm_init(statusRgbPio, statusRgbSm, offset, &c);
  pio_sm_set_enabled(statusRgbPio, statusRgbSm, true);
}

// color is 0xRRGGBB. The LED takes green, red, blue.
void writeStatusRgb(uint32_t color) {
  if (!statusRgbPio || pio_sm_is_tx_fifo_full(statusRgbPio, statusRgbSm)) return;
  uint32_t grb = (((color >> 8) & 0xFF) << 16) | (((color >> 16) & 0xFF) << 8) | (color & 0xFF);
  pio_sm_put(statusRgbPio, statusRgbSm, grb << 8);
}

// The brightness knob's ceiling for this LED, same formula as the main brain's scaledMax().
uint8_t statusMaxLevel() {
  if (ledBrightnessStep == 0) return 0;
  uint16_t pct = (uint16_t)ledBrightnessStep * 10;
  uint16_t v = ((uint16_t)STATUS_RGB_LEVEL * pct + 50) / 100;
  return (uint8_t)(v > STATUS_RGB_LEVEL ? STATUS_RGB_LEVEL : v);
}

// level is already brightness-scaled, 0-STATUS_RGB_LEVEL. Blue leans slightly toward cyan.
uint32_t statusColorAt(uint8_t color, uint8_t level) {
  if (color == STATUS_WHITE) return rgbColor(level, level, level);
  if (color == STATUS_PURPLE) return rgbColor(level, 0, level);
  return rgbColor(0, level / 3, level);
}

uint8_t statusScaleByte(uint32_t elapsed, uint32_t duration) {
  if (elapsed >= duration) return 255;
  return (uint8_t)((elapsed * 255UL) / duration);
}

void writeStatusLevel(uint8_t level) {
  statusLed.currentLevel = level;
  const uint32_t color = statusColorAt(statusLed.color, level);
  if (color != lastStatusRgb) {
    writeStatusRgb(color);
    lastStatusRgb = color;
  }
}

// Any MIDI activity (blue) or a newly connected USB device (white). Mirrors noteActivity() on the
// main brain: the trigger is just a flag, the engine below picks it up and restarts the fade-in if
// it wasn't already rising or on. A color switch while already lit takes effect immediately.
void noteStatusActivity(uint8_t color) {
  statusLed.activitySeen = true;
  statusLed.pendingColor = color;
}

void serviceStatusLed() {
  const uint32_t now = millis();

  if (lastMainFrameMs == 0 || now - lastMainFrameMs > STATUS_LINK_SILENT_MS) {
    statusLed.color = STATUS_PURPLE;
    writeStatusLevel(((now / STATUS_LINK_BLINK_MS) % 2) ? statusMaxLevel() : 0);
    statusLed.phase = STATUS_OFF; // a clean slate for whenever the link comes back
    statusLed.currentLevel = 0;
    return;
  }

  const uint8_t maxLevel = statusMaxLevel();

  if (statusLed.activitySeen) {
    statusLed.activitySeen = false;
    statusLed.lastActivityMs = now;
    statusLed.color = statusLed.pendingColor;
    if (statusLed.phase != STATUS_RISING && statusLed.phase != STATUS_ON) {
      statusLed.phase = STATUS_RISING;
      statusLed.phaseStartMs = now;
      statusLed.fadeStartLevel = statusLed.currentLevel;
    }
  }

  switch (statusLed.phase) {
    case STATUS_OFF:
      writeStatusLevel(0);
      break;

    case STATUS_RISING: {
      const uint32_t elapsed = now - statusLed.phaseStartMs;
      const uint8_t progress = statusScaleByte(elapsed, STATUS_FADE_IN_MS);
      const uint8_t level = statusLed.fadeStartLevel + (((maxLevel - statusLed.fadeStartLevel) * progress) / 255);
      writeStatusLevel(level);
      if (elapsed >= STATUS_FADE_IN_MS) {
        statusLed.phase = STATUS_ON;
        writeStatusLevel(maxLevel);
      }
      break;
    }

    case STATUS_ON:
      writeStatusLevel(maxLevel);
      if (now - statusLed.lastActivityMs > STATUS_HOLD_MS) {
        statusLed.phase = STATUS_FALLING;
        statusLed.phaseStartMs = now;
        statusLed.fadeStartLevel = statusLed.currentLevel;
      }
      break;

    case STATUS_FALLING: {
      if (now - statusLed.lastActivityMs <= STATUS_HOLD_MS) {
        statusLed.phase = STATUS_RISING;
        statusLed.phaseStartMs = now;
        statusLed.fadeStartLevel = statusLed.currentLevel;
        break;
      }
      const uint32_t elapsed = now - statusLed.phaseStartMs;
      if (elapsed >= STATUS_FADE_OUT_MS) {
        statusLed.phase = STATUS_OFF;
        writeStatusLevel(0);
      } else {
        const uint8_t progress = statusScaleByte(elapsed, STATUS_FADE_OUT_MS);
        const uint8_t level = statusLed.fadeStartLevel - ((statusLed.fadeStartLevel * progress) / 255);
        writeStatusLevel(level);
      }
      break;
    }
  }
}

// Purple, instant, two flashes: only shown right after a restart a USB fault caused. A fault
// reboots at once (see foxwizard_usb_fault below), so there's no live moment to show it in,
// only this, at the next boot. Otherwise the light stays dark, same as every other idle state.
void showBootStatus() {
  if (restartReason == RESTART_NONE) return;
  const uint32_t purple = rgbColor(STATUS_RGB_LEVEL, 0, STATUS_RGB_LEVEL);
  for (uint8_t i = 0; i < 2; ++i) {
    writeStatusRgb(purple);
    delay(STATUS_FAULT_FLASH_MS);
    writeStatusRgb(0);
    delay(STATUS_FAULT_FLASH_MS);
  }
  lastStatusRgb = 0;
}

void serviceLeds() {
  const uint32_t now = millis();
  for (uint8_t p = 0; p < LED_PORTS; ++p) {
    const HubPort &hp = hubPorts[p];
    bool blinking = (int32_t)(hp.blinkUntilMs - now) > 0;
    digitalWrite(PIN_PORT_LED[p], (hp.mounted && !blinking) ? HIGH : LOW);
  }
}

// ============================================================================================
// Setup / loop
// ============================================================================================
void setup() {
  // The pull-up needs a moment to bring a floating pin up, so a read straight after pinMode can see low
  // and mistake a bare module for one with a TRS board. Eight reads over 8 ms must all say low.
  pinMode(PIN_TRS_BOARD, INPUT_PULLUP);
  delay(5);
  trsBoard = true;
  for (uint8_t i = 0; i < 8; ++i) {
    if (digitalRead(PIN_TRS_BOARD) != LOW) trsBoard = false;
    delay(1);
  }

  for (uint8_t p = 0; p < LED_PORTS; ++p) {
    pinMode(PIN_PORT_LED[p], OUTPUT);
    digitalWrite(PIN_PORT_LED[p], LOW);
  }
  memset(midiIdxToPort, 0, sizeof(midiIdxToPort));
  readRestartReason();

  beginStatusRgb();
  showBootStatus();

  dinOuts[0].serial = &Serial2;
  dinOuts[1].serial = &dinOut2Serial;
  dinOuts[2].serial = &dinOut3Serial;
  dinOuts[3].serial = &dinOut4Serial;
  dinOuts[4].serial = &dinOut5Serial;

  Serial1.setTX(PIN_LINK_TX);
  Serial1.setRX(PIN_LINK_RX);
  Serial1.setFIFOSize(4096);
  Serial1.begin(LINK_BAUD);

  Serial2.setTX(PIN_DIN_OUT1);
  Serial2.setRX(PIN_DIN_IN);
  Serial2.begin(DIN_BAUD);

  dinOut2Serial.begin(DIN_BAUD);
  dinOut3Serial.begin(DIN_BAUD);
  dinOut4Serial.begin(DIN_BAUD);
  dinOut5Serial.begin(DIN_BAUD);

  USBHost.begin(0);
  rp2040.wdt_begin(WATCHDOG_MS);
}

void loop() {
  static uint32_t loopStartUs = 0;
  const uint32_t nowUs = micros();
  if (loopStartUs && nowUs - loopStartUs > blackBox.maxLoopUs) blackBox.maxLoopUs = nowUs - loopStartUs;
  loopStartUs = nowUs;
  blackBox.uptimeMs = millis();
  rp2040.wdt_reset();
  blackBox.stage = HOST_STAGE_USB_TASK;
  USBHost.task();

  blackBox.stage = HOST_STAGE_LINK_IN;
  pumpLinkIn();
  blackBox.stage = HOST_STAGE_DIN_IN;
  pumpDinIn();
  blackBox.stage = HOST_STAGE_USB_IN;
  pumpUsbIn();

  blackBox.stage = HOST_STAGE_DISPATCH;
  dispatchFromMain();
  blackBox.stage = HOST_STAGE_USB_OUT;
  pumpUsbOuts();
  blackBox.stage = HOST_STAGE_DIN_OUT;
  pumpDinOuts();

  blackBox.stage = HOST_STAGE_CONTROL;
  serviceControlOut();
  blackBox.stage = HOST_STAGE_LINK_OUT;
  pumpLinkOut();

  blackBox.stage = HOST_STAGE_LEDS;
  serviceLeds();
  serviceStatusLed();
}
