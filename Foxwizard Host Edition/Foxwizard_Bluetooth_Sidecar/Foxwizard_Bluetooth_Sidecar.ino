/*
  ARDUINO TOOLS SETTINGS, set these right before uploading. The build stops with the one to change if it can see a wrong one.
  Libraries: NimBLE-Arduino 2.x. Board package: esp32 by Espressif, 3.x.

  ESP32-S3 SuperMini or Waveshare ESP32-S3-Zero (both 4 MB flash, 2 MB quad PSRAM):
    Board                  ESP32S3 Dev Module
    USB Mode               USB-OTG (TinyUSB)
    USB CDC On Boot        Disabled
    Flash Size             4MB (32Mb)
    PSRAM                  Disabled (QSPI PSRAM also works, OPI PSRAM does not)
    Partition Scheme       Default 4MB with spiffs
    CPU Frequency          240MHz (WiFi)
    Core Debug Level       None
  Deluxe, ESP32-S3-WROOM-1-N16R8 (set FW_S3_MODULE to FW_S3_DELUXE in board_config.h), the others as above:
    Flash Size             16MB (128Mb)
    PSRAM                  OPI PSRAM (Disabled also starts, it just leaves the 8 MB unused)
    Partition Scheme       16M Flash (3MB APP/9.9MB FATFS)
  Flash Size is the one the build cannot see. The module reads it from the chip at start and blinks its light red if it is wrong.

  Putting a module in upload mode: hold its BOOT button, plug in USB (or press RESET while holding BOOT), let go, upload. The USB port is a
  USB MIDI port while the module runs, so it has no serial port for the IDE to reset, and this is always how it is flashed by hand.

  Foxwizard_Bluetooth_Sidecar.ino by woz.lol
  FoxWizard Bluetooth module: an ESP32-S3 SuperMini or Waveshare Zero that gives the FoxWizard Bluetooth MIDI, in both roles, over
  one serial link to the main brain. It does no routing. Everything it hears goes to the main brain tagged with the port it came
  from, and everything the main brain sends comes back tagged with the port it goes to. Frame format and the slot rules are in
  ../README.md.

  - BLE Device (link port 9): the FoxWizard shows up as a Bluetooth MIDI device to a phone or computer. Always advertising.
  - BLE Host (link ports 1 to MODULE_HANDLES, shared with USB): the FoxWizard connects to Bluetooth MIDI devices such as keyboards. The main brain
    says which devices to reconnect to and how many it has room for. The module scans for them, and for new ones while learning.

  Wiring (see board_config.h for the module switch):
  - GPIO43 = link TX to main brain GP14
  - GPIO44 = link RX from main brain GP15
  - GND shared

  The button is the module's BOOT button (GPIO0), pressed while it runs. At power on the remembered devices reconnect on their own.
  - style FW_BUTTON_ONE_MODE (the default), one add mode for both roles, nothing to choose:
    - Tap (let go within 1 second): add devices for a minute. Nothing that is connected is dropped. A phone or computer can connect to
      BLE Device (it advertises while none is connected), and the module scans for Bluetooth MIDI devices and connects the ones it finds,
      up to the room there is, remembering each at once. Tap again: stop adding early.
    - Hold until the white light: the Bluetooth radio goes off if it was on and on if it was off. It switches, and is saved, as the white light
      begins, at 2.5 seconds if it was on and 1.5 seconds if it was off. The white fade finishes and the module restarts even if the button is let go. Let go before the white and
      nothing changes.
    - Hold 10 seconds: every Bluetooth device and bond is forgotten at 10 seconds, no need to let go. Let go before and nothing is forgotten.
      The radio is left on, whatever it was, because the hold passed the 3 second step on the way.
  - style FW_BUTTON_CME, CME's way, in case it is ever needed:
    - Hold 3 seconds, let go: force peripheral. Drops every Bluetooth link and advertises for a minute so a phone or computer
      can connect. Then the remembered devices reconnect on their own.
    - Double press: learn hosts for a minute, the same adding. Short press: stop learning.
    - Hold 10 seconds, let go: forget every Bluetooth device and bond. The light goes white at 3 seconds held and red at 10.

  Status light, the module's own WS2812, off by default, same no-blink ethic as the other lights:
  - blue: fades up on Bluetooth MIDI and fades back down, 1 s up and 1/2 s down, following the main brain's brightness knob
  - green: a short flash for each new Bluetooth connection
  - blue, blinking: adding devices
  - held button, default style. Radio on: 3 red flashes from 1 s, then at 2.5 s the radio goes off with a white fade-out. Radio off: 3 green
    flashes from the press, then at 1.5 s the radio goes on with a white fade-in to solid white. Both: 1.5 s after that a fast red blink with every 4th brighter, the
    one at 10 s is the forget and fades out red.
  - purple, pulsing slowly: force peripheral (CME style only), waiting for a phone or computer
  - purple, blinking fast: nothing heard from the main brain for over a second
  - red, blinking slowly: the Tools settings are wrong, see the serial monitor
*/

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <Update.h>

#include "board_config.h"
#include "board_check.h"
#include "foxwizard_link.h"
#include "ble_midi.h"
#include "usb_midi_host.h"
#include "USB.h"
#include "USBMIDI.h"
#if FW_BENCH
#include "USBCDC.h"
#endif

static_assert(MODULE_HANDLES >= 1 && MODULE_HANDLES <= BLE_HANDLES, "MODULE_HANDLES is 1 to 8");
static_assert(BLE_HOSTS_MAX >= 1 && BLE_HOSTS_MAX <= MODULE_HANDLES, "BLE_HOSTS_MAX is 1 to MODULE_HANDLES");
static_assert(USB_HOSTS_MAX >= 1 && USB_HOSTS_MAX <= MODULE_HANDLES, "USB_HOSTS_MAX is 1 to MODULE_HANDLES");
static_assert(BLE_HOSTS_MAX + 1 <= NIMBLE_MAX_CONNECTIONS, "Raise CONFIG_BT_NIMBLE_MAX_CONNECTIONS in NimBLE-Arduino's nimconfig.h to BLE_HOSTS_MAX + 1, or lower BLE_HOSTS_MAX in board_config.h");

// ============================================================================================
// Constants
// ============================================================================================
constexpr uint8_t BT_FW_VERSION = 13;
constexpr uint32_t LEARN_MS = 60000;
constexpr uint32_t PERIPHERAL_MS = 60000;
constexpr uint32_t HEARTBEAT_MS = 500;
constexpr uint32_t IDENTITY_MS = 250;
constexpr uint32_t KNOWN_SILENCE_MS = 5000; // the main brain repeats its list this often
constexpr uint32_t HOLD_FORGET_MS = 10000;
constexpr uint32_t HOLD_MODE_MS = 3000;      // the Bluetooth radio goes on or off (CME style: force peripheral)
constexpr uint32_t HOLD_TAP_MS = 1000;       // a press shorter than this is a tap
constexpr uint32_t COUNTDOWN_GAP_MS = 1500;  // after the radio step, 1 s of fade and rest, then the red blink starts counting to the forget at HOLD_FORGET_MS
constexpr uint32_t CUE_FLASH_MS = 500;       // one flash of the radio step, lit for the first half
constexpr uint32_t CUE_FADE_MS = 500;
constexpr uint32_t HOLD_SWITCH_MS = HOLD_MODE_MS - CUE_FADE_MS; // the radio switches here, as the white begins, when it was on (1 s of tap time, then 3 red flashes)
constexpr uint32_t HOLD_SWITCH_OFF_MS = 3 * CUE_FLASH_MS;         // and here when it was off (3 green flashes from the press, no tap time)
constexpr uint32_t COUNTDOWN_BLINK_MS = 250;
constexpr uint32_t DOUBLE_PRESS_MS = 500;
constexpr uint32_t LINK_SILENT_MS = 1000;
constexpr uint8_t STATUS_RGB_LEVEL = 40;     // light brightness ceiling, 0 to 255, before the brightness knob
constexpr uint8_t STATUS_FLOOR = 6;          // button feedback and errors stay visible with the knob at 0
constexpr uint32_t FADE_IN_MS = 1000;
constexpr uint32_t FADE_OUT_MS = 500;
constexpr uint32_t CONNECT_FLASH_MS = 350;
constexpr uint16_t LINK_RX_BUDGET = 1024;
constexpr uint16_t LINK_RX_PAUSE_AT = 2048;
constexpr uint16_t LINK_RX_RESUME_AT = 1024;
constexpr uint32_t REFUSED_MS = 10000;       // a device the main brain sent away is left alone this long

