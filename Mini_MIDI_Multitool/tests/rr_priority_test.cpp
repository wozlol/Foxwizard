// Tests of src/rr_priority.h, the Round Robin Priority logic, on a computer.
//   clang++ -std=c++17 -I .. -I ../src rr_priority_test.cpp -o /tmp/rr_priority_test && /tmp/rr_priority_test

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "rr_priority.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Slot {
  uint8_t note = 0xFF;
  uint8_t velocity = 0;
  uint16_t order = 0;
  uint8_t channel = 0xFF;
};

// An entry, as Patchling writes it.
static void addEntry(uint8_t *pool, uint16_t route, const std::vector<uint8_t> &channels) {
  uint8_t pos = fw::rrOrderUsed(pool);
  pool[pos] = (uint8_t)(route + 1);
  pool[pos + 1] = (uint8_t)channels.size();
  for (size_t i = 0; i < channels.size(); ++i) {
    uint8_t &b = pool[pos + 2 + i / 2];
    if (i % 2 == 0) b = (uint8_t)(channels[i] << 4); else b |= channels[i];
  }
}

static std::vector<uint8_t> ranked(uint16_t mask, const uint8_t *pool, uint16_t route) {
  uint8_t out[16];
  const uint8_t n = fw::rrRanked(mask, pool, route, out);
  return std::vector<uint8_t>(out, out + n);
}

static void testRanking() {
  uint8_t pool[fw::kRrOrderPoolBytes] = {0};
  CHECK((ranked(0b1011, pool, 0) == std::vector<uint8_t>{0, 1, 3}), "no entry ranks lowest first");
  CHECK(ranked(0, pool, 0).empty(), "empty mask");
  addEntry(pool, 2, {3, 1, 2});
  CHECK((ranked(0b1110, pool, 2) == std::vector<uint8_t>{3, 1, 2}), "custom order kept");
  CHECK((ranked(0b1110, pool, 0) == std::vector<uint8_t>{1, 2, 3}), "another route keeps the default");
  CHECK((ranked(0b1010, pool, 2) == std::vector<uint8_t>{3, 1}), "a channel taken off the mask drops out of its place");
  CHECK((ranked(0b11110, pool, 2) == std::vector<uint8_t>{3, 1, 2, 4}), "a channel added after the order was saved comes last, lowest first");
  addEntry(pool, 5, {15, 0, 7, 8, 9});
  CHECK((ranked(0x81C1, pool, 5) == std::vector<uint8_t>{15, 0, 7, 8, 6}), "five channels, odd count, then the one it does not name");
  CHECK((ranked(0b1110, pool, 2) == std::vector<uint8_t>{3, 1, 2}), "the first entry still found after a second");
  // a damaged block
  uint8_t bad[fw::kRrOrderPoolBytes] = {0};
  bad[0] = 1; bad[1] = 17;
  CHECK((ranked(0b111, bad, 0) == std::vector<uint8_t>{0, 1, 2}), "damaged entry falls back to the default");
  std::memset(bad, 0xFF, sizeof(bad));
  CHECK(ranked(0b111, bad, 0).size() == 3, "all 0xFF falls back to the default");
  CHECK(fw::rrOrderUsed(bad) == 0, "damaged block uses nothing");
  // a duplicate channel in an entry is ignored
  uint8_t dup[fw::kRrOrderPoolBytes] = {0};
  addEntry(dup, 0, {2, 2, 1});
  CHECK((ranked(0b110, dup, 0) == std::vector<uint8_t>{2, 1}), "duplicates are ignored");
}

