#include "clock_engine.h"

namespace fw {
namespace {

constexpr uint8_t kMidiClockStatus = 0xF8;
constexpr uint8_t kMidiStartStatus = 0xFA;
constexpr uint8_t kMidiContinueStatus = 0xFB;
constexpr uint8_t kMidiStopStatus = 0xFC;
constexpr uint64_t kMinimumExternalTimeoutUs = 500000ULL;
constexpr uint8_t kMissingPulseLimit = 6;

uint16_t clampBpm(uint16_t bpm) {
  if (bpm < 20) return 20;
  if (bpm > 300) return 300;
  return bpm;
}

}  // namespace

void ClockEngine::begin(const ClockConfig &config, uint64_t nowUs) {
  config_ = config;
  config_.manualBpm = clampBpm(config_.manualBpm);
  running_ = true;
  haveExternalClock_ = false;
  filteredExternalIntervalUs_ = 0;
  lastExternalClockUs_ = 0;
  resetPhase(nowUs);
}

void ClockEngine::setConfig(const ClockConfig &config, uint64_t nowUs) {
  const bool timingSourceChanged = config.followExternal != config_.followExternal || config.autoLead != config_.autoLead;
  const bool tempoChanged = clampBpm(config.manualBpm) != config_.manualBpm;
  config_ = config;
  config_.manualBpm = clampBpm(config_.manualBpm);
  if (timingSourceChanged) {
    leading_ = false;
    sourceStopped_ = false;
    leadBpm_ = 0;
  } else if (tempoChanged && leading_) {
    leadBpm_ = config_.manualBpm; // a tempo set by hand while Auto leads is the tempo it leads at
    filteredExternalIntervalUs_ = 0;
  }
  if (timingSourceChanged || tempoChanged) resetPhase(nowUs);
}

TransportEvent ClockEngine::receiveRealtime(uint8_t status, uint64_t nowUs) {
  switch (status) {
    case kMidiClockStatus:
      receiveClock(nowUs);
      return TransportEvent::None;
    case kMidiStartStatus:
      if (config_.followExternal) {
        sourceStopped_ = false;
        leading_ = false;
        graceUntilUs_ = nowUs + kMinimumExternalTimeoutUs;
        ++startCount_;
        start(nowUs);
      }
      return TransportEvent::Start;
    case kMidiContinueStatus:
      if (config_.followExternal) {
        if (config_.autoLead) {
          sourceStopped_ = false;
          leading_ = false;
          graceUntilUs_ = nowUs + kMinimumExternalTimeoutUs;
        } else {
          resume(nowUs);
        }
      }
      return TransportEvent::Continue;
    case kMidiStopStatus:
      if (config_.followExternal) {
        if (config_.autoLead) sourceStopped_ = true; // Auto carries on with its own clock
        else stop();
      }
      return TransportEvent::Stop;
    default:
      return TransportEvent::None;
  }
}

void ClockEngine::receiveClock(uint64_t nowUs) {
  if (lastExternalClockUs_ != 0 && nowUs > lastExternalClockUs_) {
    const uint64_t interval = nowUs - lastExternalClockUs_;
    // Reject impossible clock intervals before they disturb the smoothing.
    if (interval >= 8000ULL && interval <= 150000ULL) {
      if (filteredExternalIntervalUs_ == 0) filteredExternalIntervalUs_ = interval;
      else filteredExternalIntervalUs_ =
          ((filteredExternalIntervalUs_ * 7ULL) + interval + 4ULL) / 8ULL;
    }
  }
  lastExternalClockUs_ = nowUs;
  haveExternalClock_ = true;
  if (config_.autoLead && sourceStopped_) return; // stopped: the source's clock bytes don't count
  leading_ = false;                               // Auto: the external clock is back and drives the count
  if (config_.followExternal && running_) {
    internalPulse_ += kInternalPulsesPerMidiClock;
    // A sample of the count every 250 ms, for a tempo that jitter doesn't move.
    if (tempoSampleCount_ == 0 || nowUs - tempoSamples_[tempoSampleCount_ - 1].timeUs >= 250000ULL) {
      if (tempoSampleCount_ == kTempoSamples) {
        for (uint8_t i = 1; i < kTempoSamples; ++i) tempoSamples_[i - 1] = tempoSamples_[i];
        --tempoSampleCount_;
      }
      tempoSamples_[tempoSampleCount_++] = TempoSample{nowUs, internalPulse_};
    }
  }
}

double ClockEngine::measuredPulseUs() const {
  if (tempoSampleCount_ < 2) return 0.0;
  const TempoSample &a = tempoSamples_[0];
  const TempoSample &b = tempoSamples_[tempoSampleCount_ - 1];
  if (b.timeUs - a.timeUs < 1000000ULL || b.count <= a.count) return 0.0;
  return static_cast<double>(b.timeUs - a.timeUs) / static_cast<double>(b.count - a.count);
}

double ClockEngine::pulsePosition(uint64_t nowUs) const {
  double fraction = 0.0;
  if (config_.followExternal && !leading_) {
    if (filteredExternalIntervalUs_ != 0 && haveExternalClock_ && nowUs > lastExternalClockUs_) {
      fraction = static_cast<double>(nowUs - lastExternalClockUs_) / static_cast<double>(filteredExternalIntervalUs_);
    }
  } else if (nextInternalClockUs_ != 0 && nextInternalClockUs_ > nowUs) {
    const uint64_t interval = midiClockIntervalUs();
    if (interval) fraction = 1.0 - static_cast<double>(nextInternalClockUs_ - nowUs) / static_cast<double>(interval);
  }
  if (fraction < 0.0) fraction = 0.0;
  if (fraction > 1.0) fraction = 1.0;
  return static_cast<double>(internalPulse_) + fraction * kInternalPulsesPerMidiClock;
}

bool ClockEngine::followingLive(uint64_t nowUs) {
  if (!config_.followExternal) return false;
  if (!config_.autoLead) return true;
  const bool live = externalClockPresent(nowUs) && !sourceStopped_;
  if (!live && !leading_) {
    // The source went quiet or stopped: carry on from this count with its own clock, at the tempo it had.
    leading_ = true;
    const double longPulseUs = measuredPulseUs();
    leadBpm_ = longPulseUs > 0.0
        ? static_cast<float>(60000000.0 / (longPulseUs * kInternalPpqn))
        : (filteredExternalIntervalUs_ != 0
               ? 60000000.0f / (static_cast<float>(filteredExternalIntervalUs_) * kMidiClockPpqn)
               : static_cast<float>(config_.manualBpm));
    const uint64_t interval = midiClockIntervalUs();
    if (!sourceStopped_ && haveExternalClock_ && interval != 0 && nowUs > lastExternalClockUs_) {
      // The clock went quiet some time ago, a quarter of a second or more passes before it's noticed. The beats in
      // between are counted, as if it had carried on, and the next one is due where it would have been.
      const uint64_t missed = (nowUs - lastExternalClockUs_) / interval;
      internalPulse_ += static_cast<uint32_t>(missed) * kInternalPulsesPerMidiClock;
      nextInternalClockUs_ = lastExternalClockUs_ + (missed + 1) * interval;
    } else {
      nextInternalClockUs_ = nowUs + interval;
    }
  } else if (live && leading_) {
    leading_ = false;
  }
  return !leading_;
}

bool ClockEngine::takeInternalClock(uint64_t nowUs) {
  if (!config_.sendClock || !running_) return false;
  if (config_.followExternal && followingLive(nowUs)) return false;
  if (nextInternalClockUs_ == 0) nextInternalClockUs_ = nowUs;
  if (nowUs < nextInternalClockUs_) return false;

  const uint64_t interval = midiClockIntervalUs();
  internalPulse_ += kInternalPulsesPerMidiClock;
  nextInternalClockUs_ += interval;

  // Bound catch-up work after a stall. Musical scheduling may report the stall,
  // but it must not flood the MIDI output with an unbounded clock burst.
  // Only a real stall counts: the next tick is behind now by more than four intervals. (This was an unsigned subtraction
  // that wrapped whenever the next tick was still ahead, so every tick re-anchored to the moment the main loop got
  // to it, and the clock ran slow by the loop's delay on every tick.)
  if (nowUs > nextInternalClockUs_ && nowUs - nextInternalClockUs_ > interval * 4ULL) {
    nextInternalClockUs_ = nowUs + interval;
  }
  return true;
}

void ClockEngine::start(uint64_t nowUs) {
  running_ = true;
  resetPhase(nowUs);
}

void ClockEngine::resume(uint64_t nowUs) {
  if (running_) return;
  running_ = true;
  nextInternalClockUs_ = nowUs + midiClockIntervalUs();
}

void ClockEngine::stop() {
  running_ = false;
  nextInternalClockUs_ = 0;
}

bool ClockEngine::externalClockPresent(uint64_t nowUs) const {
  if (nowUs < graceUntilUs_) return true;
  return haveExternalClock_ && nowUs >= lastExternalClockUs_ &&
         (nowUs - lastExternalClockUs_) <= externalTimeoutUs();
}

bool ClockEngine::synchronizedAdvanceAllowed(uint64_t nowUs) const {
  if (!running_) return false;
  if (config_.autoLead) return true; // Auto always has a clock, the external one or its own
  return !config_.followExternal || externalClockPresent(nowUs);
}

float ClockEngine::bpm() const {
  if (leading_ && leadBpm_ > 0) return leadBpm_;
  if (config_.followExternal && filteredExternalIntervalUs_ != 0) {
    return 60000000.0f /
           (static_cast<float>(filteredExternalIntervalUs_) * kMidiClockPpqn);
  }
  return static_cast<float>(config_.manualBpm);
}

uint32_t ClockEngine::barPulse() const {
  const uint32_t pulses = barPulses(config_.beatsPerBar, config_.beatUnit);
  return pulses ? internalPulse_ % pulses : 0;
}

uint32_t ClockEngine::lastExternalClockAgeMs(uint64_t nowUs) const {
  if (!haveExternalClock_ || nowUs < lastExternalClockUs_) return UINT32_MAX;
  const uint64_t ageMs = (nowUs - lastExternalClockUs_) / 1000ULL;
  return ageMs > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(ageMs);
}

uint64_t ClockEngine::midiClockIntervalUs() const {
  const float bpm = leading_ && leadBpm_ > 0 ? leadBpm_ : static_cast<float>(config_.manualBpm);
  return quarterNoteUs(bpm) / kMidiClockPpqn;
}

uint64_t ClockEngine::externalTimeoutUs() const {
  const uint64_t pulseBased = filteredExternalIntervalUs_ * kMissingPulseLimit;
  return pulseBased > kMinimumExternalTimeoutUs ? pulseBased : kMinimumExternalTimeoutUs;
}

void ClockEngine::resetPhase(uint64_t nowUs) {
  ++phaseResets_;
  tempoSampleCount_ = 0;
  phaseOriginUs_ = nowUs;
  internalPulse_ = 0;
  nextInternalClockUs_ = nowUs + midiClockIntervalUs();
}

}  // namespace fw
