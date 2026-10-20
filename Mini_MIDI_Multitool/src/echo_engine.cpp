#include "echo_engine.h"

namespace fw {

namespace {

constexpr uint32_t kHalfFactorsQ16[16] = {
  32768, 46341, 52016, 55109, 57052, 58386, 59358, 60097,
  60678, 61147, 61534, 61858, 62133, 62370, 62576, 62757
};
constexpr uint32_t kDoubleFactorsQ16[16] = {
  131072, 92682, 82570, 77936, 75281, 73562, 72358, 71468,
  70783, 70240, 69799, 69433, 69125, 68862, 68635, 68438
};

}  // namespace

uint32_t EchoEngine::driftFactorQ16(int8_t drift) const {
  if (drift == 0) return 65536;
  const uint8_t magnitude = static_cast<uint8_t>(drift > 0 ? drift : -drift);
  const uint8_t index = magnitude > 16 ? 15 : magnitude - 1U;
  return drift > 0 ? kHalfFactorsQ16[index] : kDoubleFactorsQ16[index];
}

void EchoEngine::emitOff(ActiveNote &note, EmitFn emit, void *context) {
  if (note.used && emit) {
    emit(context, note.target,
         EchoEvent{0, static_cast<uint8_t>(0x80 | note.channel), note.note, 0});
  }
  note = ActiveNote{};
}

void EchoEngine::reset(EmitFn emit, void *context) {
  for (ActiveNote &note : activeNotes_) emitOff(note, emit, context);
  for (Sequence &sequence : sequences_) sequence = Sequence{};
  nextSequenceId_ = 1;
  overflowCount_ = 0;
}

EchoEngine::Sequence *EchoEngine::allocateSequence() {
  reclaimSequences();
  for (Sequence &sequence : sequences_) {
    if (!sequence.used) return &sequence;
  }
  ++overflowCount_;
  // FoxWizard 1.65: full, the oldest echo gives way, so a new note always echoes.
  Sequence *oldest = nullptr;
  for (Sequence &sequence : sequences_) {
    if (!oldest || sequence.sourceOnUs < oldest->sourceOnUs) oldest = &sequence;
  }
  for (ActiveNote &note : activeNotes_) {
    if (note.used && note.sequenceId == oldest->id) note.offUs = 0; // let go on the next tick
  }
  oldest->scheduling = false;
  return oldest;
}

EchoEngine::ActiveNote *EchoEngine::allocateActiveNote() {
  for (ActiveNote &note : activeNotes_) {
    if (!note.used) return &note;
  }
  ++overflowCount_;
  return nullptr;
}

bool EchoEngine::sequenceHasActiveNotes(uint16_t sequenceId) const {
  for (const ActiveNote &note : activeNotes_) {
    if (note.used && note.sequenceId == sequenceId) return true;
  }
  return false;
}

void EchoEngine::reclaimSequences() {
  for (Sequence &sequence : sequences_) {
    // FoxWizard 1.65: a finished echo frees its slot even while its key is still held, so long-held
    // notes and unmatched Note Offs can't fill the pool.
    if (sequence.used && !sequence.scheduling && !sequenceHasActiveNotes(sequence.id)) {
      sequence = Sequence{};
    }
  }
}

void EchoEngine::noteOn(uint64_t nowUs, uint8_t target, uint8_t status,
                        uint8_t note, uint8_t velocity, const EchoConfig &config) {
  if (velocity == 0 || config.wetPercent == 0 || config.delayUs == 0 ||
      config.lengthUs <= config.delayUs) return;
  Sequence *sequence = allocateSequence();
  if (!sequence) return;
  *sequence = Sequence{};
  sequence->sourceOnUs = nowUs;
  sequence->firstRepeatUs = nowUs + config.delayUs;
  sequence->endUs = nowUs + config.lengthUs;
  sequence->nextOnUs = sequence->firstRepeatUs;
  sequence->spacingUs = config.delayUs;
  sequence->driftFactorQ16 = config.spacingFactorQ16 ? config.spacingFactorQ16 : driftFactorQ16(config.drift);
  sequence->id = nextSequenceId_++;
  if (nextSequenceId_ == 0) nextSequenceId_ = 1;
  sequence->target = target;
  sequence->channel = status & 0x0F;
  sequence->note = note;
  sequence->firstVelocity = static_cast<uint8_t>(
      (static_cast<uint16_t>(velocity) * config.wetPercent + 50U) / 100U);
  sequence->velocityDecayQ16 = config.velocityDecayQ16;
  sequence->repeatLimit = config.repeats;
  sequence->maxSpacingUs = config.maxSpacingUs;
  if (config.repeats) sequence->endUs = UINT64_MAX; // the repeat count ends it
  sequence->velocityQ16 = static_cast<uint32_t>(sequence->firstVelocity) << 16;
  sequence->used = true;
  sequence->scheduling = sequence->firstVelocity > 0;
}

void EchoEngine::noteOff(uint64_t nowUs, uint8_t target, uint8_t status, uint8_t note) {
  Sequence *newest = nullptr;
  const uint8_t channel = status & 0x0F;
  for (Sequence &sequence : sequences_) {
    if (!sequence.used || sequence.sourceReleased || sequence.target != target ||
        sequence.channel != channel || sequence.note != note) continue;
    if (!newest || static_cast<uint16_t>(sequence.id - newest->id) < 0x8000U) newest = &sequence;
  }
  if (!newest) return;
  newest->sourceReleased = true;
  const uint64_t duration = nowUs > newest->sourceOnUs ? nowUs - newest->sourceOnUs : 1000U;
  newest->sourceDurationUs = static_cast<uint32_t>(duration > UINT32_MAX ? UINT32_MAX : duration);
  for (ActiveNote &active : activeNotes_) {
    if (!active.used || active.sequenceId != newest->id) continue;
    const uint32_t gate = newest->sourceDurationUs < active.maxGateUs
        ? newest->sourceDurationUs : active.maxGateUs;
    active.offUs = active.offUs - active.maxGateUs + (gate ? gate : 1000U);
  }
}

void EchoEngine::tick(uint64_t nowUs, EmitFn emit, void *context, uint16_t maxWork) {
  uint16_t work = 0;
  for (ActiveNote &note : activeNotes_) {
    if (work >= maxWork) break;
    if (!note.used || nowUs < note.offUs) continue;
    emitOff(note, emit, context);
    ++work;
  }

  for (Sequence &sequence : sequences_) {
    if (work >= maxWork) break;
    while (sequence.used && sequence.scheduling && nowUs >= sequence.nextOnUs &&
           work < maxWork) {
      if (sequence.nextOnUs >= sequence.endUs || sequence.repeatCount >= kMaxRepeats ||
          (sequence.repeatLimit && sequence.repeatCount >= sequence.repeatLimit)) {
        sequence.scheduling = false;
        break;
      }
      const uint64_t fadeSpan = sequence.endUs - sequence.firstRepeatUs;
      const uint64_t fadeElapsed = sequence.nextOnUs - sequence.firstRepeatUs;
      uint8_t velocity = sequence.velocityDecayQ16
          ? static_cast<uint8_t>((sequence.velocityQ16 + 32768U) >> 16)
          : sequence.repeatLimit
          // Even fade by repeat: the first at full level, each after a step down, the last still heard.
          ? static_cast<uint8_t>((static_cast<uint16_t>(sequence.firstVelocity) *
                (sequence.repeatLimit - sequence.repeatCount) + sequence.repeatLimit / 2) / sequence.repeatLimit)
          : (fadeSpan == 0 ? 0 : static_cast<uint8_t>(
              (static_cast<uint64_t>(sequence.firstVelocity) * (fadeSpan - fadeElapsed)) / fadeSpan));
      if (sequence.repeatLimit && velocity == 0) velocity = 1;
      if (velocity == 0) {
        sequence.scheduling = false;
        break;
      }
      ActiveNote *active = allocateActiveNote();
      if (!active) {
        sequence.scheduling = false;
        break;
      }
      const uint32_t maxGate = sequence.spacingUs > 1333U
          ? (sequence.spacingUs * 3U) / 4U : 1000U;
      const uint32_t gate = sequence.sourceDurationUs
          ? (sequence.sourceDurationUs < maxGate ? sequence.sourceDurationUs : maxGate)
          : maxGate;
      *active = ActiveNote{sequence.nextOnUs + (gate ? gate : 1000U), maxGate,
                           sequence.id, sequence.target, sequence.channel,
                           sequence.note, true};
      if (emit) {
        emit(context, sequence.target,
             EchoEvent{0, static_cast<uint8_t>(0x90 | sequence.channel),
                           sequence.note, velocity});
      }
      ++sequence.repeatCount;
      ++work;
      if (sequence.velocityDecayQ16) {
        sequence.velocityQ16 = static_cast<uint32_t>(
            (static_cast<uint64_t>(sequence.velocityQ16) * sequence.velocityDecayQ16) >> 16);
      }
      const uint64_t nextSpacing =
          (static_cast<uint64_t>(sequence.spacingUs) * sequence.driftFactorQ16 + 32768U) >> 16;
      sequence.spacingUs = static_cast<uint32_t>(nextSpacing < 2000U ? 2000U :
          (nextSpacing > UINT32_MAX ? UINT32_MAX : nextSpacing));
      if (sequence.maxSpacingUs && sequence.spacingUs > sequence.maxSpacingUs) sequence.spacingUs = sequence.maxSpacingUs;
      sequence.nextOnUs += sequence.spacingUs;
    }
  }
  reclaimSequences();
}

void EchoEngine::stopTarget(uint8_t target, EmitFn emit, void *context) {
  for (Sequence &sequence : sequences_) {
    if (sequence.used && sequence.target == target) sequence = Sequence{};
  }
  for (ActiveNote &note : activeNotes_) {
    if (note.used && note.target == target) emitOff(note, emit, context);
  }
}

uint8_t EchoEngine::activeSequenceCount() const {
  uint8_t count = 0;
  for (const Sequence &sequence : sequences_) if (sequence.used) ++count;
  return count;
}

}  // namespace fw
