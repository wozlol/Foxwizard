#pragma once

// FoxWizard 1.93, Loop Lock. Keeps a loop at one place in the clock's beat. It works out how fast the loop's time should
// run for the loop to last a whole number of beats at the clock's tempo, and nudges that speed so the loop stays at the
// same place in the beat. The caller hands the speed to Looper::setTimeWarp.

#include <stdint.h>

#include "clock_engine.h"
#include "looper.h"

namespace fw {

struct LoopLockInput {
  uint64_t loopNowUs;      // the loop's time now
  uint64_t cycleStartUs;   // the loop's time at its top
  uint32_t lengthUs;       // the loop's length in its own time
  double pulsePos;         // the clock's count now, pulses with a fraction
  double pulseUs;          // wall microseconds in one pulse at the clock's tempo
  double beatPulses;       // pulses in a beat, the loop is held to a whole number of them
  bool clockUsable;        // the count and the tempo can be trusted this moment, otherwise the last speed holds
};

class LoopLock {
 public:
  static constexpr uint32_t kUnity = 1u << 24;
  void reset();
  bool engaged() const { return pulses_ > 0.0; }
  double loopPulses() const { return pulses_; }
  double lastErrorPulses() const { return lastError_; } // how far the loop was from its place in the beat at the last update
  // Starts the lock when it hasn't started, then returns the loop's speed (Looper::setTimeWarp's rateQ24).
  uint32_t update(const LoopLockInput &in);
  // The clock's count started over, or the loop was started again at its top: keep the loop where it is in its cycle.
  void rebase(const LoopLockInput &in);

 private:
  double phasePulses(const LoopLockInput &in) const;
  double pulses_ = 0.0;     // the loop's length in pulses, whole beats
  double offset_ = 0.0;     // the loop's place in its cycle, in pulses, is (the count + offset_) within the length
  uint32_t lengthUs_ = 0;   // the length pulses_ was worked out for, so Double and Half carry it on
  uint32_t rateQ24_ = kUnity;
  double lastError_ = 0.0;
};

// Runs the lock for the sketch: every main loop pass, hands the loop the speed that keeps it at one place in the
// clock's beat. It starts on a clock that is arriving and then keeps going, on the FoxWizard's own count when the
// clock goes quiet (Auto), until the loop is cleared or stops, or the clock setting is master.
class LoopLockDriver {
 public:
  void service(uint64_t nowUs, ClockEngine &clock, Looper &looper, bool clockFollows, bool restartOnStart,
               double beatPulses, Looper::ReleaseFn release, void *context);
  bool engaged() const { return lock_.engaged(); }
  double lastErrorPulses() const { return lock_.lastErrorPulses(); }

 private:
  LoopLock lock_;
  uint64_t lastUs_ = 0;
  uint32_t lastResets_ = 0;
  uint32_t lastStarts_ = 0;
};

}  // namespace fw