// The bytes Patchling (js/midi.js rrOrderToBytes) makes for these routes: 0 has order 3,1,0 on channels 1,2,4, route 1 is in the default
// order so has no entry, 2 has 15,0,7,8,6 on 0x81C1, 3 has all 16 from the top down.
static void testPatchlingBytes() {
  const uint8_t made[] = {1, 3, 49, 0, 3, 5, 240, 120, 96, 4, 16, 254, 220, 186, 152, 118, 84, 50, 16};
  uint8_t pool[fw::kRrOrderPoolBytes] = {0};
  std::memcpy(pool, made, sizeof(made));
  CHECK(fw::rrOrderUsed(pool) == sizeof(made), "used length of Patchling's block, %d", (int)fw::rrOrderUsed(pool));
  CHECK((ranked(0b1011, pool, 0) == std::vector<uint8_t>{3, 1, 0}), "route 0 as Patchling wrote it");
  CHECK((ranked(0b11, pool, 1) == std::vector<uint8_t>{0, 1}), "route 1 has no entry");
  CHECK((ranked(0x81C1, pool, 2) == std::vector<uint8_t>{15, 0, 7, 8, 6}), "route 2 as Patchling wrote it");
  std::vector<uint8_t> top;
  for (int c = 15; c >= 0; --c) top.push_back((uint8_t)c);
  CHECK((ranked(0xFFFF, pool, 3) == top), "route 3, all 16 from the top down");
}

static void testPoolMaintenance() {
  uint8_t pool[fw::kRrOrderPoolBytes] = {0};
  addEntry(pool, 1, {3, 1});
  addEntry(pool, 3, {2, 0, 1});
  addEntry(pool, 4, {5, 4});
  fw::rrOrderRemoveRoute(pool, 3);
  CHECK((ranked(0b1011, pool, 1) == std::vector<uint8_t>{3, 1, 0}), "route 1 unchanged by removing route 3");
  CHECK((ranked(0b110000, pool, 3) == std::vector<uint8_t>{5, 4}), "route 4 is now route 3");
  CHECK(fw::rrOrderUsed(pool) == 3 + 3, "used length after removal");
  fw::rrOrderRemoveRoute(pool, 0);
  CHECK((ranked(0b1011, pool, 0) == std::vector<uint8_t>{3, 1, 0}), "removing route 0 shifts route 1 to 0");
  // moves
  uint8_t m[fw::kRrOrderPoolBytes] = {0};
  addEntry(m, 0, {1, 0});
  addEntry(m, 2, {3, 2});
  fw::rrOrderMoveRoute(m, 0, 2); // route 0 goes to 2, route 2 goes to 1
  CHECK((ranked(0b11, m, 2) == std::vector<uint8_t>{1, 0}), "moved route's entry follows it");
  CHECK((ranked(0b1100, m, 1) == std::vector<uint8_t>{3, 2}), "the route it passed shifts down");
  fw::rrOrderMoveRoute(m, 2, 0); // and back
  CHECK((ranked(0b11, m, 0) == std::vector<uint8_t>{1, 0}), "moved back");
  CHECK((ranked(0b1100, m, 2) == std::vector<uint8_t>{3, 2}), "and the other back");
}

// A model of the route: the same calls the firmware makes.
struct Model {
  Slot slots[16];
  uint16_t orderCounter = 0;
  uint16_t mask = 0;
  uint8_t pool[fw::kRrOrderPoolBytes] = {0};
  int16_t transpose = 0;
  std::map<std::pair<int, int>, int> sounding; // (channel, note) -> count
  int spuriousOff = 0;
  int doubled = 0;
  int lastOnChannel = -1;

  Slot *find(uint8_t note) { for (auto &s : slots) if (s.note == note) return &s; return nullptr; }
  Slot *findOrAlloc(uint8_t note) {
    if (Slot *s = find(note)) return s;
    for (auto &s : slots) if (s.note == 0xFF) { s.note = note; return &s; }
    return nullptr;
  }
  void free(uint8_t note) { if (Slot *s = find(note)) *s = Slot(); }

  void emit(uint8_t channel, bool on, int16_t out) {
    if (out < 0 || out > 127) return;
    auto key = std::make_pair((int)channel, (int)out);
    if (on) { if (++sounding[key] > 1) ++doubled; lastOnChannel = channel; }
    else { if (sounding[key] <= 0) ++spuriousOff; else --sounding[key]; }
  }
  void on(uint8_t note) {
    uint8_t r[16];
    const uint8_t n = fw::rrRanked(mask, pool, 0, r);
    fw::rrNoteOn(slots, 16, orderCounter, findOrAlloc(note), true, r, n, transpose, [&](uint8_t c, bool o, int16_t x) { emit(c, o, x); });
  }
  void off(uint8_t note) {
    fw::rrNoteOff(find(note), transpose, [&](uint8_t c, bool o, int16_t x) { emit(c, o, x); });
    free(note);
  }
  int at(int c, int n) { return sounding[std::make_pair(c, n)]; }
  int soundingCount() { int n = 0; for (auto &kv : sounding) n += kv.second; return n; }
  std::set<int> channelsDown() { std::set<int> s; for (auto &kv : sounding) if (kv.second > 0) s.insert(kv.first.first); return s; }
};