static const NimBLEUUID MIDI_SERVICE("03B80E5A-EDE8-4B33-A751-6CE34EC4C700");
static const NimBLEUUID MIDI_CHARACTERISTIC("7772E5DB-3868-4112-A1A9-F2669D106BF3");

// ============================================================================================
// Types
// ============================================================================================
// Bluetooth MIDI that arrived, waiting for the main loop. Filled by the Bluetooth task, emptied by loop().
struct RxRing {
  static constexpr uint8_t SLOTS = 16;
  static constexpr uint16_t MAX = 244;
  struct Item {
    uint16_t len;
    uint8_t data[MAX];
  };
  Item items[SLOTS];
  volatile uint8_t head = 0;
  volatile uint8_t tail = 0;
  volatile uint32_t dropped = 0;

  void push(const uint8_t *d, size_t n) {
    const uint8_t next = (uint8_t)((head + 1) % SLOTS);
    if (next == tail) { ++dropped; return; }
    if (n > MAX) n = MAX;
    memcpy(items[head].data, d, n);
    items[head].len = (uint16_t)n;
    __sync_synchronize();
    head = next;
  }
  bool full() const { return (uint8_t)((head + 1) % SLOTS) == tail; }
  Item *peek() { return tail == head ? nullptr : &items[tail]; }
  void release() { __sync_synchronize(); tail = (uint8_t)((tail + 1) % SLOTS); }
  void clear() { tail = head; }
};

// Index 0 is BLE Device, 1 to MODULE_HANDLES are the hosts (Bluetooth or USB), PEER_USB_DEVICE is the module's own USB MIDI device port.
// A host's link port is its index. BLE Device and the USB device port both use LINK_PORT_BLE_DEVICE, so a computer plugged into the
// module and a phone on Bluetooth hear and play the same port.
constexpr uint8_t PEER_USB_DEVICE = 1 + MODULE_HANDLES;
constexpr uint8_t PEER_COUNT = 2 + MODULE_HANDLES;
struct Peer {
  volatile bool connected = false;
  bool announced = false;                 // loop() has seen it connect
  uint16_t connHandle = 0xFFFF;           // BLE Device: which central
  uint16_t mtu = 23;
  bool usb = false;                       // its data is USB MIDI packets, 4 bytes each, not Bluetooth MIDI: a USB device on the host port, or the USB device port
  bool usbDevice = false;                 // PEER_USB_DEVICE only: a computer has the module's USB device port mounted
  uint8_t usbIndex = 0;                   // its place in usbmidi
  NimBLEClient *client = nullptr;         // BLE Host
  NimBLERemoteCharacteristic *chr = nullptr;
  uint8_t addr[6] = {0, 0, 0, 0, 0, 0};
  char name[NAME_CHARS + 1] = {0};
  fw::blemidi::Parser parser;
  fw::ump::UsbMidiToUmp up;
  fw::ump::UmpToUsbMidi down;
  MidiQueue<MidiPacket, 32, 0> outRt;
  MidiQueue<MidiPacket, 256, 32> outNormal;
  RxRing rx;
  uint8_t identityStep = 0;
};

enum LedColor : uint8_t { LED_NONE = 0, LED_BLUE, LED_GREEN, LED_PURPLE, LED_RED, LED_WHITE };

// ============================================================================================
// Globals
// ============================================================================================
Peer peers[PEER_COUNT];
uint8_t usbHandleOf[usbmidi::kMaxDevices]; // the handle each USB MIDI device has, 0 none
Preferences prefs;
bool usbHostActive = false;                // the module's USB port is a host port this run
bool usbDeviceActive = false;              // the module's USB port is a USB MIDI device this run
bool radioOn = true;                       // the Bluetooth radio is on this run, a saved setting
USBMIDI *usbMidi = nullptr;
uint32_t usbHostRetryMs = 0;               // the host did not start, try again at this time
uint32_t restartAtMs = 0;
NimBLEServer *bleServer = nullptr;
NimBLECharacteristic *deviceChr = nullptr;
TaskHandle_t connectTask = nullptr;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;

// Where the module prints about itself. Only bench mode has a place for it, the USB port is a serial port there. Otherwise it goes
// nowhere: the USB port is MIDI only, and Serial would be UART0, which is on the link's pins.
class NullPrint : public Print {
 public:
  size_t write(uint8_t) override { return 1; }
  size_t write(const uint8_t *, size_t n) override { return n; }
};
#if FW_BENCH
USBCDC benchSerial;
Print &dbg = benchSerial;
#else
NullPrint nullPrint;
Print &dbg = nullPrint;
#endif

// The link to the main brain
HardwareSerial &linkSerial = Serial1;
MidiQueue<LinkFrame, 64, 0> toMainRt;
MidiQueue<LinkFrame, 512, 32> toMainNormal;
LinkFrame linkTxFrame;
uint8_t linkTxPos = LINK_FRAME_BYTES;
LinkFrameReader linkReader;
LinkMessageReader linkMessage;
bool mainAskedPause = false;
uint32_t mainFlowMs = 0;
bool pausingMain = false;
uint32_t lastFlowSentMs = 0;
uint32_t lastMainFrameMs = 0;
uint8_t ledBrightnessStep = 10;

// What the main brain has told us
uint8_t allowedHosts = FW_BENCH ? MODULE_HANDLES : 0;
uint8_t knownAddr[BLE_HANDLES + 1][6];
bool knownValid[BLE_HANDLES + 1];
uint32_t lastKnownMs = 0;

// What is going on here
uint32_t learnUntilMs = 0;
uint32_t peripheralUntilMs = 0;
volatile bool connecting = false;
NimBLEAddress pendingAddr;
std::string pendingName;
uint8_t refusedAddr[6] = {0, 0, 0, 0, 0, 0};
uint32_t refusedMs = 0;
uint32_t lastConnectEndMs = 0;
uint32_t lastScanMs = 0;
uint32_t lastAdvMs = 0;
volatile bool statusDirty = true;
uint32_t lastBeatMs = 0;
uint32_t lastIdentityMs = 0;
uint8_t identityPeer = 1;
uint8_t clearEventsLeft = 0;
uint8_t boardCheckResult = BOARD_CHECK_OK;
bool bannerPrinted = false;

// Button and light
uint32_t buttonHeldMs = 0;
bool buttonDown = false;          // the button is held, buttonHeldMs says for how long
bool holdStartedOn = true;        // the Bluetooth radio was on when this hold began
bool holdRadioDone = false;       // this hold has reached the radio step
bool holdClearDone = false;       // and the 10 s step
bool restartAfterRelease = false; // the radio step changed the radio setting, the restart waits until the button is up
uint32_t clearFadeStartMs = 0;    // the red fade-out that says everything was forgotten
uint32_t radioCueStartMs = 0;     // the white fade that says the radio switched, it finishes even if the button is let go
bool radioCueToOn = false;        // that fade is the radio coming on (fading in) and not going off (fading out)
uint32_t flashUntilMs = 0;
uint32_t activityMs = 0;
float ledLevel = 0;
uint32_t ledFadeMs = 0;
uint32_t lastLedWrite = 0xFFFFFFFF;

// ============================================================================================
// Small helpers
// ============================================================================================
bool timerActive(uint32_t until) { return until != 0 && (int32_t)(until - millis()) > 0; }
bool learning() { return timerActive(learnUntilMs); }
bool peripheralWindow() { return timerActive(peripheralUntilMs); }
bool addrEqual(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 6) == 0; }
bool addrZero(const uint8_t *a) { static const uint8_t z[6] = {0, 0, 0, 0, 0, 0}; return memcmp(a, z, 6) == 0; }
uint8_t peerPort(uint8_t i) { return i == 0 || i == PEER_USB_DEVICE ? LINK_PORT_BLE_DEVICE : i; }
bool isHostPeer(uint8_t i) { return i >= 1 && i <= MODULE_HANDLES; }
// The peers that answer to a link port: one, or for BLE Device two (Bluetooth and the USB device port). Returns how many.
uint8_t peersForPort(uint8_t port, Peer *out[2]) {
  if (port == LINK_PORT_BLE_DEVICE) { out[0] = &peers[0]; out[1] = &peers[PEER_USB_DEVICE]; return 2; }
  if (port >= 1 && port <= MODULE_HANDLES) { out[0] = &peers[port]; return 1; }
  return 0;
}
bool deviceConnected() { return peers[0].connected || peers[PEER_USB_DEVICE].connected; }

