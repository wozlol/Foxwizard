#pragma once

// The slots Bluetooth devices share with the USB Host ports, and the file that remembers who had which. Header only and free of
// Arduino, so tests/ble_slots_test.cpp checks the rules on a computer.
//
// Host slots 1-7 are the USB Host ports. Slot 8 is a route's spare port bit (15), only ever a Bluetooth device. A Bluetooth device
// may take slot 8 first, then USB Host slots 5, 6 and 7, which sit empty unless a hub is chained. Slots 1-4 are the hub's own
// jacks and never go to a Bluetooth device.

#include <stdint.h>
#include <string.h>

namespace fw {
namespace slots {

constexpr uint8_t kSlots = 8;
constexpr uint8_t kNameChars = 24;
constexpr uint8_t kBleOrder[4] = {8, 5, 6, 7};
constexpr uint8_t kBleCount = 4;

enum Kind : uint8_t { kNone = 0, kUsb = 1, kBle = 2 };

struct Record {
  uint8_t kind = kNone;
  uint8_t addr[6] = {0, 0, 0, 0, 0, 0};
  char name[kNameChars + 1] = {0};
  uint32_t lastSeen = 0; // when it last connected, in connection events, there is no clock
};

inline bool isBleSlot(uint8_t slot) {
  for (uint8_t i = 0; i < kBleCount; ++i) if (kBleOrder[i] == slot) return true;
  return false;
}

inline uint8_t slotBit(uint8_t slot) { return (uint8_t)(1u << (slot - 1)); }

class Table {
 public:
  static constexpr uint16_t kFileBytes = 4 + 1 + 4 + kSlots * (1 + 6 + kNameChars + 4) + 2;

  Record rec[kSlots + 1]; // 1-8, entry 0 unused
  uint32_t counter = 0;
  uint8_t bleConnected = 0; // slot n is bit n-1, not saved
  bool dirty = false;       // something worth saving changed

  void clear() {
    for (Record &r : rec) r = Record();
    counter = 0;
    bleConnected = 0;
    dirty = true;
  }

  // A USB device now in slot (1-7), or its name arrived. A change of device or name counts as a new connection.
  void usbSeen(uint8_t slot, const char *name, bool newConnection) {
    if (slot < 1 || slot > 7 || (bleConnected & slotBit(slot))) return;
    Record &r = rec[slot];
    const bool sameName = r.kind == kUsb && strncmp(r.name, name, kNameChars) == 0;
    if (sameName && !newConnection) return;
    r.kind = kUsb;
    memset(r.addr, 0, sizeof(r.addr));
    memset(r.name, 0, sizeof(r.name));
    strncpy(r.name, name, kNameChars);
    r.lastSeen = ++counter;
    dirty = true;
  }

  // The slot a Bluetooth device at addr should take, or 0 for none. usbBusy has slot n at bit n-1 for every slot a USB device
  // is in (or is waiting for). A device that comes back gets its own slot if it is still free, else a new device takes a slot
  // nothing has used, else the one whose device has been gone longest. A connected device's slot is never taken.
  uint8_t choose(const uint8_t addr[6], uint8_t usbBusy) const {
    for (uint8_t i = 0; i < kBleCount; ++i) {
      const uint8_t s = kBleOrder[i];
      if (rec[s].kind == kBle && memcmp(rec[s].addr, addr, 6) == 0 && available(s, usbBusy)) return s;
    }
    uint8_t best = 0;
    uint32_t bestAge = 0;
    for (uint8_t i = 0; i < kBleCount; ++i) {
      const uint8_t s = kBleOrder[i];
      if (!available(s, usbBusy)) continue;
      const uint32_t age = rec[s].kind == kNone ? 0 : rec[s].lastSeen;
      if (best == 0 || age < bestAge) { best = s; bestAge = age; }
    }
    return best;
  }

  // How many more Bluetooth devices could connect now, counting the slots a returning device would take back.
  uint8_t freeCount(uint8_t usbBusy) const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < kBleCount; ++i) if (available(kBleOrder[i], usbBusy)) ++n;
    return n;
  }