static void testWalkthrough() {
  // The behavior as described: lowest first, then up as notes are added, the oldest cut when all are down.
  Model m;
  m.mask = 0b00001110; // channels 2, 3, 4 (0-based 1, 2, 3)
  m.on(60); CHECK(m.lastOnChannel == 1, "first note takes the lowest");
  m.on(62); CHECK(m.lastOnChannel == 2, "second takes the next up");
  m.off(60);
  m.on(64); CHECK(m.lastOnChannel == 1, "the lowest was freed, so it is taken again");
  m.on(65); CHECK(m.lastOnChannel == 3, "the third channel");
  CHECK(m.soundingCount() == 3, "three notes sounding");
  m.on(67); // all down: the oldest (62 on channel 2) is cut
  CHECK(m.lastOnChannel == 2, "all down: the oldest note's channel is reused");
  CHECK(m.soundingCount() == 3, "still three sounding, the oldest was cut");
  CHECK(m.at(2, 62) == 0, "the oldest was released");
  m.off(62); // its key comes up later: nothing to release
  CHECK(m.spuriousOff == 0, "the cut note's own Note Off does nothing");
  CHECK(m.soundingCount() == 3, "unchanged");
  for (int n : {64, 65, 67}) m.off((uint8_t)n);
  CHECK(m.soundingCount() == 0, "all released");
  // a custom order
  Model c;
  c.mask = 0b00001110;
  addEntry(c.pool, 0, {3, 1, 2});
  c.on(60); CHECK(c.lastOnChannel == 3, "custom: the first ranked channel");
  c.on(61); CHECK(c.lastOnChannel == 1, "custom: second");
  c.on(62); CHECK(c.lastOnChannel == 2, "custom: third");
  // a note pressed again while held releases where it was first
  Model p;
  p.mask = 0b11;
  p.on(60); p.on(60);
  CHECK(p.soundingCount() == 1 && p.doubled == 0, "a repeated Note On does not stack");
  p.off(60);
  CHECK(p.soundingCount() == 0, "and releases cleanly");
  // a Note Off for a key never played does nothing
  Model q;
  q.mask = 0b11;
  q.off(70);
  CHECK(q.spuriousOff == 0 && q.soundingCount() == 0, "unknown Note Off ignored");
  // no channels selected: nothing plays and nothing breaks
  Model e;
  e.on(60); e.off(60);
  CHECK(e.soundingCount() == 0 && e.spuriousOff == 0, "no channels selected");
  // transpose is applied the same on and off, and to a cut note
  Model t;
  t.mask = 0b11; t.transpose = 12;
  t.on(60); t.on(61); t.on(62);
  CHECK(t.at(0, 72) == 0 && t.at(1, 73) == 1 && t.at(0, 74) == 1, "transposed notes, the oldest cut at its own pitch");
  t.off(60); t.off(61); t.off(62);
  CHECK(t.soundingCount() == 0 && t.spuriousOff == 0, "all transposed notes released");
}

