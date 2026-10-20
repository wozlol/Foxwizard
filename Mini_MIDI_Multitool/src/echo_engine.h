#pragma once

// Ported from ARPnMIDI (arpnmidi os/arpnmidi_main_brain/src/echo_engine.*). Change from there:
// its LoopMidiEvent is EchoEvent here.

#include <stdint.h>


namespace fw {

struct EchoEvent {
  uint32_t atUs = 0;
  uint8_t status = 0;
  uint8_t data1 = 0;
  uint8_t data2 = 0;
};

struct EchoConfig {
  uint32_t lengthUs = 0;
  uint32_t delayUs = 0;
  uint8_t wetPercent = 50;
  int8_t drift = 0;
  // FoxWizard 1.59: each repeat's velocity times this (Q16) instead of the even fade over the
  // echo's length, the way a bouncing ball loses the same share of its energy every bounce. 0 off.
  uint32_t velocityDecayQ16 = 0;
  // FoxWizard 1.64: each gap is the last one times this (Q16), used instead of drift. 0 off.
  uint32_t spacingFactorQ16 = 0;
  // FoxWizard 1.65: plays exactly this many repeats, fading evenly by repeat rather than by time,
  // and no gap grows past maxSpacingUs. 0 keeps the time-based length and fade.
  uint8_t repeats = 0;
  uint32_t maxSpacingUs = 0;
};

class EchoEngine {
 public:
  using EmitFn = void (*)(void *context, uint8_t target, const EchoEvent &event);

  void reset(EmitFn emit = nullptr, void *context = nullptr);
  void noteOn(uint64_t nowUs, uint8_t target, uint8_t status,
              uint8_t note, uint8_t velocity, const EchoConfig &config);
  void noteOff(uint64_t nowUs, uint8_t target, uint8_t status, uint8_t note);
  void tick(uint64_t nowUs, EmitFn emit, void *context, uint16_t maxWork = 48);
  void stopTarget(uint8_t target, EmitFn emit, void *context);

  uint32_t overflowCount() const { return overflowCount_; }
  uint8_t activeSequenceCount() const;

 private:
  static constexpr uint8_t kSequenceCount = 128;   // FoxWizard 1.65: was 64, fast arps filled it
  static constexpr uint8_t kActiveNoteCount = 160;
  static constexpr uint8_t kMaxRepeats = 64;

  struct Sequence {
    uint64_t sourceOnUs = 0;
    uint64_t firstRepeatUs = 0;
    uint64_t endUs = 0;
    uint64_t nextOnUs = 0;
    uint32_t spacingUs = 0;
    uint32_t sourceDurationUs = 0;
    uint32_t driftFactorQ16 = 65536;
    uint32_t velocityQ16 = 0;
    uint32_t velocityDecayQ16 = 0;
    uint32_t maxSpacingUs = 0;
    uint16_t id = 0;
    uint8_t repeatLimit = 0;
    uint8_t target = 0;
    uint8_t channel = 0;
    uint8_t note = 0;
    uint8_t firstVelocity = 0;
    uint8_t repeatCount = 0;
    bool used = false;
    bool scheduling = false;
    bool sourceReleased = false;
  };

  struct ActiveNote {
    uint64_t offUs = 0;
    uint32_t maxGateUs = 0;
    uint16_t sequenceId = 0;
    uint8_t target = 0;
    uint8_t channel = 0;
    uint8_t note = 0;
    bool used = false;
  };

  uint32_t driftFactorQ16(int8_t drift) const;
  Sequence *allocateSequence();
  ActiveNote *allocateActiveNote();
  bool sequenceHasActiveNotes(uint16_t sequenceId) const;
  void reclaimSequences();
  void emitOff(ActiveNote &note, EmitFn emit, void *context);

  Sequence sequences_[kSequenceCount]{};
  ActiveNote activeNotes_[kActiveNoteCount]{};
  uint16_t nextSequenceId_ = 1;
  uint32_t overflowCount_ = 0;
};

}  // namespace fw
