// The slots Bluetooth devices share with the USB Host ports.
//   clang++ -std=c++17 -I ../src ble_slots_test.cpp -o /tmp/ble_slots_test && /tmp/ble_slots_test

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "ble_slots.h"

using namespace fw::slots;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const uint8_t A[6] = {1, 1, 1, 1, 1, 1};
static const uint8_t B[6] = {2, 2, 2, 2, 2, 2};
static const uint8_t C[6] = {3, 3, 3, 3, 3, 3};
static const uint8_t D[6] = {4, 4, 4, 4, 4, 4};
static const uint8_t E[6] = {5, 5, 5, 5, 5, 5};

// Connects a device the way the main brain does: choose, then assign.
static uint8_t connect(Table &t, const uint8_t addr[6], const char *name, uint8_t usbBusy = 0) {
  const uint8_t s = t.choose(addr, usbBusy);
  if (s) t.bleAssign(s, addr, name);
  return s;
}

int main() {
  // The first device takes the spare slot 8, then USB Host slots 5, 6 and 7, and a fifth finds none.
  {
    Table t;
    CHECK(connect(t, A, "A") == 8, "first device takes slot 8");
    CHECK(connect(t, B, "B") == 5, "second takes slot 5");
    CHECK(connect(t, C, "C") == 6, "third takes slot 6");
    CHECK(connect(t, D, "D") == 7, "fourth takes slot 7");
    CHECK(connect(t, E, "E") == 0, "a fifth finds the slots full");
    CHECK(t.freeCount(0) == 0, "nothing free when all four are connected");
  }
  // A device that comes back gets its own slot, even when older ones are free.
  {
    Table t;
    connect(t, A, "A"); connect(t, B, "B");
    t.bleLeft(8); t.bleLeft(5);
    CHECK(t.choose(B, 0) == 5, "B gets slot 5 back, not slot 8");
    CHECK(t.choose(A, 0) == 8, "A gets slot 8 back");
  }
  // A new device takes a slot nothing has used before one that belonged to a device that left.
  {
    Table t;
    connect(t, A, "A");
    t.bleLeft(8);
    CHECK(t.choose(B, 0) == 5, "B takes the never used slot 5, A keeps slot 8 for when it returns");
  }
  // With every slot used before, the one whose device has been gone longest is taken.
  {
    Table t;
    connect(t, A, "A"); connect(t, B, "B"); connect(t, C, "C"); connect(t, D, "D");
    t.bleLeft(8); t.bleLeft(5); t.bleLeft(6); t.bleLeft(7);
    t.bleAssign(5, B, "B"); t.bleLeft(5); // B was seen most recently, then C
    t.bleAssign(6, C, "C"); t.bleLeft(6);
    CHECK(t.choose(E, 0) == 8, "A, the one gone longest, loses its slot to E");
    t.bleAssign(8, E, "E");
    CHECK(t.rec[8].kind == kBle && memcmp(t.rec[8].addr, E, 6) == 0, "slot 8 now belongs to E");
    CHECK(t.choose(A, 0) == 7, "A is next oldest-gone on slot 7 (D) and takes it");
  }
  // A connected device's slot is never taken.
  {
    Table t;
    connect(t, A, "A");
    CHECK(t.choose(A, 0) != 8, "a device already connected is not given its own slot again");
    for (const uint8_t *x : {B, C, D}) connect(t, x, "x");
    CHECK(t.choose(E, 0) == 0, "all four in use, E waits");
  }
  // USB Host slots a USB device is in are not available, slot 8 always is.
  {
    Table t;
    CHECK(t.choose(A, 0x10 | 0x20 | 0x40) == 8, "slots 5, 6, 7 busy, slot 8 is free");
    connect(t, A, "A", 0x70);
    CHECK(t.choose(B, 0x70) == 0, "slots 5, 6 and 7 busy and 8 taken: full");
    CHECK(t.freeCount(0x70) == 0, "none free");
    CHECK(t.freeCount(0x20) == 2, "slot 6 busy, 5 and 7 free");
  }
  // Forget frees a slot at once.
  {
    Table t;
    connect(t, A, "A");
    t.forget(8);
    CHECK(t.rec[8].kind == kNone && !(t.bleConnected & 0x80), "forgotten");
    CHECK(t.choose(B, 0) == 8, "slot 8 is free again");
  }
  // A device moves between slots without leaving a copy of itself behind.
  {
    Table t;
    connect(t, A, "A"); connect(t, B, "B");
    t.bleLeft(8);
    t.bleAssign(5, A, "A"); // A returned to 5 because 8 was handed out meanwhile
    CHECK(t.rec[8].kind == kNone, "A's old record in slot 8 is gone");
  }
  // USB names: remembered, never over a connected Bluetooth device, and a change counts as a new connection.
  {
    Table t;
    t.usbSeen(2, "Launchkey", true);
    CHECK(t.rec[2].kind == kUsb && strcmp(t.rec[2].name, "Launchkey") == 0, "slot 2 remembers its name");
    const uint32_t seen = t.rec[2].lastSeen;
    t.dirty = false;
    t.usbSeen(2, "Launchkey", false);
    CHECK(!t.dirty && t.rec[2].lastSeen == seen, "the same name again changes nothing");
    t.usbSeen(2, "Launchkey", true);
    CHECK(t.rec[2].lastSeen > seen, "plugged in again counts as a new connection");
    connect(t, A, "A");
    t.usbSeen(8, "x", true);
    CHECK(t.rec[8].kind == kBle, "no USB name in slot 8");
    t.usbSeen(9, "x", true);
  }
  // The file.
  {
    Table t;
    t.usbSeen(1, "Keys", true);
    connect(t, A, "Alpha"); connect(t, B, "Beta");
    t.bleLeft(8);
    uint8_t file[Table::kFileBytes + 8];
    const uint16_t n = t.save(file);
    CHECK(n == Table::kFileBytes, "file is %u bytes, wanted %u", n, Table::kFileBytes);
    Table u;
    CHECK(u.load(file, n), "loads");
    CHECK(strcmp(u.rec[1].name, "Keys") == 0 && u.rec[1].kind == kUsb, "USB name back");
    CHECK(strcmp(u.rec[8].name, "Alpha") == 0 && memcmp(u.rec[8].addr, A, 6) == 0, "Alpha back in 8");
    CHECK(u.bleConnected == 0 && !u.dirty, "nothing connected after a load");
    CHECK(u.counter == t.counter, "counter back");
    CHECK(u.choose(A, 0) == 8, "Alpha still gets its own slot");
    file[20] ^= 0x55;
    Table v;
    CHECK(!v.load(file, n), "a damaged file is refused");
    CHECK(!v.load(file, n - 1), "a short file is refused");
  }
  // Odd characters in a loaded name become '?', a Bluetooth record in a slot that cannot hold one is dropped.
  {
    Table t;
    connect(t, A, "A");
    Table u;
    uint8_t bad[Table::kFileBytes];
    t.save(bad);
    bad[9 + 7 * 35 + 1 + 6] = 0x07;
    uint16_t sum = 0;
    for (uint16_t i = 0; i < Table::kFileBytes - 2; ++i) sum = (uint16_t)(sum + bad[i]);
    bad[Table::kFileBytes - 2] = (uint8_t)sum;
    bad[Table::kFileBytes - 1] = (uint8_t)(sum >> 8);
    CHECK(u.load(bad, Table::kFileBytes), "loads");
    CHECK(u.rec[8].name[0] == '?', "control character cleaned, got 0x%02X", (unsigned)(uint8_t)u.rec[8].name[0]);
    // A Bluetooth record claimed by slot 3 is dropped.
    uint8_t wrong[Table::kFileBytes];
    t.save(wrong);
    wrong[9 + 2 * 35] = kBle;
    sum = 0;
    for (uint16_t i = 0; i < Table::kFileBytes - 2; ++i) sum = (uint16_t)(sum + wrong[i]);
    wrong[Table::kFileBytes - 2] = (uint8_t)sum;
    wrong[Table::kFileBytes - 1] = (uint8_t)(sum >> 8);
    Table w;
    CHECK(w.load(wrong, Table::kFileBytes), "loads");
    CHECK(w.rec[3].kind == kNone, "a Bluetooth record in slot 3 is dropped");
  }
  if (failures) { std::printf("%d FAILURES\n", failures); return 1; }
  std::printf("all checks pass\n");
  return 0;
}
