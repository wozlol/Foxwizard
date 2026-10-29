// Checks for ../ump.h, run on a computer, not the device:
//   clang++ -std=c++17 -I .. ump_test.cpp -o ump_test && ./ump_test
// Covers message lengths, the scaling, USB MIDI 1.0 packets to UMP and back (channel voice, system, real time, SysEx of
// every length on several groups), and the MIDI 1.0 and MIDI 2.0 Protocol translation.
#include "ump.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace fw::ump;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; if (fails < 12) { printf("  FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
typedef std::vector<std::vector<uint8_t>> Packets;
// reference: canonical USB MIDI 1.0 packets for a SysEx byte list (F0 ... F7)
static Packets sysexPackets(uint8_t cable, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> s; s.push_back(0xF0); s.insert(s.end(), data.begin(), data.end()); s.push_back(0xF7);
  Packets p; size_t at = 0;
  while (s.size() - at > 3) { p.push_back({(uint8_t)((cable << 4) | 4), s[at], s[at + 1], s[at + 2]}); at += 3; }
  const size_t rest = s.size() - at;
  p.push_back({(uint8_t)((cable << 4) | (4 + rest)), s[at], (uint8_t)(rest > 1 ? s[at + 1] : 0), (uint8_t)(rest > 2 ? s[at + 2] : 0)});
  return p;
}
static Packets roundTrip(const Packets &in, UsbMidiToUmp &up, UmpToUsbMidi &down) {
  Packets out;
  for (const auto &p : in) {
    uint32_t w[4]; const uint8_t n = up.push(p.data(), w);
    for (uint8_t i = 0; i < n;) { const uint8_t len = wordCount(w[i]); uint8_t pk[8][4]; const uint8_t m = down.push(&w[i], len, pk); for (uint8_t k = 0; k < m; ++k) out.push_back({pk[k][0], pk[k][1], pk[k][2], pk[k][3]}); i += len; }
  }
  return out;
}
int main() {
  printf("1. word counts by message type\n");
  const uint8_t want[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
  for (uint32_t t = 0; t < 16; ++t) CHECK(wordCount(t << 28) == want[t], "type %u", t);
  printf("2. scaling\n");
  for (uint32_t v = 0; v < 128; ++v) { CHECK(scaleDown(scaleUp(v, 7, 16), 16, 7) == v, "7->16->7 %u", v); CHECK(scaleDown(scaleUp(v, 7, 32), 32, 7) == v, "7->32->7 %u", v); }
  for (uint32_t v = 0; v < 16384; ++v) CHECK(scaleDown(scaleUp(v, 14, 32), 32, 14) == v, "14->32->14 %u", v);
  CHECK(scaleUp(127, 7, 16) == 0xFFFF && scaleUp(64, 7, 16) == 0x8000 && scaleUp(0, 7, 16) == 0, "7 bit ends and centre");
  CHECK(scaleUp(16383, 14, 32) == 0xFFFFFFFFu && scaleUp(8192, 14, 32) == 0x80000000u, "14 bit ends and centre");
  printf("3. channel voice, system and real time packets, every group\n");
  srand(3); Packets in;
  for (int i = 0; i < 4000; ++i) {
    const uint8_t g = rand() & 15; const int k = rand() % 6;
    if (k == 0) in.push_back({(uint8_t)((g << 4) | 0x9), (uint8_t)(0x90 | (rand() & 15)), (uint8_t)(rand() & 127), (uint8_t)(rand() & 127)});
    else if (k == 1) in.push_back({(uint8_t)((g << 4) | 0xB), (uint8_t)(0xB0 | (rand() & 15)), (uint8_t)(rand() & 127), (uint8_t)(rand() & 127)});
    else if (k == 2) in.push_back({(uint8_t)((g << 4) | 0xC), (uint8_t)(0xC0 | (rand() & 15)), (uint8_t)(rand() & 127), 0});
    else if (k == 3) in.push_back({(uint8_t)((g << 4) | 0xE), (uint8_t)(0xE0 | (rand() & 15)), (uint8_t)(rand() & 127), (uint8_t)(rand() & 127)});
    else if (k == 4) in.push_back({(uint8_t)((g << 4) | 0xF), (uint8_t)(0xF8 + rand() % 8), 0, 0});
    else { const uint8_t s = 0xF1 + rand() % 3; if (s == 0xF2) in.push_back({(uint8_t)((g << 4) | 3), 0xF2, (uint8_t)(rand() & 127), (uint8_t)(rand() & 127)}); else in.push_back({(uint8_t)((g << 4) | 2), s, (uint8_t)(rand() & 127), 0}); }
  }
  in.push_back({0x05, 0xF6, 0, 0});
  UsbMidiToUmp up; UmpToUsbMidi down; up.reset(); down.reset();
  Packets out = roundTrip(in, up, down); CHECK(out == in, "%zu in, %zu out, or they differ", in.size(), out.size());
  printf("4. SysEx of every length 0 to 40 bytes, on several groups\n");
  for (size_t len = 0; len <= 40; ++len) for (uint8_t g : {0, 5, 15}) {
    std::vector<uint8_t> d; for (size_t i = 0; i < len; ++i) d.push_back(1 + rand() % 126);
    Packets p = sysexPackets(g, d); up.reset(); down.reset(); Packets o = roundTrip(p, up, down);
    CHECK(o == p, "sysex %zu bytes group %u: %zu packets in, %zu out", len, g, p.size(), o.size());
  }
  printf("5. SysEx on two groups at once, and real time bytes between the packets\n");
  { std::vector<uint8_t> a, b; for (int i = 0; i < 30; ++i) { a.push_back(10 + i); b.push_back(60 + i); }
    Packets pa = sysexPackets(1, a), pb = sysexPackets(2, b), mixed;
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) { if (i < pa.size()) mixed.push_back(pa[i]); mixed.push_back({0x0F, 0xF8, 0, 0}); if (i < pb.size()) mixed.push_back(pb[i]); }
    up.reset(); down.reset(); Packets o = roundTrip(mixed, up, down);
    Packets oa, ob, orr, ra, rb, rr; for (auto &p : o) { if ((p[0] >> 4) == 1) oa.push_back(p); else if ((p[0] >> 4) == 2) ob.push_back(p); else orr.push_back(p); }
    for (auto &p : mixed) { if ((p[0] >> 4) == 1) ra.push_back(p); else if ((p[0] >> 4) == 2) rb.push_back(p); else rr.push_back(p); }
    CHECK(oa == ra && ob == rb && orr == rr, "interleaved streams differ"); }
  printf("6. a SysEx that starts again before it ends drops the first, and stray data bytes are ignored\n");
  { up.reset(); uint32_t w[4]; uint8_t n; n = up.push((const uint8_t[]){0x04, 0xF0, 1, 2}, w); CHECK(n == 0, "start gives nothing yet");
    n = up.push((const uint8_t[]){0x04, 0xF0, 7, 8}, w); CHECK(n == 0, "restart"); n = up.push((const uint8_t[]){0x06, 9, 0xF7, 0}, w);
    CHECK(n == 2 && ((w[0] >> 20) & 15) == 0 && ((w[0] >> 16) & 15) == 3 && ((w[0] >> 8) & 255) == 7 && (w[0] & 255) == 8 && (w[1] >> 24) == 9, "second message complete with 7 8 9");
    n = up.push((const uint8_t[]){0x06, 5, 0xF7, 0}, w); CHECK(n == 0, "end with no start"); }
  printf("7. MIDI 1.0 to MIDI 2.0 and back\n");
  for (uint32_t ch = 0; ch < 16; ++ch) for (uint32_t d1 = 0; d1 < 128; d1 += 3) for (uint32_t d2 = 0; d2 < 128; ++d2) {
    for (uint8_t op : {0x80, 0xA0, 0xB0}) { uint32_t w1 = makeMidi1(ch & 3, op | ch, d1, d2), m2[2], back[6]; CHECK(midi1ToMidi2(w1, m2) == 2, "up"); CHECK(UmpToUsbMidi::midi2ToMidi1(m2, back) == 1 && back[0] == w1, "op %02X %u %u -> %08X", op, d1, d2, back[0]); }
    if (d2) { uint32_t w1 = makeMidi1(0, 0x90 | ch, d1, d2), m2[2], back[6]; midi1ToMidi2(w1, m2); CHECK(UmpToUsbMidi::midi2ToMidi1(m2, back) == 1 && back[0] == w1, "note on %u %u", d1, d2); }
  }
  for (uint32_t pb = 0; pb < 16384; ++pb) { uint32_t w1 = makeMidi1(3, 0xE5, pb & 127, pb >> 7), m2[2], back[6]; midi1ToMidi2(w1, m2); CHECK(UmpToUsbMidi::midi2ToMidi1(m2, back) == 1 && back[0] == w1, "pitch bend %u", pb); }
  for (uint32_t v = 0; v < 128; ++v) { uint32_t w1 = makeMidi1(0, 0xD2, v, 0), m2[2], back[6]; midi1ToMidi2(w1, m2); CHECK(UmpToUsbMidi::midi2ToMidi1(m2, back) == 1 && back[0] == w1, "chan pressure %u", v); w1 = makeMidi1(0, 0xC2, v, 0); midi1ToMidi2(w1, m2); CHECK(UmpToUsbMidi::midi2ToMidi1(m2, back) == 1 && back[0] == w1, "program %u", v); }
  { uint32_t w1 = makeMidi1(0, 0x93, 60, 0), m2[2], back[6]; midi1ToMidi2(w1, m2); CHECK((m2[0] >> 20 & 15) == 8 && (m2[1] >> 16) == 0x8000, "Note On velocity 0 is a Note Off at 64"); CHECK(UmpToUsbMidi::midi2ToMidi1(m2, back) == 1 && back[0] == makeMidi1(0, 0x83, 60, 64), "and comes back as a Note Off at 64"); }
  printf("8. MIDI 2.0 messages with no exact MIDI 1.0 form\n");
  { uint32_t m2[2] = {(4u << 28) | (0x93u << 16) | (60u << 8), 0}, back[6]; CHECK(UmpToUsbMidi::midi2ToMidi1(m2, back) == 1 && (back[0] & 255) == 1, "a MIDI 2.0 Note On of velocity 0 is velocity 1 in MIDI 1.0");
    uint32_t pc[2] = {(4u << 28) | (0xC2u << 16) | 1u, (17u << 24) | (3u << 8) | 9u}; uint8_t n = UmpToUsbMidi::midi2ToMidi1(pc, back);
    CHECK(n == 3 && back[0] == makeMidi1(0, 0xB2, 0, 3) && back[1] == makeMidi1(0, 0xB2, 32, 9) && back[2] == makeMidi1(0, 0xC2, 17, 0), "Program Change with a bank");
    uint32_t rpn[2] = {(4u << 28) | (0x21u << 16) | (0u << 8) | 2u, 0x80000000u}; n = UmpToUsbMidi::midi2ToMidi1(rpn, back);
    CHECK(n == 4 && back[0] == makeMidi1(0, 0xB1, 101, 0) && back[1] == makeMidi1(0, 0xB1, 100, 2) && back[2] == makeMidi1(0, 0xB1, 6, 64) && back[3] == makeMidi1(0, 0xB1, 38, 0), "RPN is four controllers, centre value 64 0");
    uint32_t pn[2] = {(4u << 28) | (0x60u << 16) | (60u << 8), 0}; CHECK(UmpToUsbMidi::midi2ToMidi1(pn, back) == 0, "per note pitch bend has no MIDI 1.0 form"); }
  printf("%s\n", fails ? "SOME CHECKS FAILED" : "all checks pass");
  return fails ? 1 : 0;
}