// Every handle in use, Bluetooth and USB.
uint8_t hostsConnected() {
  uint8_t n = 0;
  for (uint8_t h = 1; h <= MODULE_HANDLES; ++h) if (peers[h].connected) ++n;
  return n;
}

uint8_t bleHostsConnected() {
  uint8_t n = 0;
  for (uint8_t h = 1; h <= MODULE_HANDLES; ++h) if (peers[h].connected && !peers[h].usb) ++n;
  return n;
}

uint8_t freeHostHandle() {
  for (uint8_t h = 1; h <= MODULE_HANDLES; ++h) if (!peers[h].connected) return h;
  return 0;
}

uint8_t hostByAddress(const uint8_t *a) {
  for (uint8_t h = 1; h <= MODULE_HANDLES; ++h) if (peers[h].connected && addrEqual(peers[h].addr, a)) return h;
  return 0;
}

bool isKnown(const uint8_t *a) {
  for (uint8_t s = 1; s <= BLE_HANDLES; ++s) if (knownValid[s] && addrEqual(knownAddr[s], a)) return true;
  return false;
}

// A remembered device that is not connected right now, so there is something to scan for.
bool knownMissing() {
  for (uint8_t s = 1; s <= BLE_HANDLES; ++s) if (knownValid[s] && !hostByAddress(knownAddr[s])) return true;
  return false;
}

void cleanName(char *name) { nameClean(name); }

uint32_t totalDrops() {
  uint32_t n = toMainRt.dropped() + toMainNormal.dropped();
  for (Peer &p : peers) n += p.outRt.dropped() + p.outNormal.dropped() + p.rx.dropped;
  return n;
}

// ============================================================================================
// Link out to the main brain. Realtime and control frames always go first.
// ============================================================================================
void sendToMain(uint8_t port, const uint8_t pkt[4]) {
  LinkFrame f;
  encodeLinkFrame(port, pkt, f);
  toMainRt.push(f, true);
}

void sendPacketToMain(uint8_t port, fw::ump::UsbMidiToUmp &up, const uint8_t pkt[4]) {
  uint32_t words[4];
  const uint8_t n = up.push(pkt, words);
  for (uint8_t i = 0; i < n;) {
    const uint8_t len = fw::ump::wordCount(words[i]);
    if (isRealtimeWord(words[i])) pushLinkMessage(toMainRt, port, &words[i], len, true);
    else pushLinkMessage(toMainNormal, port, &words[i], len, isCriticalWord(words[i]));
    i += len;
  }
}

bool mainPaused() { return mainAskedPause && millis() - mainFlowMs < FLOW_EXPIRY_MS; }

void pumpLinkOut() {
  while (true) {
    if (linkTxPos >= LINK_FRAME_BYTES) {
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
    while (linkTxPos < LINK_FRAME_BYTES && linkSerial.availableForWrite() > 0) linkSerial.write(linkTxFrame.b[linkTxPos++]);
    if (linkTxPos < LINK_FRAME_BYTES) return;
  }
}

// ============================================================================================
// Light
// ============================================================================================
void noteActivity() { activityMs = millis(); }

void writeLed(uint8_t r, uint8_t g, uint8_t b) {
  const uint32_t packed = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  if (packed == lastLedWrite) return;
  lastLedWrite = packed;
  neopixelWrite(PIN_STATUS_RGB, r, g, b);
}

void showColor(uint8_t color, uint8_t level) {
  uint8_t r = 0, g = 0, b = 0;
  switch (color) {
    case LED_BLUE: b = level; break;
    case LED_GREEN: g = level; break;
    case LED_PURPLE: r = level; b = level; break;
    case LED_RED: r = level; break;
    case LED_WHITE: r = g = b = level; break;
    default: break;
  }
  writeLed(r, g, b);
}

// When the held button switches the radio: 2.5 s if it was on, 1.5 s if it was off. The red blink toward the forget starts 1.5 s after that.
uint32_t holdSwitchMs() { return holdStartedOn ? HOLD_SWITCH_MS : HOLD_SWITCH_OFF_MS; }

// The light for a held button in the default style, and the red fade-out after the 10 s step. True when it has set the light.
//   Radio on:  under 1 s nothing (it may be a tap, and the blue blink answers a tap), 1 to 2.5 s three red flashes, then the radio goes off
//              and a white fade-out shows it.
//   Radio off: 0 to 1.5 s three green flashes, starting at once because nothing else answers the press, then the radio goes on and a white
//              fade-in shows it, solid white until 3 s.
//   The switch happens as the white begins (2.5 s), and the fade finishes even if the button is let go.
//   Both: from 1.5 s after the switch (4 s on, 3 s off) to 10 s a red blink every 250 ms with every 4th brighter, the last bright one at 10 s is the forget and fades out over 0.5 s.
bool buttonCue(uint32_t now, uint8_t lvl) {
  if (clearFadeStartMs) {
    const uint32_t f = now - clearFadeStartMs;
    if (f < CUE_FADE_MS) { showColor(LED_RED, (uint8_t)((uint32_t)lvl * (CUE_FADE_MS - f) / CUE_FADE_MS)); return true; }
    clearFadeStartMs = 0;
  }
  if (radioCueStartMs) {
    const uint32_t f = now - radioCueStartMs;
    if (f < CUE_FADE_MS) {
      showColor(LED_WHITE, (uint8_t)((uint32_t)lvl * (radioCueToOn ? f : CUE_FADE_MS - f) / CUE_FADE_MS));
      return true;
    }
    if (!buttonDown) {
      if (restartAfterRelease) { // let go after the fade, the restart is about to happen: show where it ended
        if (radioCueToOn && !holdClearDone) showColor(LED_WHITE, lvl);
        else writeLed(0, 0, 0);
        return true;
      }
      radioCueStartMs = 0;
    }
  }
  if (!buttonDown) return false;
  const uint32_t t = buttonHeldMs;
  if (t >= HOLD_FORGET_MS) { writeLed(0, 0, 0); return true; }
  const uint32_t sw = holdSwitchMs();
  if (t >= sw + COUNTDOWN_GAP_MS) {
    const uint32_t n = t - sw - COUNTDOWN_GAP_MS;
    const uint32_t m = n / COUNTDOWN_BLINK_MS;
    const bool lit = m >= 1 && n % COUNTDOWN_BLINK_MS < COUNTDOWN_BLINK_MS / 2;
    const uint8_t dim = (uint8_t)(lvl / 3 < 2 ? 2 : lvl / 3);
    showColor(LED_RED, lit ? (m % 4 == 0 ? lvl : dim) : 0);
    return true;
  }
  if (t >= sw) { // after the white fade, still held: on is solid white, off is dark, until the red blink starts
    if (radioCueToOn) showColor(LED_WHITE, lvl);
    else writeLed(0, 0, 0);
    return true;
  }
  if (holdStartedOn) {
    if (t < HOLD_TAP_MS) return false; // a tap lets go in this first second and the blue blink answers it
    showColor(LED_RED, (t - HOLD_TAP_MS) % CUE_FLASH_MS < CUE_FLASH_MS / 2 ? lvl : 0);
    return true;
  }
  showColor(LED_GREEN, t % CUE_FLASH_MS < CUE_FLASH_MS / 2 ? lvl : 0); // three flashes, and nothing else answers the press, so the light starts at once
  return true;
}

void serviceLed() {
  const uint32_t now = millis();
  uint8_t level = (uint8_t)((uint16_t)STATUS_RGB_LEVEL * ledBrightnessStep / 10);
  const uint8_t floorLevel = level < STATUS_FLOOR ? STATUS_FLOOR : level;

  if (boardCheckResult != BOARD_CHECK_OK) { showColor(LED_RED, (now / 1000) % 2 ? 0 : floorLevel); return; }
#if FW_BUTTON_STYLE == FW_BUTTON_CME
  if (buttonHeldMs >= HOLD_FORGET_MS) { showColor(LED_RED, floorLevel); return; }
  if (buttonHeldMs >= HOLD_MODE_MS) { showColor(LED_WHITE, floorLevel); return; }
#else
  if (buttonCue(now, floorLevel)) return;
#endif
  if (timerActive(flashUntilMs)) { showColor(LED_GREEN, floorLevel); return; }
  if (learning()) { showColor(LED_BLUE, (now / 250) % 2 ? 0 : floorLevel); return; }
  if (peripheralWindow()) { // a slow triangle, 2 seconds round
    const uint32_t t = now % 2000;
    const uint32_t tri = t < 1000 ? t : 2000 - t;
    showColor(LED_PURPLE, (uint8_t)((uint32_t)floorLevel * tri / 1000));
    return;
  }
  if (!FW_BENCH && lastMainFrameMs != 0 && now - lastMainFrameMs > LINK_SILENT_MS) { showColor(LED_PURPLE, (now / 150) % 2 ? 0 : floorLevel); return; }
  if (!FW_BENCH && lastMainFrameMs == 0 && now > 3000) { showColor(LED_PURPLE, (now / 150) % 2 ? 0 : floorLevel); return; }

  // Activity: fades up for a second while there is traffic, and down for half a second when it stops.
  const float dt = (float)(now - ledFadeMs);
  ledFadeMs = now;
  if (now - activityMs < 100) ledLevel += dt / FADE_IN_MS;
  else ledLevel -= dt / FADE_OUT_MS;
  if (ledLevel < 0) ledLevel = 0;
  if (ledLevel > 1) ledLevel = 1;
  showColor(LED_BLUE, (uint8_t)(ledLevel * level));
}

// ============================================================================================
// Bluetooth callbacks. These run in the Bluetooth task, so they only set flags and fill rings.
// ============================================================================================
Peer *peerByClient(NimBLEClient *c) {
  for (uint8_t h = 1; h <= MODULE_HANDLES; ++h) if (peers[h].client == c) return &peers[h];
  return nullptr;
}

class HostClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *c, int reason) override {
    Peer *p = peerByClient(c);
    if (p) p->connected = false;
    statusDirty = true;
  }
} hostClientCallbacks;