  void bleAssign(uint8_t slot, const uint8_t addr[6], const char *name) {
    if (!isBleSlot(slot)) return;
    for (uint8_t i = 0; i < kBleCount; ++i) { // a device lives in one slot only
      const uint8_t s = kBleOrder[i];
      if (s != slot && rec[s].kind == kBle && memcmp(rec[s].addr, addr, 6) == 0 && !(bleConnected & slotBit(s))) rec[s] = Record();
    }
    Record &r = rec[slot];
    r.kind = kBle;
    memcpy(r.addr, addr, 6);
    memset(r.name, 0, sizeof(r.name));
    strncpy(r.name, name, kNameChars);
    r.lastSeen = ++counter;
    bleConnected |= slotBit(slot);
    dirty = true;
  }

  void bleName(uint8_t slot, const char *name) {
    if (!isBleSlot(slot) || rec[slot].kind != kBle || strncmp(rec[slot].name, name, kNameChars) == 0) return;
    memset(rec[slot].name, 0, sizeof(rec[slot].name));
    strncpy(rec[slot].name, name, kNameChars);
    dirty = true;
  }

  void bleLeft(uint8_t slot) { if (slot >= 1 && slot <= kSlots) bleConnected &= (uint8_t)~slotBit(slot); }

  void forget(uint8_t slot) {
    if (slot < 1 || slot > kSlots) return;
    rec[slot] = Record();
    bleConnected &= (uint8_t)~slotBit(slot);
    dirty = true;
  }

  // The file: "FWSL", version 1, the counter, 8 records, a 16 bit sum of everything before it.
  uint16_t save(uint8_t *out) const {
    uint8_t *p = out;
    memcpy(p, "FWSL", 4); p += 4;
    *p++ = 1;
    for (uint8_t i = 0; i < 4; ++i) *p++ = (uint8_t)(counter >> (8 * i));
    for (uint8_t s = 1; s <= kSlots; ++s) {
      const Record &r = rec[s];
      *p++ = r.kind;
      memcpy(p, r.addr, 6); p += 6;
      memcpy(p, r.name, kNameChars); p += kNameChars;
      for (uint8_t i = 0; i < 4; ++i) *p++ = (uint8_t)(r.lastSeen >> (8 * i));
    }
    uint16_t sum = 0;
    for (const uint8_t *q = out; q < p; ++q) sum = (uint16_t)(sum + *q);
    *p++ = (uint8_t)sum;
    *p++ = (uint8_t)(sum >> 8);
    return (uint16_t)(p - out);
  }

  bool load(const uint8_t *in, uint16_t len) {
    if (len != kFileBytes || memcmp(in, "FWSL", 4) != 0 || in[4] != 1) return false;
    uint16_t sum = 0;
    for (uint16_t i = 0; i < kFileBytes - 2; ++i) sum = (uint16_t)(sum + in[i]);
    if (in[kFileBytes - 2] != (uint8_t)sum || in[kFileBytes - 1] != (uint8_t)(sum >> 8)) return false;
    const uint8_t *p = in + 5;
    uint32_t c = 0;
    for (uint8_t i = 0; i < 4; ++i) c |= (uint32_t)*p++ << (8 * i);
    Record loaded[kSlots + 1];
    for (uint8_t s = 1; s <= kSlots; ++s) {
      Record &r = loaded[s];
      r.kind = *p++;
      memcpy(r.addr, p, 6); p += 6;
      memcpy(r.name, p, kNameChars); p += kNameChars;
      r.name[kNameChars] = 0;
      for (uint8_t i = 0; i < kNameChars && r.name[i]; ++i) if (r.name[i] < 0x20 || r.name[i] > 0x7E) r.name[i] = '?';
      for (uint8_t i = 0; i < 4; ++i) r.lastSeen |= (uint32_t)*p++ << (8 * i);
      if (r.kind > kBle || (r.kind == kBle && !isBleSlot(s)) || (r.kind == kUsb && s > 7)) r = Record();
      if (r.lastSeen > c) c = r.lastSeen;
    }
    for (uint8_t s = 1; s <= kSlots; ++s) rec[s] = loaded[s];
    counter = c;
    bleConnected = 0;
    dirty = false;
    return true;
  }

 private:
  // A slot a Bluetooth device could take right now.
  bool available(uint8_t slot, uint8_t usbBusy) const {
    if (bleConnected & slotBit(slot)) return false;
    if (slot != 8 && (usbBusy & slotBit(slot))) return false;
    return true;
  }
};

}  // namespace slots
}  // namespace fw
