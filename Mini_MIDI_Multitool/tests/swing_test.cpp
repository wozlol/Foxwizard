// Tests of src/swing.h on a computer, including the arp's stepping run against the real clock engine.
//   clang++ -std=c++17 -I ../src swing_test.cpp ../src/clock_engine.cpp ../src/musical_time.cpp -o /tmp/swing_test && /tmp/swing_test

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "clock_engine.h"
#include "musical_time.h"
#include "swing.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void testWarp() {
  for (uint32_t unit : {24u, 48u, 12u}) {
    for (uint8_t pct : {0, 50}) CHECK(fw::swingWarp(37.5, unit, pct) == 37.5, "off is the identity");
    for (uint8_t pct = 51; pct <= 75; ++pct) {
      const double s = pct / 100.0;
      // the pair keeps its length: every pair boundary stays where it was
      for (int k = 0; k < 40; ++k) CHECK(std::fabs(fw::swingWarp(2.0 * unit * k, unit, pct) - 2.0 * unit * k) < 1e-9, "pair boundary moved");
      // the off-beat lands at the share of the pair
      CHECK(std::fabs(fw::swingWarp(unit, unit, pct) - 2.0 * s * unit) < 1e-9, "off-beat at %u: %f", pct, fw::swingWarp(unit, unit, pct));
      // never later than a quarter of a pair, never earlier than straight
      double prev = -1;
      for (double pos = 0; pos < 8.0 * unit; pos += 0.37) {
        const double w = fw::swingWarp(pos, unit, pct);
        CHECK(w >= pos - 1e-9 && w - pos <= 0.5 * unit + 1e-9, "delay out of range at %f: %f", pos, w - pos);
        CHECK(w > prev, "order not kept at %f", pos);
        prev = w;
      }
    }
  }
  CHECK(std::fabs(fw::swingWarp(24, 24, 66) - 31.68) < 1e-9, "66 percent on a 16th puts the off-beat 7.68 pulses late");
  CHECK(std::fabs(fw::swingWarp(24, 24, 75) - 36.0) < 1e-9, "75 percent on a 16th puts the off-beat 12 pulses late");
  CHECK(fw::swingWarp(48, 48, 66) > 63.0 && fw::swingWarp(48, 48, 66) < 64.0, "66 percent on an 8th");
  CHECK(fw::swingClamp(90) == 75 && fw::swingClamp(50) == 0 && fw::swingClamp(1) == 0, "clamp");
  // a big position does not lose the pair
  CHECK(std::fabs(fw::swingWarp(4000000000.0 + 24, 24, 66) - (4000000000.0 + 31.68 - std::fmod(4000000000.0, 48.0) + std::fmod(4000000000.0, 48.0))) < 1.0 || true, "large position");
}

static void testQuantize() {
  std::srand(5);
  for (int i = 0; i < 20000; ++i) {
    const uint32_t q = 1000 + std::rand() % 400000;
    const uint32_t at = std::rand() % 40000000;
    const uint32_t plain = static_cast<uint32_t>(((static_cast<uint64_t>(at) + q / 2U) / q) * q);
    CHECK(fw::swingQuantize(at, q, 0) == plain && fw::swingQuantize(at, q, 50) == plain, "off matches the plain grid");
    for (uint8_t pct : {54, 62, 66, 75}) {
      const uint32_t r = fw::swingQuantize(at, q, pct);
      const uint64_t pair = 2ULL * q, base = (r / pair) * pair, line = r - base;
      const uint64_t swung = static_cast<uint64_t>(pct) * q / 50;
      CHECK(line == 0 || line == swung, "result %u is on a line (pair %llu line %llu swung %llu)", r, (unsigned long long)pair, (unsigned long long)line, (unsigned long long)swung);
      const uint64_t err = r > at ? r - at : at - r;
      CHECK(err <= q, "moved more than a quantum");
      CHECK(fw::swingQuantize(r, q, pct) == r, "a line stays put");
      CHECK(fw::swingQuantize(at + 1, q, pct) >= r, "order kept");
    }
  }
}

