// Checks for ../foxwizard_link.h with ../ump.h, run on a computer, not the device:
//   clang++ -std=c++17 -I .. link_test.cpp -o link_test && ./link_test
// Covers the frame format, the byte stream and its recovery, messages put back together (with real time words between
// them), whole-message queue pushes, and the full path from one brain's USB MIDI 1.0 packet to the other's.
#include "foxwizard_link.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace fw::ump;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; if (fails < 12) { printf("  FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
typedef std::vector<std::vector<uint8_t>> Packets;

// the two brains' link output: realtime queue first, then the normal one, as pumpLinkOut does
struct Side {
  MidiQueue<LinkFrame, 64, 0> rt;
  MidiQueue<LinkFrame, 2048, 128> normal;
  UsbMidiToUmp up; UmpToUsbMidi down; LinkMessageReader reader; LinkFrameReader frames;
  void send(uint8_t port, const uint8_t pkt[4]) {
    uint32_t w[4]; const uint8_t n = up.push(pkt, w);
    for (uint8_t i = 0; i < n;) { const uint8_t len = wordCount(w[i]);
      if (isRealtimeWord(w[i])) pushLinkMessage(rt, port, &w[i], len, true); else pushLinkMessage(normal, port, &w[i], len, isCriticalWord(w[i])); i += len; }
  }
  std::vector<uint8_t> drain(bool realtimeEvery = false) { // the bytes on the wire
    std::vector<uint8_t> out; int turn = 0;
    while (!rt.empty() || !normal.empty()) {
      LinkFrame *f = nullptr;
      if (realtimeEvery && (turn++ % 2) && normal.peek()) { f = normal.peek(); } else f = rt.peek() ? rt.peek() : normal.peek();
      for (uint8_t i = 0; i < LINK_FRAME_BYTES; ++i) out.push_back(f->b[i]);
      if (f == rt.peek()) rt.pop(); else normal.pop();
    }
    return out;
  }
};
static Packets receive(Side &rx, const std::vector<uint8_t> &bytes, uint8_t wantPort) {
  Packets got;
  for (uint8_t b : bytes) {
    if (!rx.frames.feed(b)) continue;
    uint32_t word; bool first; const uint8_t port = decodeLinkWord(rx.frames.frame, word, first);
    if (port != wantPort) continue;
    uint32_t msg[4]; const uint8_t n = rx.reader.feed(word, first, msg); if (!n) continue;
    uint8_t pk[8][4]; const uint8_t m = rx.down.push(msg, n, pk); for (uint8_t k = 0; k < m; ++k) got.push_back({pk[k][0], pk[k][1], pk[k][2], pk[k][3]});
  }
  return got;
}
static Packets sysexPackets(uint8_t cable, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> s; s.push_back(0xF0); s.insert(s.end(), data.begin(), data.end()); s.push_back(0xF7); Packets p; size_t at = 0;
  while (s.size() - at > 3) { p.push_back({(uint8_t)((cable << 4) | 4), s[at], s[at + 1], s[at + 2]}); at += 3; }
  const size_t rest = s.size() - at; p.push_back({(uint8_t)((cable << 4) | (4 + rest)), s[at], (uint8_t)(rest > 1 ? s[at + 1] : 0), (uint8_t)(rest > 2 ? s[at + 2] : 0)}); return p;
}
int main() {
  srand(11);
  printf("1. a word in a frame and back, every port, with and without the first flag\n");
  for (int i = 0; i < 20000; ++i) { uint32_t w = ((uint32_t)rand() << 17) ^ ((uint32_t)rand() << 3) ^ rand(); const uint8_t port = rand() & 15; const bool first = rand() & 1;
    LinkFrame f; encodeLinkWord(port, w, first, f); uint32_t w2; bool f2; const uint8_t p2 = decodeLinkWord(f, w2, f2);
    CHECK(w2 == w && f2 == first && p2 == port, "%08X port %u", w, port); CHECK((f.b[0] & 0x80) && !(f.b[1] & 0x80) && !(f.b[2] & 0x80) && !(f.b[3] & 0x80) && !(f.b[4] & 0x80) && !(f.b[5] & 0x80), "only byte 0 has bit 7"); }
  { const uint8_t pkt[4] = {2, 0xFF, 0x80, 0x7F}, back[4] = {0}; uint8_t b2[4]; LinkFrame f; encodeLinkFrame(LINK_PORT_CONTROL, pkt, f); const uint8_t port = decodeLinkFrame(f, b2); (void)back;
    CHECK(port == 15 && b2[0] == 2 && b2[1] == 0xFF && b2[2] == 0x80 && b2[3] == 0x7F, "control bytes keep all 8 bits"); }
  printf("2. frame sizes, and the byte stream recovering from a lost byte\n");
  { Side tx, rx; std::vector<uint32_t> sent;
    for (int i = 0; i < 200; ++i) { uint32_t w = makeMidi1(0, 0x90, i & 127, 100); sent.push_back(w); LinkFrame f; encodeLinkWord(1, w, true, f); for (uint8_t k = 0; k < 6; ++k) { if (i == 50 && k == 3) continue; /* a byte lost */ uint8_t b = f.b[k]; if (rx.frames.feed(b)) { uint32_t ww; bool ff; decodeLinkWord(rx.frames.frame, ww, ff); (void)ww; } } }
    // count frames the reader completes
    int whole = 0; Side r2; for (int i = 0; i < 200; ++i) { LinkFrame f; encodeLinkWord(1, makeMidi1(0, 0x90, i & 127, 100), true, f); for (uint8_t k = 0; k < 6; ++k) { if (i == 50 && k == 3) continue; if (r2.frames.feed(f.b[k])) ++whole; } }
    CHECK(whole == 199, "a lost byte costs only the frame it hit: %d of 200", whole); }
  printf("3. messages of 1 to 4 words, a real time message between their words, a lost first word\n");
  { LinkMessageReader r; uint32_t out[4];
    const uint32_t m4[4] = {0xF0000000u | 1, 2, 3, 4}; // stream type, 4 words
    CHECK(r.feed(m4[0], true, out) == 0 && r.feed(m4[1], false, out) == 0, "waits");
    CHECK(r.feed(makeSystem(0, 0xF8, 0, 0), true, out) == 1 && out[0] == makeSystem(0, 0xF8, 0, 0), "real time between words comes out whole");
    CHECK(r.feed(m4[2], false, out) == 0 && r.feed(m4[3], false, out) == 4 && out[0] == m4[0] && out[3] == 4, "the message goes on after it");
    CHECK(r.feed(5, false, out) == 0, "words of a message whose start was lost are ignored");
    const uint32_t two[2] = {(4u << 28) | (0x91u << 16) | (60u << 8), 0xFFFF0000u};
    r.feed(m4[0], true, out); r.feed(m4[1], false, out); // abandoned
    CHECK(r.feed(two[0], true, out) == 0 && r.feed(two[1], false, out) == 2 && out[1] == two[1], "a new first word drops the unfinished one"); }
  printf("4. a message goes into a queue whole or not at all, and a Note Off still fits when ordinary traffic fills it\n");
  { MidiQueue<LinkFrame, 16, 4> q; const uint32_t w[2] = {(4u << 28) | (0x90u << 16), 0}; int in = 0;
    while (pushLinkMessage(q, 1, w, 2, false)) ++in; CHECK(q.size() == (size_t)in * 2 && q.size() <= 11, "only whole messages: %d in, %u frames", in, q.size());
    const uint32_t off[2] = {(4u << 28) | (0x80u << 16), 0}; CHECK(isCriticalWord(off[0]) && pushLinkMessage(q, 1, off, 2, true), "a MIDI 2.0 Note Off uses the reserve");
    CHECK(isCriticalWord(makeMidi1(0, 0x80, 1, 0)) && isCriticalWord(makeMidi1(0, 0x90, 1, 0)) && !isCriticalWord(makeMidi1(0, 0x90, 1, 9)) && isCriticalWord(makeMidi1(0, 0xB0, 123, 0)) && isCriticalWord(makeMidi1(0, 0xB0, 64, 0)) && !isCriticalWord(makeMidi1(0, 0xB0, 64, 127)), "critical set"); }
  printf("5. whole path: packets in one brain, the other brain's packets out, every kind of message\n");
  for (uint8_t port : {1, 5, 12}) {
    Packets in; for (int i = 0; i < 3000; ++i) { const uint8_t g = rand() & 3; const int k = rand() % 8;
      if (k == 0) in.push_back({(uint8_t)((g << 4) | 9), 0x90, (uint8_t)(rand() & 127), (uint8_t)(1 + rand() % 127)});
      else if (k == 1) in.push_back({(uint8_t)((g << 4) | 8), 0x80, (uint8_t)(rand() & 127), (uint8_t)(rand() & 127)});
      else if (k == 2) in.push_back({(uint8_t)((g << 4) | 0xB), 0xB1, (uint8_t)(rand() & 127), (uint8_t)(rand() & 127)});
      else if (k == 3) in.push_back({(uint8_t)((g << 4) | 0xE), 0xE2, (uint8_t)(rand() & 127), (uint8_t)(rand() & 127)});
      else if (k == 4) in.push_back({(uint8_t)((g << 4) | 0xD), 0xD3, (uint8_t)(rand() & 127), 0});
      else if (k == 5) in.push_back({(uint8_t)((g << 4) | 0xF), (uint8_t)(0xF8 + rand() % 8), 0, 0});
      else if (k == 6) { std::vector<uint8_t> d; const int len = rand() % 40; for (int j = 0; j < len; ++j) d.push_back(1 + rand() % 126); for (auto &p : sysexPackets(g, d)) in.push_back(p); }
      else in.push_back({(uint8_t)((g << 4) | 0xC), 0xC0, (uint8_t)(rand() & 127), 0}); }
    Side tx, rx; std::vector<uint8_t> wire;
    for (size_t i = 0; i < in.size(); ++i) { tx.send(port, in[i].data()); if (i % 40 == 39) { auto w = tx.drain(); wire.insert(wire.end(), w.begin(), w.end()); } } // the output pump keeps up
    { auto w = tx.drain(); wire.insert(wire.end(), w.begin(), w.end()); }
    Packets got = receive(rx, wire, port);
    // real time has priority, so compare the streams apart, then everything else in order
    Packets inRt, inRest, gotRt, gotRest; for (auto &p : in) ((p[0] & 15) == 0xF ? inRt : inRest).push_back(p); for (auto &p : got) ((p[0] & 15) == 0xF ? gotRt : gotRest).push_back(p);
    CHECK(inRt == gotRt, "port %u real time bytes: %zu in, %zu out", port, inRt.size(), gotRt.size()); CHECK(inRest == gotRest, "port %u other packets: %zu in, %zu out", port, inRest.size(), gotRest.size()); }
  printf("6. real time words sent between the words of a message still arrive, and SysEx isn't damaged\n");
  { Side tx, rx; std::vector<uint8_t> d; for (int i = 0; i < 50; ++i) d.push_back(1 + i); Packets sx = sysexPackets(0, d); Packets rtIn;
    for (auto &p : sx) { tx.send(7, p.data()); const uint8_t rt[4] = {0x0F, 0xF8, 0, 0}; tx.send(7, rt); rtIn.push_back({0x0F, 0xF8, 0, 0}); }
    Packets got = receive(rx, tx.drain(true), 7); Packets gs, gr; for (auto &p : got) ((p[0] & 15) == 0xF ? gr : gs).push_back(p);
    CHECK(gs == sx && gr == rtIn, "%zu sysex packets in, %zu out, %zu real time in, %zu out", sx.size(), gs.size(), rtIn.size(), gr.size()); }
  printf("7. a MIDI 2.0 message sent over the link to a MIDI 1.0 port arrives as MIDI 1.0\n");
  { Side rx; const uint32_t note[2] = {(4u << 28) | (0x93u << 16) | (64u << 8), 0xC0000000u}; std::vector<uint8_t> wire; for (int i = 0; i < 2; ++i) { LinkFrame f; encodeLinkWord(6, note[i], i == 0, f); for (uint8_t k = 0; k < 6; ++k) wire.push_back(f.b[k]); }
    Packets got = receive(rx, wire, 6); CHECK(got.size() == 1 && got[0][0] == 0x09 && got[0][1] == 0x93 && got[0][2] == 64 && got[0][3] == (0xC000 >> 9), "note on velocity 0xC000 is %d", got.empty() ? -1 : got[0][3]); }
  printf("%s\n", fails ? "SOME CHECKS FAILED" : "all checks pass");
  return fails ? 1 : 0;
}