void hostNotify(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify) {
  Peer *p = peerByClient(chr->getClient());
  if (p) p->rx.push(data, len);
}

class DeviceServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *srv, NimBLEConnInfo &info) override {
    peers[0].connHandle = info.getConnHandle();
    memcpy(peers[0].addr, info.getAddress().getVal(), 6);
    peers[0].mtu = 23;
    peers[0].connected = true;
    srv->updateConnParams(info.getConnHandle(), 6, 12, 0, 200); // 7.5 to 15 ms, the phone or computer decides
    statusDirty = true;
  }
  void onDisconnect(NimBLEServer *srv, NimBLEConnInfo &info, int reason) override {
    peers[0].connected = false;
    statusDirty = true;
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo &info) override { peers[0].mtu = mtu; }
} deviceServerCallbacks;

class DeviceCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &info) override {
    const NimBLEAttValue v = chr->getValue();
    peers[0].rx.push(v.data(), v.size());
  }
} deviceCharCallbacks;

class HostScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    if (!dev->isAdvertisingService(MIDI_SERVICE)) return;
    const uint8_t *a = dev->getAddress().getVal();
    if (hostByAddress(a)) return;
    if (refusedMs && millis() - refusedMs < REFUSED_MS && addrEqual(refusedAddr, a)) return;
    if (!learning() && !isKnown(a)) return;
    bool take = false;
    portENTER_CRITICAL(&stateMux);
    if (!connecting && hostsConnected() < allowedHosts && bleHostsConnected() < BLE_HOSTS_MAX && !peripheralWindow()) {
      connecting = true; // only the flag in here, no allocation with the interrupts off
      take = true;
    }
    portEXIT_CRITICAL(&stateMux);
    if (!take) return;
    pendingAddr = dev->getAddress();
    pendingName = dev->getName();
    NimBLEDevice::getScan()->stop();
    xTaskNotifyGive(connectTask);
  }
} hostScanCallbacks;

// ============================================================================================
// Connecting to a Bluetooth MIDI device. Blocks for a few seconds, so it has a task of its own and the link keeps going.
// ============================================================================================
bool connectHost(uint8_t h) {
  NimBLEClient *c = NimBLEDevice::getDisconnectedClient();
  if (!c) {
    if (NimBLEDevice::getCreatedClientCount() >= NIMBLE_MAX_CONNECTIONS) return false;
    c = NimBLEDevice::createClient();
  }
  c->setClientCallbacks(&hostClientCallbacks, false);
  c->setConnectionParams(12, 12, 0, 150); // 15 ms, safe for 3 links
  c->setConnectTimeout(5000);
  if (!c->connect(pendingAddr)) {
    NimBLEDevice::deleteClient(c);
    return false;
  }
  NimBLERemoteService *svc = c->getService(MIDI_SERVICE);
  NimBLERemoteCharacteristic *chr = svc ? svc->getCharacteristic(MIDI_CHARACTERISTIC) : nullptr;
  if (!chr) {
    c->disconnect();
    return false;
  }
  // The client is registered before it subscribes, so the first notifications find their handle. A reused client may still be
  // on another handle from its last connection.
  for (uint8_t k = 1; k <= MODULE_HANDLES; ++k) if (k != h && peers[k].client == c) { peers[k].client = nullptr; peers[k].chr = nullptr; }
  Peer &p = peers[h];
  p.rx.clear();
  p.outRt.clear();
  p.outNormal.clear();
  p.parser.reset();
  p.up.reset();
  p.down.reset();
  p.client = c;
  p.chr = chr;
  p.usb = false;
  if (!chr->subscribe(true, hostNotify)) {
    p.client = nullptr;
    p.chr = nullptr;
    c->disconnect();
    return false;
  }
  memcpy(p.addr, pendingAddr.getVal(), 6);
  memset(p.name, 0, sizeof(p.name));
  strncpy(p.name, pendingName.c_str(), NAME_CHARS);
  if (p.name[0] == 0) { // not in the advertisement, ask the device
    const NimBLEAttValue v = c->getValue(NimBLEUUID((uint16_t)0x1800), NimBLEUUID((uint16_t)0x2A00));
    strncpy(p.name, v.c_str(), NAME_CHARS);
  }
  if (p.name[0] == 0) snprintf(p.name, sizeof(p.name), "BLE %02X%02X%02X", p.addr[2], p.addr[1], p.addr[0]);
  cleanName(p.name);
  p.mtu = c->getMTU();
  p.announced = false;
  p.identityStep = 0;
  p.connected = true;
  statusDirty = true;
  return true;
}

void connectTaskFn(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const uint8_t h = freeHostHandle();
    if (h) connectHost(h);
    lastConnectEndMs = millis();
    connecting = false;
  }
}

// ============================================================================================
// Scanning and advertising
// ============================================================================================
void serviceScan() {
  if (!radioOn) return;
  const uint32_t now = millis();
  NimBLEScan *scan = NimBLEDevice::getScan();
  const bool room = hostsConnected() < allowedHosts && bleHostsConnected() < BLE_HOSTS_MAX;
  const bool want = room && !connecting && !peripheralWindow() && (learning() || knownMissing()) && now - lastConnectEndMs > 1500;
  if (want && !scan->isScanning() && now - lastScanMs > 1000) {
    lastScanMs = now;
    scan->start(0, false, true);
    statusDirty = true;
  } else if (!want && scan->isScanning()) {
    scan->stop();
    statusDirty = true;
  }
}

void serviceAdvertising() {
  if (!radioOn) return;
  const uint32_t now = millis();
  if (peers[0].connected || now - lastAdvMs < 2000) return;
  lastAdvMs = now;
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  if (!adv->isAdvertising()) adv->start();
}

// A USB device is let go and left alone until it is unplugged. A Bluetooth device is disconnected.
void disconnectHost(uint8_t h) {
  Peer &p = peers[h];
  if (p.usb) { usbmidi::release(p.usbIndex); p.connected = false; return; }
  if (p.connected && p.client) p.client->disconnect();
  p.connected = false;
}

