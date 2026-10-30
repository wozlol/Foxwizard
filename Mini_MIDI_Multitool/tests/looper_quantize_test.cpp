// Record Quantize on the looper, with swing: a note's start goes to the grid and the note always keeps the length it was played.
//   clang++ -std=c++17 -I ../src looper_quantize_test.cpp ../src/looper.cpp -o /tmp/looper_quantize_test && /tmp/looper_quantize_test

#include <cstdio>
#include <vector>

#include "looper.h"

using namespace fw;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Played { uint32_t atUs; uint8_t status; uint8_t note; };
static std::vector<Played> played;
static void emit(void *, uint8_t, const LoopMidiEvent &e) { played.push_back({e.atUs, e.status, e.data1}); }
static void release(void *, uint8_t) {}

// Records one bar (2 s) of notes given as {at ms, length ms, note}, quantized to 125 ms with the given swing, and returns the loop's events.
struct Note { uint32_t atMs, lengthMs; uint8_t note; };
static std::vector<Played> record(const std::vector<Note> &notes, uint8_t swing, uint32_t quantumUs = 125000) {
  Looper looper;
  looper.reset();
  looper.setRecordQuantizeUs(quantumUs);
  looper.setRecordSwing(swing);
  looper.armRecord(0, 2000000, false);
  struct Ev { uint64_t at; bool on; uint8_t note; };
  std::vector<Ev> evs;
  for (const Note &n : notes) { evs.push_back({1000000ULL + n.atMs * 1000ULL, true, n.note}); evs.push_back({1000000ULL + (n.atMs + n.lengthMs) * 1000ULL, false, n.note}); }
  for (size_t i = 1; i < evs.size(); ++i) for (size_t j = i; j > 0 && evs[j].at < evs[j - 1].at; --j) std::swap(evs[j], evs[j - 1]);
  for (const Ev &e : evs) {
    LoopMidiEvent m;
    m.status = e.on ? 0x90 : 0x80; m.data1 = e.note; m.data2 = e.on ? 100 : 0;
    looper.capture(e.at, m);
  }
  looper.finishRecording(1000000ULL + 2000000ULL);
  played.clear();
  looper.start(5000000);
  for (uint64_t t = 5000000; t < 5000000 + 2000000; t += 500) looper.tick(t, emit, release, nullptr);
  return played;
}

static uint32_t timeOf(const std::vector<Played> &p, uint8_t note, bool on) {
  for (const Played &e : p) if (e.note == note && ((e.status & 0xF0) == 0x90) == on) return e.atUs / 1000;
  return 0xFFFFFFFF;
}

int main() {
  // Straight, 125 ms grid. A 40 ms note played at 130 starts at 125 and keeps its 40 ms, its end is not snapped.
  {
    auto p = record({{0, 40, 60}, {130, 40, 62}, {250, 40, 64}, {370, 40, 65}, {490, 40, 67}}, 0);
    const uint32_t want[] = {0, 125, 250, 375, 500};
    const uint8_t notes[] = {60, 62, 64, 65, 67};
    for (int i = 0; i < 5; ++i) {
      CHECK(timeOf(p, notes[i], true) == want[i], "note %d On at %u, wanted %u", notes[i], timeOf(p, notes[i], true), want[i]);
      CHECK(timeOf(p, notes[i], false) == want[i] + 40, "note %d Off at %u, wanted %u (it keeps its 40 ms)", notes[i], timeOf(p, notes[i], false), want[i] + 40);
    }
  }
  // Longer notes too, the end is never put on a line by itself. A 130 ms note played at 10 starts at 0 and ends at 130, not 125.
  {
    auto p = record({{10, 130, 60}, {240, 150, 62}}, 0);
    CHECK(timeOf(p, 60, true) == 0 && timeOf(p, 60, false) == 130, "a 130 ms note is 0 to 130, got %u to %u", timeOf(p, 60, true), timeOf(p, 60, false));
    CHECK(timeOf(p, 62, true) == 250 && timeOf(p, 62, false) == 400, "a 150 ms note is 250 to 400, got %u to %u", timeOf(p, 62, true), timeOf(p, 62, false));
  }
  // 75% swing on 125 ms: lines at 0, 187.5, 250, 437.5, 500. A 90 ms note on the first line used to collapse.
  {
    auto p = record({{0, 90, 60}, {130, 40, 62}, {490, 90, 67}}, 75);
    CHECK(timeOf(p, 60, true) == 0 && timeOf(p, 60, false) == 90, "swung: first note 0 to 90, got %u to %u", timeOf(p, 60, true), timeOf(p, 60, false));
    CHECK(timeOf(p, 62, true) == 187 && timeOf(p, 62, false) == 227, "swung: the off-beat note 187 to 227, got %u to %u", timeOf(p, 62, true), timeOf(p, 62, false));
    CHECK(timeOf(p, 67, true) == 500 && timeOf(p, 67, false) == 590, "swung: last note 500 to 590, got %u to %u", timeOf(p, 67, true), timeOf(p, 67, false));
  }
  // An overdub put in while the loop plays: a hold keeps its length too.
  {
    Looper looper;
    looper.reset();
    looper.setRecordQuantizeUs(125000);
    looper.armRecord(0, 2000000, false);
    LoopMidiEvent m;
    m.status = 0x90; m.data1 = 60; m.data2 = 100; looper.capture(1000000, m);
    m.status = 0x80; m.data2 = 0; looper.capture(1040000, m);
    looper.finishRecording(3000000);
    looper.start(5000000);
    looper.armRecord(0, 0, true);
    m.status = 0x90; m.data1 = 62; m.data2 = 100; looper.capture(5000000ULL + 4000000ULL + 130000ULL, m);
    m.status = 0x80; m.data2 = 0; looper.capture(5000000ULL + 4000000ULL + 170000ULL, m);
    looper.finishRecording(5000000ULL + 4000000ULL + 600000ULL);
    played.clear();
    for (uint64_t t = 5000000; t < 5000000 + 2000000; t += 500) looper.tick(t, emit, release, nullptr);
    CHECK(timeOf(played, 62, true) == 125 && timeOf(played, 62, false) == 165, "an overdub hold is 125 to 165, got %u to %u", timeOf(played, 62, true), timeOf(played, 62, false));
  }
  // The same note pressed twice, the second press replaces the first's memory. Two notes at once, each with its own length.
  {
    auto p = record({{0, 30, 60}, {10, 50, 64}, {260, 30, 60}}, 0);
    CHECK(timeOf(p, 64, false) == 50, "a second note held at the same time keeps its own 50 ms, got %u", timeOf(p, 64, false));
    CHECK(timeOf(p, 60, true) == 0, "first note On at 0");
  }
  // A note that ends at the end of the bar stays inside it.
  {
    auto p = record({{1990, 5, 60}}, 0);
    for (const Played &e : p) CHECK(e.atUs < 2000000, "an event at %u us is outside the 2 s loop", e.atUs);
  }
  // Quantize off: nothing moves.
  {
    auto p = record({{0, 40, 60}, {130, 40, 62}}, 0, 0);
    CHECK(timeOf(p, 62, true) == 130 && timeOf(p, 62, false) == 170, "quantize off leaves 130 to 170, got %u to %u", timeOf(p, 62, true), timeOf(p, 62, false));
  }
  if (failures) { std::printf("%d FAILURES\n", failures); return 1; }
  std::printf("all checks pass\n");
  return 0;
}
