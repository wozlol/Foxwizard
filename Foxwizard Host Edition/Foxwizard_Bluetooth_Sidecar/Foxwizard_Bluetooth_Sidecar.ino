/*
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
  - the main brain's GP28 on EN and GP29 on IO0, so it can flash this module over the same two wires (not in this version)
  - GND shared

  Arduino setup: the Tools settings are in board_check.h, and the build stops with the one to change if one is wrong.

  The button is the module's BOOT button (GPIO0), pressed while it runs. At power on the remembered devices reconnect on their own.
  - style FW_BUTTON_ONE_MODE (the default), one add mode for both roles, nothing to choose:
    - Hold 3 seconds, let go: add devices for a minute. A phone or computer can connect to BLE Device (it advertises all the time),
      and the module scans for Bluetooth MIDI devices and connects the ones it finds, up to the room there is, remembering each at once.
    - Short press: stop adding early.
  - style FW_BUTTON_CME, CME's way, in case it is ever needed:
    - Hold 3 seconds, let go: force peripheral. Drops every Bluetooth link and advertises for a minute so a phone or computer
      can connect. Then the remembered devices reconnect on their own.
    - Double press: learn hosts for a minute, the same adding. Short press: stop learning.
  - Both: hold 10 seconds, let go, to forget every Bluetooth device and bond.
  The light goes white at 3 seconds held and red at 10, so you know what letting go will do.

  Status light, the module's own WS2812, off by default, same no-blink ethic as the other lights:
  - blue: fades up on Bluetooth MIDI and fades back down, 1 s up and 1/2 s down, following the main brain's brightness knob
  - green: a short flash for each new Bluetooth connection
  - blue, blinking: adding devices
  - purple, pulsing slowly: force peripheral (CME style only), waiting for a phone or computer
  - purple, blinking fast: nothing heard from the main brain for over a second
  - red, blinking slowly: the Tools settings are wrong, see the serial monitor
*/

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

#include "board_config.h"
#include "board_check.h"
#include "foxwizard_link.h"
#include "ble_midi.h"
#include "usb_midi_host.h"

static_assert(MODULE_HANDLES >= 1 && MODULE_HANDLES <= BLE_HANDLES, "MODULE_HANDLES is 1 to 8");
static_assert(BLE_HOSTS_MAX >= 1 && BLE_HOSTS_MAX <= MODULE_HANDLES, "BLE_HOSTS_MAX is 1 to MODULE_HANDLES");
static_assert(BLE_HOSTS_MAX + 1 <= NIMBLE_MAX_CONNECTIONS, "Raise CONFIG_BT_NIMBLE_MAX_CONNECTIONS in NimBLE-Arduino's nimconfig.h to BLE_HOSTS_MAX + 1, or lower BLE_HOSTS_MAX in board_config.h");

// ============================================================================================
// Constants
// ============================================================================================
constexpr uint8_t BT_FW_VERSION = 1;
constexpr uint32_t LEARN_MS = 60000;
constexpr uint32_t PERIPHERAL_MS = 60000;
constexpr uint32_t HEARTBEAT_MS = 500;
constexpr uint32_t IDENTITY_MS = 250;
constexpr uint32_t KNOWN_SILENCE_MS = 5000; // the main brain repeats its list this often
constexpr uint32_t HOLD_FORGET_MS = 10000;
constexpr uint32_t HOLD_MODE_MS = 3000;
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
  Item *peek() { return tail == head ? nullptr : &items[tail]; }
  void release() { __sync_synchronize(); tail = (uint8_t)((tail + 1) % SLOTS); }
  void clear() { tail = head; }
};

