// Tests of src/usb_coalesce.h on a computer.
//   clang++ -std=c++17 -I ../src usb_coalesce_test.cpp -o /tmp/usb_coalesce_test && /tmp/usb_coalesce_test

#include <cstdio>
#include <vector>

#include "usb_coalesce.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void pkt(uint8_t out[4], uint8_t status, uint8_t d1 = 0, uint8_t d2 = 0) { out[0] = status >> 4; out[1] = status; out[2] = d1; out[3] = d2; }

static void testClassification() {
  uint8_t p[4];
  for (uint8_t ch = 0; ch < 16; ++ch) {
    pkt(p, 0xA0 | ch, 60, 50); CHECK(fw::usbPacketIsContinuous(p), "poly pressure waits");
    pkt(p, 0xD0 | ch, 50); CHECK(fw::usbPacketIsContinuous(p), "channel pressure waits");
    pkt(p, 0xE0 | ch, 0, 64); CHECK(fw::usbPacketIsContinuous(p), "pitch bend waits");
    pkt(p, 0x90 | ch, 60, 100); CHECK(!fw::usbPacketIsContinuous(p), "a note on does not wait");
    pkt(p, 0x80 | ch, 60, 0); CHECK(!fw::usbPacketIsContinuous(p), "a note off does not wait");
    pkt(p, 0xC0 | ch, 5); CHECK(!fw::usbPacketIsContinuous(p), "program change does not wait");
    for (int cc = 0; cc < 128; ++cc) {
      pkt(p, 0xB0 | ch, (uint8_t)cc, 10);
      const bool critical = cc == 0 || cc == 32 || (cc >= 64 && cc <= 69) || cc >= 120;
      CHECK(fw::usbPacketIsContinuous(p) == !critical, "cc %d: waits %d", cc, (int)fw::usbPacketIsContinuous(p));
    }
  }
  pkt(p, 0xF8); CHECK(!fw::usbPacketIsContinuous(p), "clock does not wait");
  pkt(p, 0xF0, 1, 2); CHECK(!fw::usbPacketIsContinuous(p), "sysex does not wait");
}

// The queue as the firmware keeps it: packets with their arrival time, the coalescer deciding when they go. Returns every flush.
struct Flush { uint32_t atUs; size_t packets; };
struct Sim {
  fw::UsbCoalescer c;
  std::vector<uint32_t> queue; // arrival times
  std::vector<Flush> flushes;
  std::vector<uint32_t> latencies;
  bool realtime = false;
  void queued(bool continuous, uint32_t now) { queue.push_back(now); c.packetQueued(continuous, now); pump(now); }
  void pump(uint32_t now) {
    if (queue.empty()) { c.queueEmptied(); return; }
    if (!c.mayWrite(now, realtime)) return;
    flushes.push_back({now, queue.size()});
    for (uint32_t t : queue) latencies.push_back(now - t);
    queue.clear();
    realtime = false;
    c.queueEmptied();
  }
};

static void testTiming() {
  // one controller packet alone goes at its deadline, not before
  { Sim s; s.queued(true, 1000); s.pump(2999); CHECK(s.flushes.empty(), "held before 2 ms"); s.pump(3001); CHECK(s.flushes.size() == 1 && s.flushes[0].packets == 1, "goes at 2 ms"); }
  // a stream of controller packets, one every 100 us: the wait is never longer than 2 ms for any packet, and they go together
  {
    Sim s;
    for (uint32_t t = 0; t < 200000; t += 100) { s.queued(true, t); s.pump(t + 50); }
    for (uint32_t t = 200000; t < 210000 && !s.queue.empty(); t += 50) s.pump(t); // the main loop keeps pumping
    uint32_t worst = 0; for (uint32_t l : s.latencies) worst = l > worst ? l : worst;
    CHECK(worst <= fw::kUsbCoalesceUs + 100, "worst wait %u us", worst);
    CHECK(s.latencies.size() == 2000, "every packet went, %zu", s.latencies.size());
    CHECK(s.flushes.size() <= 200000 / 2000 + 2, "about one transfer per 2 ms, got %zu", s.flushes.size());
    size_t biggest = 0; for (const Flush &f : s.flushes) biggest = f.packets > biggest ? f.packets : biggest;
    std::printf("a stream of 10,000 packets a second: %zu transfers for 2,000 packets, up to %zu in one\n", s.flushes.size(), biggest);
  }
  // a note sends everything waiting at once, with no wait of its own
  { Sim s; s.queued(true, 1000); s.queued(true, 1200); s.queued(false, 1500); CHECK(s.flushes.size() == 1 && s.flushes[0].atUs == 1500 && s.flushes[0].packets == 3, "a note flushes the controller packets before it"); }
  // after a flush the next controller packet starts a fresh wait
  { Sim s; s.queued(false, 100); CHECK(s.flushes.size() == 1, "a note alone goes"); s.queued(true, 200); CHECK(s.flushes.size() == 1, "then a controller waits"); s.pump(2201); CHECK(s.flushes.size() == 2, "and goes 2 ms after its own arrival"); }
  // real time bypasses the wait and takes the waiting packets with it
  { Sim s; s.queued(true, 1000); s.realtime = true; s.pump(1100); CHECK(s.flushes.size() == 1 && s.flushes[0].packets == 1, "real time flushes"); }
  // the clock wrapping round does not hold a packet for 71 minutes
  { Sim s; const uint32_t near = 0xFFFFFF00u; s.queued(true, near); s.pump(near + 100); CHECK(s.flushes.empty(), "held across the wrap"); s.pump(near + 2500); CHECK(s.flushes.size() == 1, "goes after the wrap"); }
  // a deadline of exactly zero is never produced
  { Sim s; s.queued(true, 0xFFFFFFFFu - 1999u); CHECK(s.c.holdUntilUs != 0, "deadline never reads as no wait"); }
}

int main() {
  testClassification();
  testTiming();
  if (failures) { std::printf("%d FAILURES\n", failures); return 1; }
  std::printf("all checks pass\n");
  return 0;
}