// Every Bluetooth link. USB devices stay.
void disconnectAll() {
  for (uint8_t h = 1; h <= MODULE_HANDLES; ++h) if (!peers[h].usb) disconnectHost(h);
  if (peers[0].connected && bleServer) bleServer->disconnect(peers[0].connHandle);
}

// ============================================================================================
// USB MIDI devices on the module's own USB port
// ============================================================================================
// Called from the USB client task.
void usbRxSink(uint8_t index, const uint8_t *data, size_t length) {
  const uint8_t h = usbHandleOf[index];
  if (h) peers[h].rx.push(data, length);
}

// A USB device gets a handle when there is room for it, as a Bluetooth device does. With no room it waits, plugged in and unused, and
// starts working when the main brain says there is a slot.
void serviceUsb() {
  if (!usbHostActive) return;
  for (uint8_t i = 0; i < usbmidi::kMaxDevices; ++i) {
    const uint8_t h = usbHandleOf[i];
    const uint8_t st = usbmidi::state(i);
    if (h) {
      if (st != usbmidi::kActive && !usbmidi::pending(i)) { // unplugged, or let go
        peers[h].connected = false;
        usbHandleOf[i] = 0;
        statusDirty = true;
      }
      continue;
    }
    if (st != usbmidi::kOpened || usbmidi::ignored(i) || usbmidi::pending(i)) continue;
    if (hostsConnected() >= allowedHosts) continue;
    const uint8_t nh = freeHostHandle();
    if (!nh) continue;
    const usbmidi::Device &d = usbmidi::device(i);
    Peer &p = peers[nh];
    p.rx.clear();
    p.outRt.clear();
    p.outNormal.clear();
    p.up.reset();
    p.down.reset();
    p.client = nullptr;
    p.chr = nullptr;
    p.usb = true;
    p.usbIndex = i;
    p.addr[0] = (uint8_t)d.vid; p.addr[1] = (uint8_t)(d.vid >> 8);
    p.addr[2] = (uint8_t)d.pid; p.addr[3] = (uint8_t)(d.pid >> 8);
    p.addr[4] = (uint8_t)d.serialHash; p.addr[5] = (uint8_t)(d.serialHash >> 8);
    memset(p.name, 0, sizeof(p.name));
    strncpy(p.name, d.name, NAME_CHARS);
    cleanName(p.name);
    p.announced = false;
    p.identityStep = 0;
    usbHandleOf[i] = nh;
    p.connected = true;
    statusDirty = true;
    usbmidi::activate(i);
  }
}

// The module's USB port as a USB MIDI device: a computer lists it as USB_DEVICE_NAME and it is a second way into BLE Device's link port.
// There is no serial port on it, nothing in the USB descriptor but MIDI.
void startUsbDevice() {
  USB.manufacturerName(USB_MAKER_NAME);
  USB.productName(USB_DEVICE_NAME);
  usbMidi = new USBMIDI(USB_DEVICE_NAME);
  usbDeviceActive = USB.begin();
}

// What the module's USB port is this run: the saved setting, host unless the user chose device in Patchling. It never changes by itself.
// If the host does not start it keeps trying, so a device or a power supply that is the cause can be fixed and the port then works.
void startUsbRole() {
#if FW_BENCH
  benchSerial.begin();
  USB.begin(); // bench mode: the port is a serial port, no MIDI
  return;
#else
  const uint8_t role = FW_USB_HOST ? prefs.getUChar("usbrole", 0) : 1; // 0 host, 1 device
#if FW_USB_HOST
  if (role == 0) {
    usbHostActive = usbmidi::begin(usbRxSink);
    if (!usbHostActive) usbHostRetryMs = millis() + 3000;
  }
#endif
  if (role == 1) startUsbDevice();
#endif
}

// A computer on the module's USB device port. It is mounted when the computer has configured the port. Packets from it wait in the ring
// for pumpBleIn(), packets for it go out in pumpBleOut().
void serviceUsbDevice() {
  if (!usbDeviceActive) return;
  Peer &p = peers[PEER_USB_DEVICE];
  const bool mounted = (bool)USB;
  if (mounted && !p.connected) {
    p.rx.clear();
    p.outRt.clear();
    p.outNormal.clear();
    p.up.reset();
    p.down.reset();
    p.usb = true;
    p.usbDevice = true;
    p.announced = false;
    p.connected = true;
    statusDirty = true;
  } else if (!mounted && p.connected) {
    p.connected = false;
    statusDirty = true;
  }
  if (!p.connected) return;
  // Only read while the ring has room, so a busy link leaves the packets in TinyUSB's own buffer and the computer is told to wait.
  for (uint8_t round = 0; round < 4 && !p.rx.full(); ++round) {
    uint8_t buf[64];
    uint8_t n = 0;
    midiEventPacket_t e;
    while (n + 4 <= sizeof(buf) && usbMidi->readPacket(&e)) { buf[n++] = e.header; buf[n++] = e.byte1; buf[n++] = e.byte2; buf[n++] = e.byte3; }
    if (!n) break;
    p.rx.push(buf, n);
  }
}

// ============================================================================================
// Button
// ============================================================================================
void startLearning() {
  if (!radioOn || allowedHosts == 0) return; // the radio is off, or no room, the main brain says so
  learnUntilMs = millis() + LEARN_MS;
  peripheralUntilMs = 0;
  lastAdvMs = 0;
  if (!peers[0].connected) NimBLEDevice::startAdvertising(); // a phone or computer can connect during the minute as well
  statusDirty = true;
}

void stopLearning() {
  if (learnUntilMs) statusDirty = true;
  learnUntilMs = 0;
}

void forcePeripheral() {
  if (!radioOn) return;
  learnUntilMs = 0;
  disconnectAll();
  peripheralUntilMs = millis() + PERIPHERAL_MS;
  lastAdvMs = 0;
  NimBLEDevice::startAdvertising();
  statusDirty = true;
}

void forgetEverything() {
  learnUntilMs = 0;
  peripheralUntilMs = 0;
  memset(knownValid, 0, sizeof(knownValid));
  disconnectAll();
  if (radioOn) NimBLEDevice::deleteAllBonds();
  clearEventsLeft = 3; // sent with the next heartbeats, so a busy link does not lose it
  statusDirty = true;
}

void shortPress(uint32_t now, uint32_t &lastShortMs) {
#if FW_BUTTON_STYLE == FW_BUTTON_CME
  stopLearning();
  if (lastShortMs && now - lastShortMs < DOUBLE_PRESS_MS) { // the second press of a double press
    lastShortMs = 0;
    startLearning();
  } else {
    lastShortMs = now;
  }
#else
  // A tap while adding stops it, otherwise it starts adding. Nothing that is connected is dropped either way.
  if (learning()) stopLearning();
  else startLearning();
  bannerPrinted = false;
#endif
}

#if FW_BUTTON_STYLE == FW_BUTTON_CME
void serviceButton() {
  static bool down = false;
  static uint32_t downMs = 0, edgeMs = 0, lastShortMs = 0;
  const uint32_t now = millis();
  const bool pressed = digitalRead(PIN_BOOT) == LOW;
  if (pressed && !down && now - edgeMs > 25) {
    down = true;
    downMs = edgeMs = now;
  } else if (pressed && down) {
    buttonHeldMs = now - downMs;
  } else if (!pressed && down && now - edgeMs > 25) {
    down = false;
    edgeMs = now;
    const uint32_t held = now - downMs;
    buttonHeldMs = 0;
    if (held >= HOLD_FORGET_MS) forgetEverything();
#if FW_BUTTON_STYLE == FW_BUTTON_CME
    else if (held >= HOLD_MODE_MS) {
      forcePeripheral();
      bannerPrinted = false;
    }
#endif
    else if (held >= 30) {
      shortPress(now, lastShortMs);
    }
  }
  if (lastShortMs && now - lastShortMs > DOUBLE_PRESS_MS) lastShortMs = 0;
}
#else
// The switch, as the white begins: the Bluetooth radio goes on if it was off and off if it was on. It is saved now, the white fade runs to
// the end whatever the button does, and the restart waits for the fade and for the button to come up.
void radioStep() {
  radioCueStartMs = millis();
  if (!radioCueStartMs) radioCueStartMs = 1;
  radioCueToOn = !holdStartedOn;
#if !FW_BENCH
  prefs.putUChar("radio", holdStartedOn ? 1 : 0); // 1 is off
  restartAfterRelease = true;
#endif
}