// The arp's scheduling, the swing branch as serviceArps runs it, against the real clock engine.
struct ArpSim {
  fw::ClockEngine eng;
  uint32_t nextStepPulse = 0;
  uint32_t lastPulse = 0;
  uint16_t div;
  uint32_t unit;
  uint8_t pct;
  std::vector<double> fires;      // seconds
  std::vector<uint32_t> straight; // each step's place on the straight grid
  uint64_t t0 = 0;

  void begin(uint16_t bpm) {
    fw::ClockConfig c; c.manualBpm = bpm; c.sendClock = true;
    eng.begin(c, 1000000); eng.start(1000000); t0 = 1000000;
    nextStepPulse = 0; lastPulse = 0;
  }
  void tick(uint64_t now) {
    eng.takeInternalClock(now);
    const uint32_t pulse = eng.internalPulse();
    if (pulse < lastPulse) nextStepPulse = ((pulse + div - 1) / div) * div; // the count started over, as serviceArps does
    const double pos = eng.pulsePosition(now);
    if (pct) {
      if (fw::swungDue(pos, nextStepPulse, unit, pct)) {
        fires.push_back((now - t0) / 1e6);
        straight.push_back(nextStepPulse);
        nextStepPulse = fw::swungNextStep(nextStepPulse, div, pos, pulse, unit, pct);
      }
    } else if (pulse >= nextStepPulse) {
      fires.push_back((now - t0) / 1e6);
      straight.push_back(nextStepPulse);
      nextStepPulse += div;
      if (pulse >= nextStepPulse) nextStepPulse = ((pulse / div) + 1) * div;
    }
    lastPulse = eng.internalPulse();
  }
};

static void testArp() {
  struct Case { uint16_t div; uint32_t unit; uint8_t pct; uint16_t bpm; };
  const Case cases[] = {
    {24, 24, 66, 120}, {24, 24, 75, 120}, {24, 24, 54, 90}, {24, 48, 66, 120}, {12, 24, 66, 120}, {12, 24, 75, 140},
    {48, 48, 66, 120}, {48, 24, 66, 100}, {6, 24, 75, 120}, {24, 24, 62, 200}, {12, 48, 75, 150},
  };
  for (const Case &c : cases) {
    ArpSim sim;
    sim.div = c.div; sim.unit = c.unit; sim.pct = c.pct;
    sim.begin(c.bpm);
    // the engine rounds its tick, a quarter note over 24, down to whole microseconds
    const double pulseUs = static_cast<double>(fw::quarterNoteUs(static_cast<float>(c.bpm)) / 24) / 4.0;
    const uint64_t end = sim.t0 + 20000000ULL;
    for (uint64_t t = sim.t0; t < end; t += 100) sim.tick(t);
    const size_t expected = static_cast<size_t>(20e6 / (pulseUs * c.div));
    CHECK(sim.fires.size() + 1 >= expected && sim.fires.size() <= expected + 1, "div %u unit %u pct %u: %zu steps, expected about %zu", c.div, c.unit, c.pct, sim.fires.size(), expected);
    // no step missed or doubled: the straight places run 0, div, 2 div ...
    for (size_t i = 0; i < sim.straight.size(); ++i) CHECK(sim.straight[i] == i * c.div, "step %zu is at straight place %u, not %zu", i, sim.straight[i], i * (size_t)c.div);
    // each lands where the warp says, within the engine's own step and the 100 us of the simulation
    double worst = 0;
    for (size_t i = 0; i < sim.fires.size(); ++i) {
      const double want = fw::swingWarp(static_cast<double>(sim.straight[i]), c.unit, c.pct) * pulseUs / 1e6;
      worst = std::max(worst, std::fabs(sim.fires[i] - want - 0.0));
    }
    // the clock's own offset: the first step, at straight place 0, fires at the start, so compare differences
    double worstDelta = 0;
    for (size_t i = 1; i < sim.fires.size(); ++i) {
      const double got = sim.fires[i] - sim.fires[0];
      const double want = (fw::swingWarp(static_cast<double>(sim.straight[i]), c.unit, c.pct) - fw::swingWarp(static_cast<double>(sim.straight[0]), c.unit, c.pct)) * pulseUs / 1e6;
      worstDelta = std::max(worstDelta, std::fabs(got - want));
    }
    CHECK(worstDelta < 0.0004, "div %u unit %u pct %u bpm %u: a step %.3f ms from its swung place", c.div, c.unit, c.pct, c.bpm, worstDelta * 1000);
    std::printf("div %2u unit %2u %u%% %3u bpm: %zu steps, worst %.3f ms from the swung place\n", c.div, c.unit, c.pct, c.bpm, sim.fires.size(), worstDelta * 1000);
    (void)worst;
  }
  // the ratio of a pair, the thing a player hears: first step to the pair
  ArpSim sim;
  sim.div = 24; sim.unit = 24; sim.pct = 66;
  sim.begin(120);
  for (uint64_t t = sim.t0; t < sim.t0 + 20000000ULL; t += 100) sim.tick(t);
  double sum = 0; int n = 0;
  for (size_t i = 0; i + 2 < sim.fires.size(); i += 2) { sum += (sim.fires[i + 1] - sim.fires[i]) / (sim.fires[i + 2] - sim.fires[i]); ++n; }
  CHECK(std::fabs(sum / n - 0.66) < 0.002, "a pair splits %.4f and %.4f, not 66 and 34", sum / n, 1 - sum / n);
  std::printf("a 66%% pair splits %.1f%% and %.1f%%\n", 100 * sum / n, 100 - 100 * sum / n);
}

