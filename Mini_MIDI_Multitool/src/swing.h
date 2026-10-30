#pragma once

// FoxWizard 1.116, swing. It delays the off-beat notes and nothing else: the MIDI clock stays straight. Plain C++ so a computer
// can test it.
//
// The swing base is a note value, a unit of pulses (24 for a 16th, 48 for an 8th at the clock's 96 per quarter note). Two units
// make a pair. Swing stretches the first unit of each pair and shrinks the second, so the pair keeps its length and every pair
// starts where it would have, nothing builds up. The amount is the share of the pair the first unit takes: 50 is straight, 66 is
// a triplet feel, 75 is the most (the second unit gets a third of what the first has). 0 also means off.
//
// The warp moves every position, not only the off-beat ones, so a faster arp division swings with it. It never moves a position
// by more than a quarter of a pair (at 75), and the order of positions is kept.

#include <stdint.h>

namespace fw {

constexpr uint8_t kSwingMaxPercent = 75;

inline uint8_t swingClamp(uint8_t percent) { return percent <= 50 ? 0 : (percent > kSwingMaxPercent ? kSwingMaxPercent : percent); }

// Where a position on the straight grid lands once swung. pos and unit are in pulses.
inline double swingWarp(double pos, uint32_t unit, uint8_t percent) {
  percent = swingClamp(percent);
  if (percent == 0 || unit == 0) return pos;
  const double s = percent / 100.0;
  const double pair = 2.0 * unit;
  uint64_t pairs = static_cast<uint64_t>(pos / pair);
  double base = static_cast<double>(pairs) * pair;
  double x = pos - base;
  if (x < 0.0) { x += pair; base -= pair; } // a rounding edge, keeps x inside the pair
  if (x >= pair) { x -= pair; base += pair; }
  return base + (x < unit ? x * 2.0 * s : 2.0 * s * unit + (x - unit) * 2.0 * (1.0 - s));
}

// The arp's test for a step: due once the clock's position, with its fraction, reaches the step's swung place.
inline bool swungDue(double pulsePos, uint32_t stepPulse, uint32_t unit, uint8_t percent) {
  return pulsePos >= swingWarp(static_cast<double>(stepPulse), unit, percent);
}

// After the step at fired has played: the next step on the straight grid, and if that one is already due as well (the arp fell a
// whole step behind, a stall), skipped ahead to the first step still to come, as the arp does without swing.
inline uint32_t swungNextStep(uint32_t fired, uint16_t div, double pulsePos, uint32_t pulse, uint32_t unit, uint8_t percent) {
  uint32_t next = fired + div;
  if (swungDue(pulsePos, next, unit, percent)) {
    next = ((pulse / div) + 1) * div;
    while (swungDue(pulsePos, next, unit, percent)) next += div;
  }
  return next;
}

// Record Quantize on the looper snaps a time to the nearest line of its grid. With swing the grid's lines are at 0, the swung
// off-beat, and the end of each pair of quantum-long units. Times are microseconds in the loop. 0 percent is the plain grid.
inline uint32_t swingQuantize(uint32_t atUs, uint32_t quantumUs, uint8_t percent) {
  if (quantumUs == 0) return atUs;
  percent = swingClamp(percent);
  if (percent == 0) {
    return static_cast<uint32_t>(((static_cast<uint64_t>(atUs) + quantumUs / 2U) / quantumUs) * quantumUs);
  }
  const uint64_t pair = 2ULL * quantumUs;
  const uint64_t base = (static_cast<uint64_t>(atUs) / pair) * pair;
  const uint64_t x = static_cast<uint64_t>(atUs) - base;
  const uint64_t swung = static_cast<uint64_t>(percent) * quantumUs / 50ULL; // 2 * (percent / 100) * quantum
  const uint64_t d0 = x, d1 = x > swung ? x - swung : swung - x, d2 = pair - x;
  uint64_t line = 0;
  if (d1 < d0 && d1 <= d2) line = swung;
  else if (d2 < d0 && d2 < d1) line = pair;
  return static_cast<uint32_t>(base + line);
}

}  // namespace fw