// The 10 s step: every Bluetooth device is forgotten at once, with a red fade-out. The hold passed the radio step on the way, so the radio
// setting is put back to on, the same as a new unit. It only needs a restart if the module started with the radio off.
void clearStep() {
  forgetEverything();
  clearFadeStartMs = millis();
  if (!clearFadeStartMs) clearFadeStartMs = 1;
#if !FW_BENCH
  prefs.putUChar("radio", 0); // 0 is on
  restartAfterRelease = !radioOn;
#endif
}

void serviceButton() {
  static bool down = false;
  static uint32_t downMs = 0, edgeMs = 0, lastShortMs = 0;
  const uint32_t now = millis();
  const bool pressed = digitalRead(PIN_BOOT) == LOW;
  if (pressed && !down && now - edgeMs > 25) {
    down = true;
    downMs = edgeMs = now;
    buttonDown = true;
    holdStartedOn = radioOn;
    holdRadioDone = holdClearDone = false;
    radioCueStartMs = 0;
  } else if (pressed && down) {
    buttonHeldMs = now - downMs;
    if (!holdRadioDone && buttonHeldMs >= holdSwitchMs()) { holdRadioDone = true; radioStep(); }
    if (!holdClearDone && buttonHeldMs >= HOLD_FORGET_MS) { holdClearDone = true; clearStep(); }
  } else if (!pressed && down && now - edgeMs > 25) {
    down = false;
    edgeMs = now;
    const uint32_t held = now - downMs;
    buttonHeldMs = 0;
    buttonDown = false;
    if (restartAfterRelease) {
      uint32_t wait = 150;
      if (radioCueStartMs && (int32_t)(radioCueStartMs + CUE_FADE_MS - now) > 0) wait = radioCueStartMs + CUE_FADE_MS - now + 100; // the white finishes first
      if (holdClearDone && wait < 1100) wait = 1100; // after a forget, the clear messages to the main brain go out first
      restartAtMs = now + wait;
      if (!restartAtMs) restartAtMs = 1;
    } else if (held >= 30 && held < HOLD_TAP_MS && holdStartedOn) {
      shortPress(now, lastShortMs);
    }
  }
}
#endif

// ============================================================================================
// Firmware update from the main brain (module firmware 2), see foxwizard_link.h. The image goes into the other app slot and the
// module restarts into it after the END block. Bluetooth carries on while it is open, 15 s of silence drops the update.
// ============================================================================================
UpdBlockReader updReader;
bool updateOpen = false;
uint16_t updateNextChunk = 0;
uint32_t updateLastMs = 0;

void updateReply(uint8_t cmd, uint8_t status, uint8_t extra) {
  uint8_t pkt[4];
  updControl(UPD_REPLY, cmd, status, extra, pkt);
  sendToMain(LINK_PORT_CONTROL, pkt);
}

void updateDrop() {
  if (Update.isRunning()) Update.abort();
  updateOpen = false;
}

void updateBlock(const UpdBlock &b) {
  updateLastMs = millis();
  if (b.cmd == UPD_CMD_BEGIN) {
    if (b.len < 19) { updateReply(b.cmd, UPD_BAD_BLOCK, 0); return; }
    const uint32_t size = ((uint32_t)b.data[0] << 16) | ((uint32_t)b.data[1] << 8) | b.data[2];
    char md5[33];
    for (uint8_t i = 0; i < 16; ++i) {
      md5[i * 2] = "0123456789abcdef"[b.data[3 + i] >> 4];
      md5[i * 2 + 1] = "0123456789abcdef"[b.data[3 + i] & 15];
    }
    md5[32] = '\0';
    updateDrop();
    if (!Update.begin(size) || !Update.setMD5(md5)) {
      const uint8_t error = Update.getError();
      updateDrop();
      updateReply(b.cmd, UPD_ERROR, error);
      return;
    }
    updateOpen = true;
    updateNextChunk = 0;
    updateReply(b.cmd, UPD_OK, 0);
  } else if (b.cmd == UPD_CMD_DATA) {
    if (!updateOpen || !Update.isRunning() || b.len < 3) { updateReply(b.cmd, UPD_NOT_READY, 0); return; }
    const uint16_t idx = (uint16_t)(((uint16_t)b.data[0] << 8) | b.data[1]);
    if (updateNextChunk > 0 && idx == updateNextChunk - 1) { updateReply(b.cmd, UPD_OK, (uint8_t)idx); return; } // its reply was lost
    if (idx != updateNextChunk) { updateReply(b.cmd, UPD_BAD_INDEX, (uint8_t)updateNextChunk); return; }
    const uint8_t n = (uint8_t)(b.len - 2);
    uint8_t chunk[UPD_CHUNK_BYTES];
    memcpy(chunk, b.data + 2, n);
    if (Update.write(chunk, n) != n) { updateReply(b.cmd, UPD_ERROR, Update.getError()); return; }
    ++updateNextChunk;
    updateReply(b.cmd, UPD_OK, (uint8_t)idx);
  } else if (b.cmd == UPD_CMD_END) {
    if (!updateOpen || !Update.isRunning()) { updateReply(b.cmd, UPD_NOT_READY, 0); return; }
    if (!Update.end()) { updateReply(b.cmd, UPD_ERROR, Update.getError()); return; } // checks the size and MD5, then picks the new slot
    updateOpen = false;
    updateReply(b.cmd, UPD_OK, 0);
    restartAtMs = millis() + 300; // the reply gets out first
    if (!restartAtMs) restartAtMs = 1;
  }
}

void updateFrame(const uint8_t pkt[4]) {
  updateLastMs = millis();
  const UpdBlockReader::Result r = updReader.feed(pkt);
  if (r == UpdBlockReader::DONE) updateBlock(updReader.block);
  else if (r == UpdBlockReader::BAD) updateReply(updReader.block.cmd, UPD_BAD_BLOCK, 0);
}

// ============================================================================================
// From the main brain
// ============================================================================================
void handleControl(const uint8_t pkt[4]) {
  const uint8_t type = pkt[0] & 0x0F;
  if (type == CONTROL_UPDATE) {
    updateFrame(pkt);
  } else if (type == CONTROL_FLOW) {
    mainAskedPause = pkt[1] == 1;
    mainFlowMs = millis();
  } else if (type == CONTROL_BRIGHTNESS) {
    ledBrightnessStep = pkt[1] > 10 ? 10 : pkt[1];
  } else if (type == CONTROL_BLE_KNOWN) {
    const uint8_t part = pkt[1] >> 4, slot = pkt[1] & 0x0F;
    static uint8_t staged[BLE_HANDLES + 1][6];
    if (slot < 1 || slot > BLE_HANDLES || part > 2) return;
    staged[slot][part * 2] = pkt[2];
    staged[slot][part * 2 + 1] = pkt[3];
    if (part == 2) {
      memcpy(knownAddr[slot], staged[slot], 6);
      knownValid[slot] = !addrZero(knownAddr[slot]);
    }
    lastKnownMs = millis();
  } else if (type == CONTROL_BLE_COMMAND) {
    const uint8_t value = pkt[2];
    switch (pkt[1]) {
      case BLE_CMD_LEARN: if (value) startLearning(); else stopLearning(); break;
      case BLE_CMD_ALLOW: allowedHosts = value > MODULE_HANDLES ? MODULE_HANDLES : value; break;
      case BLE_CMD_USB_ROLE: { // saved, and the module restarts so the port starts as the other kind
        Preferences saved;
        saved.begin("fwbt", false);
        saved.putUChar("usbrole", value ? 1 : 0);
        saved.end();
        restartAtMs = millis() + 400;
        break;
      }
      case BLE_CMD_RADIO: { // saved, and the module restarts with the radio on or off
        Preferences saved;
        saved.begin("fwbt", false);
        saved.putUChar("radio", value ? 1 : 0);
        saved.end();
        restartAtMs = millis() + 400;
        break;
      }
      case BLE_CMD_FACTORY: { // the main brain was factory reset: as a new unit, nothing remembered, the radio on and the USB port a host
        forgetEverything();
        Preferences saved;
        saved.begin("fwbt", false);
        saved.putUChar("usbrole", 0);
        saved.putUChar("radio", 0);
        saved.end();
        restartAtMs = millis() + 400;
        break;
      }
      case BLE_CMD_DISCONNECT:
        if (value >= 1 && value <= MODULE_HANDLES && peers[value].connected) {
          memcpy(refusedAddr, peers[value].addr, 6);
          refusedMs = millis();
          disconnectHost(value);
        }
        break;
      case BLE_CMD_FORGET:
        if (value >= 1 && value <= MODULE_HANDLES) {
          for (uint8_t s = 1; s <= BLE_HANDLES; ++s) if (addrEqual(knownAddr[s], peers[value].addr)) knownValid[s] = false;
          memcpy(refusedAddr, peers[value].addr, 6);
          refusedMs = millis();
          disconnectHost(value);
        }
        break;
      default: break;
    }
    statusDirty = true;
  }
}