// Random playing: keys down and up in any order, retriggers, a changing mask. After every step no channel holds two notes, nothing
// but selected channels sound, and at the end with every key up nothing is left sounding.
static void testFuzz() {
  std::srand(12345);
  long steps = 0;
  for (int round = 0; round < 3000; ++round) {
    Model m;
    m.mask = (uint16_t)(1 + std::rand() % 0xFFFF);
    if (std::rand() % 3 == 0) {
      std::vector<uint8_t> order;
      for (int c = 0; c < 16; ++c) if (m.mask & (1u << c)) order.push_back((uint8_t)c);
      for (size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[std::rand() % i]);
      addEntry(m.pool, 0, order);
    }
    m.transpose = (int16_t)(std::rand() % 25) - 12;
    std::set<int> down;
    const int ops = 20 + std::rand() % 200;
    for (int i = 0; i < ops; ++i) {
      const int note = 40 + std::rand() % 30;
      if (std::rand() % 2 && down.size() < 15) { m.on((uint8_t)note); down.insert(note); }
      else if (!down.empty()) {
        auto it = down.begin(); std::advance(it, std::rand() % down.size());
        m.off((uint8_t)*it); down.erase(it);
      }
      ++steps;
      for (auto &kv : m.sounding) {
        CHECK(kv.second <= 1, "round %d: a note sounds twice", round);
        if (kv.second > 0) CHECK(m.mask & (1u << kv.first.first), "round %d: sounding on a channel not selected", round);
      }
      std::map<int, int> perChannel;
      for (auto &kv : m.sounding) if (kv.second > 0) ++perChannel[kv.first.first];
      for (auto &kv : perChannel) CHECK(kv.second <= 1, "round %d: channel %d holds %d notes", round, kv.first, kv.second);
      CHECK(m.spuriousOff == 0, "round %d: an Off for a note not sounding", round);
      if (failures > 20) return;
    }
    for (int n : down) m.off((uint8_t)n);
    CHECK(m.soundingCount() == 0, "round %d: %d notes left sounding after every key was released", round, m.soundingCount());
    CHECK(m.spuriousOff == 0, "round %d: spurious Off at the end", round);
  }
  std::printf("fuzz: %ld steps\n", steps);
}

// With fewer keys than channels, every note takes the first free channel in rank order.
static void testLowestFree() {
  std::srand(777);
  for (int round = 0; round < 2000; ++round) {
    Model m;
    m.mask = (uint16_t)(1 + std::rand() % 0xFFFF);
    uint8_t r[16];
    const uint8_t n = fw::rrRanked(m.mask, m.pool, 0, r);
    std::set<int> downCh;
    std::vector<int> keys;
    for (int i = 0; i < 40; ++i) {
      if ((std::rand() % 2) && (int)keys.size() < n) {
        const int note = 40 + (int)keys.size() + 20 * (int)(std::rand() % 2) * 0; // distinct
        int free = -1;
        for (uint8_t k = 0; k < n; ++k) if (!downCh.count(r[k])) { free = r[k]; break; }
        int use = note; while (std::find(keys.begin(), keys.end(), use) != keys.end()) ++use;
        m.on((uint8_t)use);
        CHECK(m.lastOnChannel == free, "round %d: took channel %d, first free is %d", round, m.lastOnChannel, free);
        downCh.insert(m.lastOnChannel);
        keys.push_back(use);
      } else if (!keys.empty()) {
        const size_t k = std::rand() % keys.size();
        Slot *s = m.find((uint8_t)keys[k]);
        const int ch = s->channel;
        m.off((uint8_t)keys[k]);
        downCh.erase(ch);
        keys.erase(keys.begin() + k);
      }
      if (failures > 20) return;
    }
    for (int k : keys) m.off((uint8_t)k);
  }
}