// Index 0 is BLE Device, 1 and up are BLE Hosts. Its link port is the index, except BLE Device which is LINK_PORT_BLE_DEVICE.
struct Peer {
  volatile bool connected = false;
  bool announced = false;                 // loop() has seen it connect
  uint16_t connHandle = 0xFFFF;           // BLE Device: which central
  uint16_t mtu = 23;
  bool usb = false;                       // a USB MIDI device on the module's USB host port, not a Bluetooth one
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

enum LedColor : uint8_t { LED_NONE = 0, LED_BLUE, LED_GREEN, LED_PURPLE, LED_RED, LED_WHITE, LED_ORANGE };

// ============================================================================================
// Globals
// ============================================================================================
Peer peers[1 + MODULE_HANDLES];
uint8_t usbHandleOf[usbmidi::kMaxDevices]; // the handle each USB MIDI device has, 0 none
Preferences prefs;
bool usbHostActive = false;                // the module's USB port is a host port this run
bool usbFailsafe = false;                  // the host failed to start last time, the port is a service port until told otherwise
uint32_t usbTryClearMs = 0;
uint32_t restartAtMs = 0;
NimBLEServer *bleServer = nullptr;
NimBLECharacteristic *deviceChr = nullptr;
TaskHandle_t connectTask = nullptr;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;

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
uint8_t peerPort(uint8_t i) { return i == 0 ? LINK_PORT_BLE_DEVICE : i; }
Peer *peerForPort(uint8_t port) {
  if (port == LINK_PORT_BLE_DEVICE) return &peers[0];
  if (port >= 1 && port <= MODULE_HANDLES) return &peers[port];
  return nullptr;
}

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
    case LED_ORANGE: r = level; g = level / 3; break;
    default: break;
  }
  writeLed(r, g, b);
}

void serviceLed() {
  const uint32_t now = millis();
  uint8_t level = (uint8_t)((uint16_t)STATUS_RGB_LEVEL * ledBrightnessStep / 10);
  const uint8_t floorLevel = level < STATUS_FLOOR ? STATUS_FLOOR : level;

  if (boardCheckResult != BOARD_CHECK_OK) { showColor(LED_RED, (now / 1000) % 2 ? 0 : floorLevel); return; }
  if (buttonHeldMs >= HOLD_FORGET_MS) { showColor(LED_RED, floorLevel); return; }
  if (buttonHeldMs >= HOLD_MODE_MS) { showColor(LED_WHITE, floorLevel); return; }
  if (usbFailsafe && now < 30000) { showColor(LED_ORANGE, (now / 500) % 2 ? 0 : floorLevel); return; } // the USB host did not start, see the module's status in Patchling
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

// What the module's USB port is this run. A saved setting, host unless told otherwise. If the host crashed the last time it started, or
// three starts in a row never got through 5 seconds, the port falls back to the chip's USB serial so the module can always be reached
// and flashed. A power cut a second after start is not a crash, so one of them alone does not move the port.
void startUsbRole() {
#if FW_BENCH || !FW_USB_HOST
  return; // the port stays the chip's USB serial and JTAG
#else
  prefs.begin("fwbt", false);
  uint8_t role = prefs.getUChar("usbrole", 0); // 0 host, 1 device
  const uint8_t tries = prefs.getUChar("usbtry", 0); // starts of the host that never ran for 5 seconds
  const esp_reset_reason_t why = esp_reset_reason();
  const bool crashed = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT;
  if (role == 0 && tries && (crashed || tries >= 3)) {
    role = 1;
    prefs.putUChar("usbrole", 1);
    prefs.putUChar("usbtry", 0);
    usbFailsafe = true;
  }
  if (role == 0) {
    prefs.putUChar("usbtry", (uint8_t)(tries + 1));
    usbHostActive = usbmidi::begin(usbRxSink);
    if (usbHostActive) usbTryClearMs = millis() + 5000;
    else { prefs.putUChar("usbtry", 0); prefs.putUChar("usbrole", 1); usbFailsafe = true; }
  }
#endif
}

// ============================================================================================
// Button
// ============================================================================================
void startLearning() {
  if (allowedHosts == 0) return; // no room, the main brain says so
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
  NimBLEDevice::deleteAllBonds();
  clearEventsLeft = 3; // sent with the next heartbeats, so a busy link does not lose it
  statusDirty = true;
}

void shortPress(uint32_t now, uint32_t &lastShortMs) {
  stopLearning();
#if FW_BUTTON_STYLE == FW_BUTTON_CME
  if (lastShortMs && now - lastShortMs < DOUBLE_PRESS_MS) { // the second press of a double press
    lastShortMs = 0;
    startLearning();
  } else {
    lastShortMs = now;
  }
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
  } else if (pressed && down) {
    buttonHeldMs = now - downMs;
  } else if (!pressed && down && now - edgeMs > 25) {
    down = false;
    edgeMs = now;
    const uint32_t held = now - downMs;
    buttonHeldMs = 0;
    if (held >= HOLD_FORGET_MS) forgetEverything();
    else if (held >= HOLD_MODE_MS) {
#if FW_BUTTON_STYLE == FW_BUTTON_CME
      forcePeripheral();
#else
      startLearning();
#endif
      bannerPrinted = false;
    } else if (held >= 30) {
      shortPress(now, lastShortMs);
    }
  }
  if (lastShortMs && now - lastShortMs > DOUBLE_PRESS_MS) lastShortMs = 0;
}