void queueToPeer(Peer &p, const uint8_t pkt[4]) {
  if (!p.connected) return;
  const bool realtime = (pkt[0] & 0x0F) == 0x0F;
  if (realtime) p.outRt.push(MidiPacket{{pkt[0], pkt[1], pkt[2], pkt[3]}}, true);
  else p.outNormal.push(MidiPacket{{pkt[0], pkt[1], pkt[2], pkt[3]}}, isCriticalWord(((uint32_t)0x2 << 28) | ((uint32_t)pkt[1] << 16) | ((uint32_t)pkt[2] << 8) | pkt[3]));
}

void pumpLinkIn() {
  uint16_t budget = LINK_RX_BUDGET;
  while (budget-- && linkSerial.available() > 0) {
    if (!linkReader.feed((uint8_t)linkSerial.read())) continue;
    uint32_t word;
    bool first;
    const uint8_t port = decodeLinkWord(linkReader.frame, word, first);
    lastMainFrameMs = millis();
    if (port == LINK_PORT_CONTROL) {
      const uint8_t pkt[4] = {(uint8_t)(word >> 24), (uint8_t)(word >> 16), (uint8_t)(word >> 8), (uint8_t)word};
      handleControl(pkt);
      continue;
    }
    Peer *targets[2];
    const uint8_t targetCount = peersForPort(port, targets);
    if (first && isRealtimeWord(word)) {
      const uint8_t pkt[4] = {0x0F, (uint8_t)(word >> 16), 0, 0};
      for (uint8_t t = 0; t < targetCount; ++t) queueToPeer(*targets[t], pkt);
      continue;
    }
    uint32_t message[4];
    const uint8_t words = linkMessage.feed(word, first, message);
    if (!words) continue;
    for (uint8_t t = 0; t < targetCount; ++t) {
      Peer *p = targets[t];
      if (!p->connected) continue;
      uint8_t packets[8][4];
      const uint8_t made = p->down.push(message, words, packets);
      for (uint8_t k = 0; k < made; ++k) queueToPeer(*p, packets[k]);
    }
  }
  // Ask the main brain to pause while bytes pile up faster than they are parsed.
  const int waiting = linkSerial.available();
  const bool wantPause = pausingMain ? waiting > LINK_RX_RESUME_AT : waiting > LINK_RX_PAUSE_AT;
  if (wantPause != pausingMain || millis() - lastFlowSentMs >= FLOW_REPEAT_MS) {
    pausingMain = wantPause;
    const uint8_t pkt[4] = {CONTROL_FLOW, (uint8_t)(wantPause ? 1 : 0), 0, 0};
    sendToMain(LINK_PORT_CONTROL, pkt);
    lastFlowSentMs = millis();
  }
}

// ============================================================================================
// Bluetooth MIDI in and out
// ============================================================================================
void pumpBleIn() {
  for (uint8_t i = 0; i < PEER_COUNT; ++i) {
    Peer &p = peers[i];
    RxRing::Item *item;
    while ((item = p.rx.peek()) != nullptr) {
      if (toMainNormal.freeSlots(false) < 64) return; // the link is behind, the data waits in the ring
      if (p.usb) { // already USB MIDI packets, 4 bytes each
        for (uint16_t k = 0; k + 3 < item->len; k += 4) {
          const uint8_t *pkt = item->data + k;
          if ((pkt[0] & 0x0F) < 2) continue;
          if (p.announced) sendPacketToMain(peerPort(i), p.up, pkt);
          noteActivity();
        }
        p.rx.release();
        continue;
      }
      p.parser.feed(item->data, item->len, [&](const uint8_t pkt[4]) {
        if ((pkt[0] & 0x0F) < 2) return;
#if FW_BENCH
        dbg.printf("%s %u:", i == 0 ? "device" : "host", (unsigned)i);
        for (uint8_t k = 1; k < 4 && k <= fw::blemidi::kCinBytes[pkt[0] & 0x0F]; ++k) dbg.printf(" %02X", pkt[k]);
        dbg.println();
#endif
        if (p.announced) sendPacketToMain(peerPort(i), p.up, pkt);
        noteActivity();
      });
      p.rx.release();
    }
  }
}

bool sendBle(Peer &p, const uint8_t *data, uint16_t len) {
  if (!p.connected) return false;
  if (p.client) return p.chr && p.chr->writeValue(data, len, false);
  return deviceChr && deviceChr->notify(data, len, p.connHandle);
}

void pumpBleOut() {
  const uint16_t stamp = (uint16_t)(millis() & 0x1FFF);
  for (uint8_t i = 0; i < PEER_COUNT; ++i) {
    Peer &p = peers[i];
    if (!p.connected) continue;
    if (p.usbDevice) { // the computer on the module's USB device port, a packet at a time while TinyUSB has room
      for (uint8_t sent = 0; sent < 16; ++sent) {
        bool realtime = true;
        MidiPacket *pk = p.outRt.peekAt(0);
        if (!pk) { realtime = false; pk = p.outNormal.peekAt(0); }
        if (!pk) break;
        midiEventPacket_t e = {(uint8_t)(pk->b[0] & 0x0F), pk->b[1], pk->b[2], pk->b[3]};
        if (!usbMidi->writePacket(&e)) break;
        if (realtime) p.outRt.pop(); else p.outNormal.pop();
        noteActivity();
      }
      continue;
    }
    if (p.usb) { // USB MIDI packets as they are, a few at a time
      uint8_t buf[64];
      uint16_t n = 0, takenRt = 0, takenNormal = 0;
      MidiPacket *pk;
      while (n + 4 <= sizeof(buf) && (pk = p.outRt.peekAt(takenRt)) != nullptr) { buf[n] = pk->b[0] & 0x0F; buf[n + 1] = pk->b[1]; buf[n + 2] = pk->b[2]; buf[n + 3] = pk->b[3]; n += 4; ++takenRt; }
      while (n + 4 <= sizeof(buf) && takenRt == p.outRt.size() && (pk = p.outNormal.peekAt(takenNormal)) != nullptr) { buf[n] = pk->b[0] & 0x0F; buf[n + 1] = pk->b[1]; buf[n + 2] = pk->b[2]; buf[n + 3] = pk->b[3]; n += 4; ++takenNormal; }
      if (n && usbmidi::send(p.usbIndex, buf, n)) {
        for (uint16_t k = 0; k < takenRt; ++k) p.outRt.pop();
        for (uint16_t k = 0; k < takenNormal; ++k) p.outNormal.pop();
        noteActivity();
      }
      continue;
    }
    if (p.client) p.mtu = p.client->getMTU();
    const uint16_t limit = p.mtu > 23 ? (p.mtu - 3 > 180 ? 180 : p.mtu - 3) : 20;
    for (uint8_t round = 0; round < 2; ++round) { // at most two Bluetooth packets for a link in a pass
      fw::blemidi::Writer w;
      w.setLimit(limit);
      uint16_t takenRt = 0, takenNormal = 0;
      MidiPacket *pk;
      while ((pk = p.outRt.peekAt(takenRt)) != nullptr && w.add(pk->b, stamp)) ++takenRt;
      while (takenRt == p.outRt.size() && (pk = p.outNormal.peekAt(takenNormal)) != nullptr && w.add(pk->b, stamp)) ++takenNormal;
      if (w.empty()) break;
      if (!sendBle(p, w.bytes(), w.size())) break; // the stack is full, try again next pass
      for (uint16_t k = 0; k < takenRt; ++k) p.outRt.pop();
      for (uint16_t k = 0; k < takenNormal; ++k) p.outNormal.pop();
      noteActivity();
    }
  }
}

