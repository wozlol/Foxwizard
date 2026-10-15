#pragma once

// Ported from ARPnMIDI (arpnmidi os/arpnmidi_main_brain/src). Change from there: any time
// signature instead of 4/4 or 3/4.

#include <stdint.h>

namespace fw {

// 96 internal pulses per quarter note represents straight, triplet, and dotted
// values exactly through dotted and triplet 1/64 notes.
constexpr uint16_t kInternalPpqn = 96;
constexpr uint8_t kMidiClockPpqn = 24;
constexpr uint8_t kInternalPulsesPerMidiClock = kInternalPpqn / kMidiClockPpqn;

enum class MusicalDivision : uint8_t {
  Whole = 0,
  HalfDotted,
  Half,
  QuarterDotted,
  HalfTriplet,
  Quarter,
  EighthDotted,
  QuarterTriplet,
  Eighth,
  SixteenthDotted,
  EighthTriplet,
  Sixteenth,
  ThirtySecondDotted,
  SixteenthTriplet,
  ThirtySecond,
  SixtyFourthDotted,
  ThirtySecondTriplet,
  SixtyFourth,
  SixtyFourthTriplet,
  Count
};

constexpr uint8_t kMusicalDivisionCount =
    static_cast<uint8_t>(MusicalDivision::Count);

const char *divisionName(MusicalDivision division);
uint16_t divisionPulses(MusicalDivision division);
uint64_t quarterNoteUs(float bpm);
uint64_t divisionDurationUs(MusicalDivision division, float bpm);
// beatUnit is the time signature's bottom number (2, 4, 8, 16).
uint32_t barPulses(uint8_t beatsPerBar, uint8_t beatUnit);
uint64_t barDurationUs(float bpm, uint8_t beatsPerBar, uint8_t beatUnit);
bool isDotted(MusicalDivision division);
bool isTriplet(MusicalDivision division);

}  // namespace fw