// ============================================================================================
// From the main brain
// ============================================================================================
void handleControl(const uint8_t pkt[4]) {
  const uint8_t type = pkt[0] & 0x0F;
  if (type == CONTROL_FLOW) {
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
        saved.putUChar("usbtry", 0);
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
    Peer *p = peerForPort(port);
    if (first && isRealtimeWord(word)) {
      const uint8_t pkt[4] = {0x0F, (uint8_t)(word >> 16), 0, 0};
      if (p) queueToPeer(*p, pkt);
      continue;
    }
    uint32_t message[4];
    const uint8_t words = linkMessage.feed(word, first, message);
    if (!words || !p || !p->connected) continue;
    uint8_t packets[8][4];
    const uint8_t made = p->down.push(message, words, packets);
    for (uint8_t k = 0; k < made; ++k) queueToPeer(*p, packets[k]);
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
  for (uint8_t i = 0; i < 1 + MODULE_HANDLES; ++i) {
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
        Serial.printf("%s %u:", i == 0 ? "device" : "host", (unsigned)i);
        for (uint8_t k = 1; k < 4 && k <= fw::blemidi::kCinBytes[pkt[0] & 0x0F]; ++k) Serial.printf(" %02X", pkt[k]);
        Serial.println();
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
  for (uint8_t i = 0; i < 1 + MODULE_HANDLES; ++i) {
    Peer &p = peers[i];
    if (!p.connected) continue;
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
  for (uint8_t i = 0; i < 1 + MODULE_HANDLES; ++i) {
    Peer &p = peers[i];
    if (p.connected && !p.announced) {
      p.announced = true;
      flashUntilMs = millis() + CONNECT_FLASH_MS;
      if (peers[0].connected && i == 0) peripheralUntilMs = 0; // the phone or computer got in, normal service again
      if (i > 0 && toMainRt.freeSlots(false) > 24) sendIdentity((uint8_t)i);
      statusDirty = true;
#if FW_BENCH
      Serial.printf("connected %s %u: %s\n", i == 0 ? "device" : "host", (unsigned)i, i == 0 ? "central" : p.name);
#endif
    } else if (!p.connected && p.announced) {
      p.announced = false;
      if (i > 0) { p.client = nullptr; p.chr = nullptr; p.usb = false; }
      p.rx.clear();
      p.outRt.clear();
      p.outNormal.clear();
      p.parser.reset();
      p.up.reset();
      p.down.reset();
      statusDirty = true;
#if FW_BENCH
      Serial.printf("disconnected %s %u\n", i == 0 ? "device" : "host", (unsigned)i);
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
    const uint8_t flags = (uint8_t)((peers[0].connected ? 1 : 0) | (learning() ? 2 : 0) | (NimBLEDevice::getScan()->isScanning() ? 4 : 0) | (peripheralWindow() ? 8 : 0) | (usbHostActive ? 16 : 0) | (usbFailsafe ? 32 : 0));
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
  Serial.begin(115200);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  neopixelWrite(PIN_STATUS_RGB, 0, 0, 0);
  boardCheckResult = boardCheckRun(Serial, MODULE_NAME, BT_FW_VERSION);

  linkSerial.setRxBufferSize(4096);
  linkSerial.setTxBufferSize(1024);
  linkSerial.begin(LINK_BAUD, SERIAL_8N1, PIN_LINK_RX, PIN_LINK_TX);

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
  startUsbRole(); // after Bluetooth is up, a USB host that fails then costs nothing else
  ledFadeMs = millis();
}

void loop() {
  if (usbTryClearMs && (int32_t)(millis() - usbTryClearMs) >= 0) { usbTryClearMs = 0; prefs.putUChar("usbtry", 0); } // the host has run for 5 seconds
  if (restartAtMs && (int32_t)(millis() - restartAtMs) >= 0) ESP.restart();
  if (!bannerPrinted && !usbHostActive && millis() > 3000) { // after a serial monitor has had time to open
    bannerPrinted = true;
    boardCheckRun(Serial, MODULE_NAME, BT_FW_VERSION);
    Serial.printf("  allowed hosts %u, learning %s, button style %s\n", allowedHosts, learning() ? "on" : "off", FW_BUTTON_STYLE == FW_BUTTON_CME ? "CME" : "one mode");
  }
  serviceButton();
  pumpLinkIn();
  serviceUsb();
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
