#pragma once

// Ported from ARPnMIDI (arpnmidi os/arpnmidi_main_brain/src). Change from there: any time
// signature instead of 4/4 or 3/4.

#include <stdint.h>

#include "musical_time.h"

namespace fw {

enum class TransportEvent : uint8_t {
  None = 0,
  Start,
  Continue,
  Stop
};

struct ClockConfig {
  bool followExternal = false;
  // FoxWizard 1.92, Auto: follows external clock while it arrives, and leads with its own, at the tempo the
  // external clock had, while it doesn't. Needs followExternal.
  bool autoLead = false;
  bool sendClock = false;
  uint8_t beatsPerBar = 4;
  uint8_t beatUnit = 4;
  uint16_t manualBpm = 120;
};

class ClockEngine {
 public:
  void begin(const ClockConfig &config, uint64_t nowUs);
  void setConfig(const ClockConfig &config, uint64_t nowUs);

  // Handles F8, FA, FB, and FC. The caller remains responsible for forwarding
  // messages when Clock Out is enabled.
  TransportEvent receiveRealtime(uint8_t status, uint64_t nowUs);

  // Returns true once for every internally generated MIDI clock byte that is
  // due. Call repeatedly until it returns false.
  bool takeInternalClock(uint64_t nowUs);

  void start(uint64_t nowUs);
  void resume(uint64_t nowUs);
  void stop();

  bool running() const { return running_; }
  bool followingExternal() const { return config_.followExternal; }
  bool clockOutEnabled() const { return config_.sendClock; }
  bool externalClockPresent(uint64_t nowUs) const;
  bool synchronizedAdvanceAllowed(uint64_t nowUs) const;
  // True when the clock being followed is the one driving the count. False for a master, and for Auto while it
  // leads with its own clock because the external one is quiet or stopped.
  bool followingLive(uint64_t nowUs);

  float bpm() const;
  uint32_t internalPulse() const { return internalPulse_; }
  uint32_t barPulse() const;
  uint64_t phaseOriginUs() const { return phaseOriginUs_; }
  uint32_t lastExternalClockAgeMs(uint64_t nowUs) const;

 private:
  void receiveClock(uint64_t nowUs);
  uint64_t midiClockIntervalUs() const;
  uint64_t externalTimeoutUs() const;
  void resetPhase(uint64_t nowUs);

  ClockConfig config_{};
  bool leading_ = false;        // Auto: running its own clock because the external one is quiet or stopped
  bool sourceStopped_ = false;  // Auto: the source sent Stop, its clock bytes don't count until it sends Start or Continue
  float leadBpm_ = 0;           // the tempo Auto leads at, the external tempo as it went quiet
  uint64_t graceUntilUs_ = 0;   // after a Start or Continue the source counts as present, its first clock byte is due
  bool running_ = true;
  bool haveExternalClock_ = false;
  uint64_t phaseOriginUs_ = 0;
  uint64_t lastExternalClockUs_ = 0;
  uint64_t filteredExternalIntervalUs_ = 0;
  uint64_t nextInternalClockUs_ = 0;
  uint32_t internalPulse_ = 0;
};

}  // namespace fw