// ============================================================================================
// Control out to the main brain
// ============================================================================================
void sendIdentity(uint8_t h) {
  Peer &p = peers[h];
  for (uint8_t part = 0; part < 3; ++part) {
    const uint8_t pkt[4] = {CONTROL_BLE_ADDRESS, (uint8_t)((part << 4) | h), p.addr[part * 2], p.addr[part * 2 + 1]};
    sendToMain(LINK_PORT_CONTROL, pkt);
  }
  const uint8_t kind[4] = {CONTROL_BLE_ADDRESS, (uint8_t)((3 << 4) | h), p.usb ? MODULE_KIND_USB : MODULE_KIND_BLE, 0};
  sendToMain(LINK_PORT_CONTROL, kind);
  for (uint8_t chunk = 0; chunk < NAME_CHUNKS; ++chunk) {
    uint8_t chars[2];
    nameChunk(p.name, chunk, chars);
    const uint8_t pkt[4] = {CONTROL_DEVICE_NAME, (uint8_t)((chunk << 4) | h), chars[0], chars[1]};
    sendToMain(LINK_PORT_CONTROL, pkt);
  }
}

void serviceConnections() {
  for (uint8_t i = 0; i < PEER_COUNT; ++i) {
    Peer &p = peers[i];
    if (p.connected && !p.announced) {
      p.announced = true;
      flashUntilMs = millis() + CONNECT_FLASH_MS;
      if (!isHostPeer(i)) peripheralUntilMs = 0; // the phone or computer got in, normal service again
      if (isHostPeer(i) && toMainRt.freeSlots(false) > 24) sendIdentity((uint8_t)i);
      statusDirty = true;
#if FW_BENCH
      dbg.printf("connected %s %u: %s\n", isHostPeer(i) ? "host" : "device", (unsigned)i, isHostPeer(i) ? p.name : "central");
#endif
    } else if (!p.connected && p.announced) {
      p.announced = false;
      if (isHostPeer(i)) { p.client = nullptr; p.chr = nullptr; p.usb = false; }
      p.rx.clear();
      p.outRt.clear();
      p.outNormal.clear();
      p.parser.reset();
      p.up.reset();
      p.down.reset();
      statusDirty = true;
#if FW_BENCH
      dbg.printf("disconnected %s %u\n", isHostPeer(i) ? "host" : "device", (unsigned)i);
#endif
    }
  }
}

void serviceControlOut() {
  const uint32_t now = millis();
  if (statusDirty || now - lastBeatMs >= HEARTBEAT_MS) {
    statusDirty = false;
    lastBeatMs = now;
    uint8_t mask = 0;
    for (uint8_t h = 1; h <= MODULE_HANDLES; ++h) if (peers[h].connected && peers[h].announced) mask |= (uint8_t)(1 << (h - 1));
    const uint32_t drops = totalDrops();
    const uint8_t map[4] = {CONTROL_PORT_MAP, mask, BT_FW_VERSION, (uint8_t)(drops > 127 ? 127 : drops)};
    sendToMain(LINK_PORT_CONTROL, map);
    const uint8_t flags = (uint8_t)((deviceConnected() ? 1 : 0) | (learning() ? 2 : 0) | (radioOn && NimBLEDevice::getScan()->isScanning() ? 4 : 0) | (peripheralWindow() ? 8 : 0) | (usbHostActive ? 16 : 0) | (radioOn ? 0 : 64));
    const uint8_t st[4] = {CONTROL_BLE_STATUS, flags, hostsConnected(), boardCheckResult};
    sendToMain(LINK_PORT_CONTROL, st);
    if (clearEventsLeft) {
      --clearEventsLeft;
      const uint8_t ev[4] = {CONTROL_BLE_EVENT, BLE_EVENT_CLEAR_ALL, 0, 0};
      sendToMain(LINK_PORT_CONTROL, ev);
    }
  }
  // Each connected host's address and name again now and then, so the main brain gets them even if the first pass was missed.
  if (now - lastIdentityMs >= IDENTITY_MS * 20) {
    lastIdentityMs = now;
    for (uint8_t n = 0; n < MODULE_HANDLES; ++n) {
      identityPeer = (uint8_t)(identityPeer % MODULE_HANDLES + 1);
      if (peers[identityPeer].connected && peers[identityPeer].announced && toMainRt.freeSlots(false) > 24) { sendIdentity(identityPeer); break; }
    }
  }
}

// ============================================================================================
// Setup and loop
// ============================================================================================
void setup() {
  pinMode(PIN_BOOT, INPUT_PULLUP);
  neopixelWrite(PIN_STATUS_RGB, 0, 0, 0);
  prefs.begin("fwbt", false);
  radioOn = FW_BENCH || prefs.getUChar("radio", 0) == 0;

  linkSerial.setRxBufferSize(4096);
  linkSerial.setTxBufferSize(1024);
  linkSerial.begin(LINK_BAUD, SERIAL_8N1, PIN_LINK_RX, PIN_LINK_TX);

  boardCheckResult = boardCheckRun(dbg, MODULE_NAME, BT_FW_VERSION);

  if (radioOn) {
    NimBLEDevice::init("FoxWizard");
    NimBLEDevice::setMTU(185);

    // BLE Device
    bleServer = NimBLEDevice::createServer();
    bleServer->setCallbacks(&deviceServerCallbacks, false);
    NimBLEService *svc = bleServer->createService(MIDI_SERVICE);
    deviceChr = svc->createCharacteristic(MIDI_CHARACTERISTIC, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    deviceChr->setCallbacks(&deviceCharCallbacks);
    svc->start();
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->setName("FoxWizard");
    adv->addServiceUUID(MIDI_SERVICE);
    adv->enableScanResponse(true);
    adv->start();

    // BLE Host
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&hostScanCallbacks, true);
    scan->setActiveScan(true);
    scan->setInterval(120);
    scan->setWindow(60);

    xTaskCreate(connectTaskFn, "bleconn", 6144, nullptr, 1, &connectTask);
  }
  startUsbRole(); // after Bluetooth is up, a USB host that fails then costs nothing else
  ledFadeMs = millis();
}

void loop() {
#if FW_USB_HOST && !FW_BENCH
  if (usbHostRetryMs && (int32_t)(millis() - usbHostRetryMs) >= 0) { // the host did not start: it keeps trying, the port never changes role by itself
    usbHostActive = usbmidi::begin(usbRxSink);
    usbHostRetryMs = usbHostActive ? 0 : millis() + 3000;
  }
#endif
  if (restartAtMs && (int32_t)(millis() - restartAtMs) >= 0) {
#if !FW_BENCH
    if (digitalRead(PIN_BOOT) == LOW) restartAtMs = millis() + 100; // never reset with BOOT held down, the chip could start its upload mode
    else
#endif
      ESP.restart();
  }
  if (updateOpen && millis() - updateLastMs > UPD_SILENCE_MS) updateDrop(); // the main brain went quiet: the update is given up
#if FW_BENCH
  if (!bannerPrinted && millis() > 3000) { // after a serial monitor has had time to open
    bannerPrinted = true;
    boardCheckRun(dbg, MODULE_NAME, BT_FW_VERSION);
    dbg.printf("  allowed hosts %u, learning %s, button style %s\n", allowedHosts, learning() ? "on" : "off", FW_BUTTON_STYLE == FW_BUTTON_CME ? "CME" : "one mode");
  }
#endif
  serviceButton();
  pumpLinkIn();
  serviceUsb();
  serviceUsbDevice();
  serviceConnections();
  pumpBleIn();
  pumpBleOut();
  serviceScan();
  serviceAdvertising();
  serviceControlOut();
  pumpLinkOut();
  serviceLed();
  delay(1);
}

// arduino-esp32 3.3.x gives the Bluetooth controller's memory back at boot unless a library says Bluetooth is in use, and then
// NimBLEDevice::init() crashes inside esp_bt_controller_init(), the module reboots forever. The core's own BLE library says it is in
// use, NimBLE-Arduino does not, so the sketch does. Harmless on a core that does not look for it.
extern "C" bool bleInUse(void) { return true; }
