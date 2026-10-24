#include "loop_lock.h"

#include <math.h>

namespace fw {

namespace {
constexpr double kSettleUs = 2000000.0;    // a phase error is taken out over about 2 s
constexpr double kMaxNudge = 0.003;        // at most 0.3% faster or slower for the phase, on top of the tempo
}  // namespace

void LoopLock::reset() {
  pulses_ = 0.0;
  offset_ = 0.0;
  lengthUs_ = 0;
  rateQ24_ = kUnity;
}

double LoopLock::phasePulses(const LoopLockInput &in) const {
  const uint64_t elapsed = in.loopNowUs > in.cycleStartUs ? in.loopNowUs - in.cycleStartUs : 0;
  const double position = static_cast<double>(elapsed % in.lengthUs);
  return position * pulses_ / static_cast<double>(in.lengthUs);
}

void LoopLock::rebase(const LoopLockInput &in) {
  if (pulses_ <= 0.0 || !in.lengthUs) return;
  offset_ = phasePulses(in) - in.pulsePos;
}

uint32_t LoopLock::update(const LoopLockInput &in) {
  if (!in.lengthUs || in.pulseUs <= 0.0 || in.beatPulses <= 0.0) return rateQ24_;
  if (pulses_ <= 0.0) {
    // Starts here: the loop's length in whole beats of the clock, and its place in the beat as it is now.
    double beats = floor(static_cast<double>(in.lengthUs) / in.pulseUs / in.beatPulses + 0.5);
    if (beats < 1.0) beats = 1.0;
    pulses_ = beats * in.beatPulses;
    lengthUs_ = in.lengthUs;
    offset_ = phasePulses(in) - in.pulsePos;
  } else if (in.lengthUs != lengthUs_) {
    pulses_ *= static_cast<double>(in.lengthUs) / static_cast<double>(lengthUs_); // Double and Half
    lengthUs_ = in.lengthUs;
  }
  if (!in.clockUsable) return rateQ24_;

  double want = fmod(in.pulsePos + offset_, pulses_);
  if (want < 0.0) want += pulses_;
  double error = want - phasePulses(in);
  if (error > pulses_ / 2.0) error -= pulses_;
  if (error < -pulses_ / 2.0) error += pulses_;
  if (fabs(error) > pulses_ / 4.0) { // too far to slew: take the loop's place as it is
    offset_ = phasePulses(in) - in.pulsePos;
    error = 0.0;
  }
  lastError_ = error;
  const double loopUsPerPulse = static_cast<double>(in.lengthUs) / pulses_;
  const double nominal = loopUsPerPulse / in.pulseUs;           // the loop's time per wall time for a whole number of beats
  double nudge = error * loopUsPerPulse / kSettleUs;
  if (nudge > kMaxNudge) nudge = kMaxNudge;
  if (nudge < -kMaxNudge) nudge = -kMaxNudge;
  rateQ24_ = static_cast<uint32_t>(nominal * (1.0 + nudge) * kUnity + 0.5);
  return rateQ24_;
}

void LoopLockDriver::service(uint64_t nowUs, ClockEngine &clock, Looper &looper, bool clockFollows, bool restartOnStart,
                             double beatPulses, Looper::ReleaseFn release, void *context) {
  if (nowUs - lastUs_ < 10000ULL && nowUs >= lastUs_) return; // every 10 ms is plenty
  lastUs_ = nowUs;
  const LoopTrackState &track = looper.track(0);
  const bool haveLoop = looper.playing() && track.count && !track.hidden && track.lengthUs;
  const bool resetSeen = clock.phaseResetCount() != lastResets_;
  const bool startSeen = clock.startCount() != lastStarts_;
  lastResets_ = clock.phaseResetCount();
  lastStarts_ = clock.startCount();
  if (!haveLoop || !clockFollows) {
    if (lock_.engaged()) {
      lock_.reset();
      looper.setTimeWarp(nowUs, LoopLock::kUnity);
    }
    return;
  }
  const bool live = clock.followingLive(nowUs);
  if (!lock_.engaged() && !live) return; // the lock starts on a clock that is arriving

  // A Start from the clock: the loop plays again from its top if the setting says so.
  if (startSeen && restartOnStart) looper.restartTrackAtTop(nowUs, release, context);

  LoopLockInput in;
  in.loopNowUs = looper.virtualNow(nowUs);
  in.cycleStartUs = looper.track(0).cycleStartUs;
  in.lengthUs = looper.track(0).lengthUs;
  in.pulsePos = clock.pulsePosition(nowUs);
  double pulseUs = clock.measuredPulseUs();
  if (pulseUs <= 0.0) pulseUs = 60000000.0 / (static_cast<double>(clock.bpm()) * kInternalPpqn);
  in.pulseUs = pulseUs;
  in.beatPulses = beatPulses;
  in.clockUsable = live ? clock.externalClockPresent(nowUs) : true;
  if (resetSeen && lock_.engaged()) lock_.rebase(in); // the count started over, the loop stays where it is
  looper.setTimeWarp(nowUs, lock_.update(in));
}

}  // namespace fw