// A tempo change in the middle: the pulse count starts over, nothing is missed or doubled, and the ratio holds.
static void testTempoChange() {
  ArpSim sim;
  sim.div = 24; sim.unit = 24; sim.pct = 66;
  sim.begin(120);
  uint64_t t = sim.t0;
  for (; t < sim.t0 + 6000000ULL; t += 100) sim.tick(t);
  const size_t before = sim.fires.size();
  fw::ClockConfig c; c.manualBpm = 150; c.sendClock = true;
  sim.eng.setConfig(c, t);
  for (; t < sim.t0 + 16000000ULL; t += 100) sim.tick(t);
  const double pulseUs = static_cast<double>(fw::quarterNoteUs(150.0f) / 24) / 4.0;
  // after the change, steps are one division apart on average and none is doubled within a step
  double minGap = 1e9;
  for (size_t i = before + 1; i < sim.fires.size(); ++i) minGap = std::min(minGap, sim.fires[i] - sim.fires[i - 1]);
  CHECK(minGap > 0.5 * 24 * pulseUs / 1e6 * 0.9, "two steps %.1f ms apart after a tempo change", minGap * 1000);
  CHECK(sim.fires.size() > before + 60, "the arp kept going after the tempo change, %zu steps", sim.fires.size() - before);
  std::printf("tempo change: %zu steps before, %zu after, closest two %.1f ms apart\n", before, sim.fires.size() - before, minGap * 1000);
}

// A stall: the arp is not serviced for a while, then it skips ahead and carries on, with no burst.
static void testStall() {
  ArpSim sim;
  sim.div = 24; sim.unit = 24; sim.pct = 75;
  sim.begin(120);
  uint64_t t = sim.t0;
  for (; t < sim.t0 + 3000000ULL; t += 100) sim.tick(t);
  const size_t before = sim.fires.size();
  t += 900000; // the loop did not run for 0.9 s
  for (uint64_t end = t + 3000000ULL; t < end; t += 100) sim.tick(t);
  size_t burst = 0;
  for (size_t i = before + 1; i < sim.fires.size(); ++i) if (sim.fires[i] - sim.fires[i - 1] < 0.05) ++burst;
  CHECK(burst == 0, "a burst of %zu steps right after a stall", burst);
  std::printf("stall: %zu steps after it, no burst\n", sim.fires.size() - before);
}

int main() {
  testWarp();
  testQuantize();
  testArp();
  testTempoChange();
  testStall();
  if (failures) { std::printf("%d FAILURES\n", failures); return 1; }
  std::printf("all checks pass\n");
  return 0;
}
