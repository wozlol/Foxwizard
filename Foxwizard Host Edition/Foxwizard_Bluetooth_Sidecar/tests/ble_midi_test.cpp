// Bluetooth MIDI to and from USB MIDI packets.
//   clang++ -std=c++17 -I .. ble_midi_test.cpp -o /tmp/ble_midi_test && /tmp/ble_midi_test

#include <cstdio>
#include <cstring>
#include <vector>

#include "ble_midi.h"

using namespace fw::blemidi;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

typedef std::vector<std::vector<uint8_t>> Packets;

static Packets parse(Parser &p, std::initializer_list<uint8_t> bytes) {
  std::vector<uint8_t> v(bytes);
  Packets out;
  p.feed(v.data(), v.size(), [&](const uint8_t pkt[4]) { out.emplace_back(pkt, pkt + 4); });
  return out;
}

static bool is(const std::vector<uint8_t> &p, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return p.size() == 4 && p[0] == a && p[1] == b && p[2] == c && p[3] == d;
}

int main() {
  // A note on and a note off in one packet.
  {
    Parser p;
    Packets r = parse(p, {0x81, 0xA1, 0x90, 0x3C, 0x64, 0xA2, 0x80, 0x3C, 0x00});
    CHECK(r.size() == 2, "two messages, got %zu", r.size());
    if (r.size() == 2) {
      CHECK(is(r[0], 0x09, 0x90, 0x3C, 0x64), "note on");
      CHECK(is(r[1], 0x08, 0x80, 0x3C, 0x00), "note off");
    }
  }
  // Running status: the second note has no status byte.
  {
    Parser p;
    Packets r = parse(p, {0x80, 0x81, 0x90, 0x3C, 0x64, 0x82, 0x3E, 0x64});
    CHECK(r.size() == 2 && is(r[1], 0x09, 0x90, 0x3E, 0x64), "running status note on");
  }
  // Program change has one data byte, pressure too.
  {
    Parser p;
    Packets r = parse(p, {0x80, 0x81, 0xC2, 0x05, 0x82, 0xD3, 0x40});
    CHECK(r.size() == 2, "two messages");
    if (r.size() == 2) {
      CHECK(is(r[0], 0x0C, 0xC2, 0x05, 0x00), "program change");
      CHECK(is(r[1], 0x0D, 0xD3, 0x40, 0x00), "channel pressure");
    }
  }
  // Real time inside a note, and running status survives it.
  {
    Parser p;
    Packets r = parse(p, {0x80, 0x81, 0x90, 0x3C, 0x82, 0xF8, 0x83, 0x64, 0x84, 0x3E, 0x64});
    CHECK(r.size() == 3, "clock plus two notes, got %zu", r.size());
    if (r.size() == 3) {
      CHECK(is(r[0], 0x0F, 0xF8, 0x00, 0x00), "clock first");
      CHECK(is(r[1], 0x09, 0x90, 0x3C, 0x64), "the interrupted note on");
      CHECK(is(r[2], 0x09, 0x90, 0x3E, 0x64), "running status after it");
    }
  }
  // Pitch bend and control change.
  {
    Parser p;
    Packets r = parse(p, {0x80, 0x81, 0xE0, 0x00, 0x40, 0x82, 0xB0, 0x07, 0x7F});
    CHECK(r.size() == 2 && is(r[0], 0x0E, 0xE0, 0x00, 0x40) && is(r[1], 0x0B, 0xB0, 0x07, 0x7F), "bend and cc");
  }
  // System common: song select, tune request, then channel running status is gone.
  {
    Parser p;
    Packets r = parse(p, {0x80, 0x81, 0x90, 0x3C, 0x64, 0x82, 0xF3, 0x05, 0x83, 0xF6, 0x84, 0x3C, 0x64});
    CHECK(r.size() == 3, "note, song select, tune request, nothing from the stray data, got %zu", r.size());
    if (r.size() == 3) {
      CHECK(is(r[1], 0x02, 0xF3, 0x05, 0x00), "song select");
      CHECK(is(r[2], 0x05, 0xF6, 0x00, 0x00), "tune request");
    }
  }
  // A short SysEx in one packet.
  {
    Parser p;
    Packets r = parse(p, {0x80, 0x81, 0xF0, 0x7E, 0x01, 0x82, 0xF7});
    // F0 7E 01 fills a packet (CIN 4), then F7 ends with a one byte packet (CIN 5).
    CHECK(r.size() == 2, "two packets, got %zu", r.size());
    if (r.size() == 2) {
      CHECK(is(r[0], 0x04, 0xF0, 0x7E, 0x01), "sysex start");
      CHECK(is(r[1], 0x05, 0xF7, 0x00, 0x00), "sysex end alone");
    }
  }
  {
    Parser p;
    Packets r = parse(p, {0x80, 0x81, 0xF0, 0x7E, 0x82, 0xF7});
    CHECK(r.size() == 1 && is(r[0], 0x07, 0xF0, 0x7E, 0xF7), "three byte sysex ends in CIN 7");
  }
  {
    Parser p;
    Packets r = parse(p, {0x80, 0x81, 0xF0, 0x82, 0xF7});
    CHECK(r.size() == 1 && is(r[0], 0x06, 0xF0, 0xF7, 0x00), "F0 F7 is CIN 6");
  }
  // A SysEx across two packets, with a real time byte in the second.
  {
    Parser p;
    Packets a = parse(p, {0x80, 0x81, 0xF0, 0x01, 0x02, 0x03, 0x04});
    Packets b = parse(p, {0x80, 0x05, 0x82, 0xF8, 0x06, 0x83, 0xF7});
    CHECK(a.size() == 1 && is(a[0], 0x04, 0xF0, 0x01, 0x02), "first packet makes one USB packet and keeps two bytes waiting");
    // After the first: F0 01 02 | 03 04 pending. Second: 05 completes 03 04 05, then clock, then 06, F7.
    CHECK(b.size() == 3, "second packet gives three, got %zu", b.size());
    if (b.size() == 3) {
      CHECK(is(b[0], 0x04, 0x03, 0x04, 0x05), "continuation completes a packet");
      CHECK(is(b[1], 0x0F, 0xF8, 0x00, 0x00), "clock inside the sysex");
      CHECK(is(b[2], 0x06, 0x06, 0xF7, 0x00), "end with one data byte");
    }
  }
  // A status byte ends a SysEx that never finished.
  {
    Parser p;
    parse(p, {0x80, 0x81, 0xF0, 0x01});
    Packets r = parse(p, {0x80, 0x82, 0x90, 0x3C, 0x64});
    CHECK(r.size() == 1 && is(r[0], 0x09, 0x90, 0x3C, 0x64), "note after an unfinished sysex");
  }
  // Garbage and empty input do nothing.
  {
    Parser p;
    CHECK(parse(p, {}).empty(), "empty");
    CHECK(parse(p, {0x80}).empty(), "header only");
    CHECK(parse(p, {0x80, 0x3C, 0x64}).empty(), "data with no status");
  }

  // Writer: USB packets to a BLE packet, then back through the parser.
  {
    Writer w;
    w.setLimit(40);
    const uint8_t on[4] = {0x09, 0x90, 0x3C, 0x64};
    const uint8_t off[4] = {0x08, 0x80, 0x3C, 0x00};
    const uint8_t clk[4] = {0x0F, 0xF8, 0, 0};
    const uint8_t pc[4] = {0x0C, 0xC0, 0x05, 0};
    CHECK(w.add(on, 0x1234) && w.add(off, 0x1234) && w.add(clk, 0x1235) && w.add(pc, 0x1235), "all fit");
    const uint8_t *b = w.bytes();
    CHECK(b[0] == (0x80 | ((0x1234 >> 7) & 0x3F)), "header carries the top timestamp bits, got 0x%02X", b[0]);
    CHECK(b[1] == (0x80 | (0x1234 & 0x7F)) && b[2] == 0x90, "timestamp then status");
    Parser p;
    Packets r;
    p.feed(w.bytes(), w.size(), [&](const uint8_t pkt[4]) { r.emplace_back(pkt, pkt + 4); });
    CHECK(r.size() == 4, "four messages come back, got %zu", r.size());
    if (r.size() == 4) CHECK(is(r[0], 0x09, 0x90, 0x3C, 0x64) && is(r[1], 0x08, 0x80, 0x3C, 0x00) && is(r[2], 0x0F, 0xF8, 0, 0) && is(r[3], 0x0C, 0xC0, 0x05, 0), "same messages");
  }
  // Writer: a packet that does not fit says so, and nothing is half written.
  {
    Writer w;
    w.setLimit(8);
    const uint8_t on[4] = {0x09, 0x90, 0x3C, 0x64};
    CHECK(w.add(on, 0), "first fits: header, stamp, 3 bytes = 5");
    CHECK(!w.add(on, 0), "second does not fit in 8");
    CHECK(w.size() == 5, "size stayed 5, got %u", w.size());
    w.clear();
    CHECK(w.empty() && w.add(on, 0), "after clear it fits again");
  }
  // Writer: a SysEx through the writer and back, in one BLE packet and across two.
  {
    const uint8_t pk[3][4] = {{0x04, 0xF0, 0x01, 0x02}, {0x04, 0x03, 0x04, 0x05}, {0x06, 0x06, 0xF7, 0x00}};
    for (uint16_t limit : {64, 8}) {
      Writer w;
      w.setLimit(limit);
      Parser p;
      Packets r;
      auto sink = [&](const uint8_t pkt[4]) { r.emplace_back(pkt, pkt + 4); };
      for (const auto &x : pk) {
        if (!w.add(x, 100)) {
          p.feed(w.bytes(), w.size(), sink);
          w.clear();
          CHECK(w.add(x, 100), "fits after a flush");
        }
      }
      p.feed(w.bytes(), w.size(), sink);
      CHECK(r.size() == 3, "limit %u: sysex returns as three packets, got %zu", limit, r.size());
      if (r.size() == 3) CHECK(is(r[0], 0x04, 0xF0, 0x01, 0x02) && is(r[1], 0x04, 0x03, 0x04, 0x05) && is(r[2], 0x06, 0x06, 0xF7, 0x00), "limit %u: same sysex", limit);
    }
  }
  // Writer: a one byte SysEx end, a lone F7.
  {
    Writer w;
    const uint8_t end[4] = {0x05, 0xF7, 0, 0};
    CHECK(w.add(end, 0) && w.size() == 3, "header, stamp, F7");
  }
  if (failures) { std::printf("%d FAILURES\n", failures); return 1; }
  std::printf("all checks pass\n");
  return 0;
}