// Velocity: the range cut into equal bands, harder notes further along the rank, a key pressed again moves, Note Off finds its channel.
static void testVelocity() {
  CHECK(fw::rrVelocityBand(1, 2) == 0 && fw::rrVelocityBand(64, 2) == 0 && fw::rrVelocityBand(65, 2) == 1 && fw::rrVelocityBand(127, 2) == 1, "two channels split at 64/65");
  CHECK(fw::rrVelocityBand(32, 4) == 0 && fw::rrVelocityBand(33, 4) == 1 && fw::rrVelocityBand(64, 4) == 1 && fw::rrVelocityBand(65, 4) == 2 && fw::rrVelocityBand(96, 4) == 2 && fw::rrVelocityBand(97, 4) == 3, "four channels by quarters");
  CHECK(fw::rrVelocityBand(100, 1) == 0 && fw::rrVelocityBand(0, 3) == 0 && fw::rrVelocityBand(200, 3) == 2, "one channel, and out of range velocities stay inside");
  for (uint8_t n = 1; n <= 16; ++n) {
    uint8_t last = 0, size[16] = {0};
    for (int v = 1; v <= 127; ++v) {
      const uint8_t b = fw::rrVelocityBand((uint8_t)v, n);
      CHECK(b >= last && b < n, "n %u: band %u at %d must not go back and stay under %u", n, b, v, n);
      last = b; ++size[b];
    }
    CHECK(last == n - 1, "n %u: the hardest band is the last channel", n);
    uint8_t lo = 255, hi = 0;
    for (uint8_t b = 0; b < n; ++b) { lo = std::min(lo, size[b]); hi = std::max(hi, size[b]); }
    CHECK(hi - lo <= 1, "n %u: bands differ by more than one velocity (%u to %u)", n, lo, hi);
  }
  // The routing: ranked channels 5, 2, 9 (the route's own order), notes land by velocity and their Offs follow.
  uint8_t pool[fw::kRrOrderPoolBytes] = {0};
  addEntry(pool, 0, {5, 2, 9});
  const auto order = ranked((1u << 5) | (1u << 2) | (1u << 9), pool, 0);
  Slot slots[16];
  for (int i = 0; i < 5; ++i) slots[i].note = (uint8_t)(60 + i);
  std::vector<std::string> log;
  auto emit = [&](uint8_t ch, bool on, int16_t pitch) { log.push_back(std::string(on ? "+" : "-") + std::to_string(ch) + ":" + std::to_string(pitch)); };
  const uint8_t n = (uint8_t)order.size();
  fw::rrVelocityNoteOn(&slots[0], order.data(), n, 20, 0, emit);   // soft: rank 1, channel 5
  CHECK(log.size() == 1 && log[0] == "+5:60" && slots[0].channel == 5, "soft note on channel 5, got %s", log.empty() ? "nothing" : log[0].c_str());
  log.clear();
  fw::rrVelocityNoteOn(&slots[1], order.data(), n, 127, 0, emit);  // hard: rank 3, channel 9
  CHECK(log.size() == 1 && log[0] == "+9:61", "hard note on channel 9, got %s", log.empty() ? "nothing" : log[0].c_str());
  log.clear();
  fw::rrVelocityNoteOn(&slots[2], order.data(), n, 64, 0, emit);   // middle: rank 2, channel 2
  CHECK(log.size() == 1 && log[0] == "+2:62", "middle note on channel 2, got %s", log.empty() ? "nothing" : log[0].c_str());
  log.clear();
  fw::rrVelocityNoteOn(&slots[3], order.data(), n, 10, 0, emit);   // no rotation: another soft note shares channel 5
  CHECK(log.size() == 1 && log[0] == "+5:63", "another soft note shares channel 5, got %s", log.empty() ? "nothing" : log[0].c_str());
  // pressed again while down, harder: it lets go where it was and sounds on its new channel
  log.clear();
  fw::rrVelocityNoteOn(&slots[0], order.data(), n, 120, 2, emit);
  CHECK(log.size() == 2 && log[0] == "-5:62" && log[1] == "+9:62", "pressed again: off on 5, on on 9 with transpose, got %s %s", log.empty() ? "" : log[0].c_str(), log.size() > 1 ? log[1].c_str() : "");
  log.clear();
  fw::rrNoteOff(&slots[1], 0, emit);
  CHECK(log.size() == 1 && log[0] == "-9:61" && slots[1].channel == 0xFF, "Note Off lands on the channel it sounds on");
  log.clear();
  fw::rrNoteOff(&slots[1], 0, emit);
  CHECK(log.empty(), "a second Note Off does nothing");
  log.clear();
  fw::rrVelocityNoteOn((Slot *)nullptr, order.data(), n, 100, 0, emit);
  fw::rrVelocityNoteOn(&slots[4], order.data(), 0, 100, 0, emit);
  CHECK(log.empty(), "no slot or no channel, no note");
}

int main() {
  testVelocity();
  testRanking();
  testPatchlingBytes();
  testPoolMaintenance();
  testWalkthrough();
  testFuzz();
  testLowestFree();
  if (failures) { std::printf("%d FAILURES\n", failures); return 1; }
  std::printf("all checks pass\n");
  return 0;
}
