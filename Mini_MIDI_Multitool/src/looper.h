#pragma once

// Ported from ARPnMIDI (arpnmidi os/arpnmidi_main_brain/src/four_track_looper.*). Changes from
// there: one track (the FoxWizard button drives one loop), a 6144-event pool in 12-byte slots, and
// each event carries a tag, where it plays back to.

#include <stdint.h>

namespace fw {

constexpr uint8_t kLoopTrackCount = 1;
constexpr uint16_t kLoopEventPoolSize = 6144;
constexpr uint16_t kNoLoopEvent = 0xFFFF;

enum class LoopTrackMode : uint8_t {
  Layers = 0,
  PartsAutoSolo,
  Manual
};

struct LoopMidiEvent {
  uint32_t atUs = 0;
  uint8_t status = 0;
  uint8_t data1 = 0;
  uint8_t data2 = 0;
  uint16_t tag = 0; // FoxWizard: where it plays back to, set by the sketch
  uint8_t layer = 0; // FoxWizard 1.60: the recording pass it came from, 0 the take itself
};

struct LoopTrackState {
  uint16_t head = kNoLoopEvent;
  uint16_t tail = kNoLoopEvent;
  uint16_t playCursor = kNoLoopEvent;
  uint16_t count = 0;
  uint32_t lengthUs = 0;
  uint32_t storedLengthUs = 0;
  uint32_t generation = 0;
  uint64_t cycleStartUs = 0;
  uint32_t pausedPositionUs = 0;
  // Where this track's own cycle begins inside the shared transport cycle. A
  // layer that started partway through the others keeps that relationship
  // across stop, clear, undo, and play again.
  uint32_t startOffsetUs = 0;
  bool muted = false;
  bool solo = false;
  bool hidden = false;
};

class Looper {
 public:
  using EmitFn = void (*)(void *context, uint8_t track, const LoopMidiEvent &event);
  using ReleaseFn = void (*)(void *context, uint8_t track);
  using VisitFn = bool (*)(void *context, uint8_t track, const LoopMidiEvent &event);
  // FoxWizard 1.66, Note Delete: asked about each Note On as playback reaches it. True erases that
  // note from the loop, its Note Off too, and nothing sounds.
  using EraseFn = bool (*)(void *context, const LoopMidiEvent &event);
  // FoxWizard 1.87: layer is the pass the hold came from, so the caller can keep each layer's hold apart.
  using HeldNoteFn = void (*)(void *context, uint8_t track, uint16_t tag, uint8_t channel,
                              uint8_t note, uint8_t velocity, uint8_t layer);

  Looper();
  void reset();

  void setTrackMode(LoopTrackMode mode) { mode_ = mode; }
  LoopTrackMode trackMode() const { return mode_; }
  void selectTrack(uint8_t track);
  uint8_t selectedTrack() const { return selectedTrack_; }

  bool trackHasContent(uint8_t track) const;
  void armRecord(uint8_t track, uint32_t fixedLengthUs, bool overdub);
  bool beginArmedRecording(uint64_t nowUs);
  void setRecordQuantizeUs(uint32_t quantumUs) { recordQuantizeUs_ = quantumUs; }
  bool capture(uint64_t nowUs, const LoopMidiEvent &event);
  bool finishRecording(uint64_t nowUs);
  void cancelRecording();
  bool recordingArmed() const { return recordingArmed_; }
  bool recording() const { return recording_; }
  bool overdubbing() const { return overdubbing_; }
  uint8_t recordingTrack() const { return recordingTrack_; }

  void start(uint64_t nowUs);
  void pause(uint64_t nowUs, ReleaseFn release, void *context);
  void resume(uint64_t nowUs);
  void stop(ReleaseFn release, void *context);
  bool resizeTrack(uint8_t track, uint32_t lengthUs, uint64_t nowUs,
                   ReleaseFn release, void *context);
  void tick(uint64_t nowUs, EmitFn emit, ReleaseFn release, void *context,
            uint16_t maxEvents = 96);
  bool playing() const { return playing_; }
  bool paused() const { return paused_; } // FoxWizard: Pause resumes where it stopped

  // FoxWizard 1.60, overdub layers. Every overdub pass is its own layer. Un-Overdub hides the
  // newest one still showing (ending a pass in progress first, which becomes that layer), and
  // Re-Overdub shows the one hidden most recently. Hidden layers stay until memory runs short or
  // they fall past the last kMaxUndoLayers, so a layer undone before a newer overdub can still come
  // back. The take itself (layer 0) is only ever cleared, never un-overdubbed.
  static constexpr uint8_t kMaxUndoLayers = 16;
  void setEraseHook(EraseFn fn, void *context) { eraseFn_ = fn; eraseContext_ = context; }
  // FoxWizard 1.73: a take's set length can change while it's armed or taking.
  uint32_t recordFixedLengthUs() const { return recordFixedLengthUs_; }
  void setRecordFixedLengthUs(uint32_t us) { recordFixedLengthUs_ = us; }
  // Whether Un-Overdub or Re-Overdub would do something, for a button's light.
  bool canUnOverdub() const {
    for (uint8_t i = 0; i < layerCount_; ++i) if (!layerHidden(layers_[i])) return true;
    return false;
  }
  bool canReOverdub() const { return hiddenCount_ > 0; }
  bool unOverdub(uint64_t nowUs);
  bool reOverdub();
  bool layerHidden(uint8_t layer) const { return layerHiddenBits_[layer >> 3] & (1U << (layer & 7)); }
  uint16_t visibleCount() const; // events not in a hidden layer: what a save keeps
  uint32_t layerStamp() const { return layerStamp_; } // changes whenever a layer hides or shows

  void safeClear(uint8_t track, ReleaseFn release, void *context);
  void undoClear(uint8_t track);
  void permanentlyClear(uint8_t track);
  void clearAll(ReleaseFn release, void *context);
  void setMuted(uint8_t track, bool muted, ReleaseFn release, void *context);
  void setSolo(uint8_t track, bool solo, ReleaseFn release, void *context);
  bool audible(uint8_t track) const;

  // A track's cursor keeps advancing through its own recorded events every
  // tick regardless of audibility: muted, soloed out, or hidden all still let
  // time pass inside that track's data. This replays everything the cursor
  // has already stepped past in the current cycle and reports which notes
  // are still sounding as of right now, so a track regaining audibility can
  // retrigger exactly what it should already be playing instead of only
  // picking up whatever note-on happens to come next.
  void collectHeldNotes(uint8_t track, HeldNoteFn fn, void *context) const;

  const LoopTrackState &track(uint8_t index) const { return tracks_[index]; }
  uint16_t usedEvents() const { return usedEvents_; }
  uint16_t freeEvents() const { return kLoopEventPoolSize - usedEvents_; }
  uint32_t overflowCount() const { return overflowCount_; }
  bool hasAnyData() const;
  uint8_t oldestPopulatedTrack() const;
  uint8_t newestPopulatedTrack() const;
  bool visitEvents(VisitFn visitor, void *context) const;
  bool restoreEvent(uint8_t track, const LoopMidiEvent &event);
  void setRestoredTrackState(uint8_t track, uint32_t lengthUs,
                             uint32_t storedLengthUs, uint32_t generation,
                             bool muted, bool solo, bool hidden,
                             uint32_t startOffsetUs = 0);
  void beginImport(uint8_t track, uint32_t lengthUs, ReleaseFn release, void *context);
  bool importEvent(uint8_t track, const LoopMidiEvent &event);
  void finishImport(uint8_t track, uint64_t boundaryUs);

 private:
  // FoxWizard: the event's fields and the list link side by side, 12 bytes a slot, not 16.
  struct EventSlot {
    uint32_t atUs = 0;
    uint16_t tag = 0;
    uint16_t next = kNoLoopEvent;
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
    uint8_t layer = 0; // the slot's one spare byte, so it stays 12
    LoopMidiEvent event() const { return LoopMidiEvent{atUs, status, data1, data2, tag, layer}; }
    void setEvent(const LoopMidiEvent &e) { atUs = e.atUs; status = e.status; data1 = e.data1; data2 = e.data2; tag = e.tag; layer = e.layer; }
  };
  static_assert(sizeof(EventSlot) == 12, "a loop event slot is 12 bytes");

  uint16_t allocateSlot();
  void freeSlot(uint16_t slot);
  bool insertEvent(uint8_t track, const LoopMidiEvent &event);
  void captureTrackPhase(uint8_t track);
  void seekTrackTo(LoopTrackState &track, uint64_t nowUs, uint32_t positionUs,
                   bool skipEventsAtPosition);
  void releaseIfAudibilityChanged(uint8_t track, bool wasAudible,
                                  ReleaseFn release, void *context);
  void resetPlaybackCursors(uint64_t nowUs);
  void closeHeldRecordingNotes(uint32_t boundaryUs);
  // FoxWizard 1.80: an overdub records a held note as one finished hold, put in when it's let go.
  struct PendingHold {
    bool used = false;
    uint8_t channel = 0, note = 0, velocity = 0, layer = 0;
    uint16_t tag = 0;
    uint32_t positionUs = 0;  // where in the loop it was pressed
    uint32_t startUs = 0;     // when, the low 32 bits of the clock, for telling how long it lasted
  };
  static constexpr uint8_t kHoldCount = 32;
  bool startHold(uint8_t channel, uint8_t note, uint8_t velocity, uint16_t tag, uint32_t positionUs, uint64_t nowUs);
  bool endHold(uint8_t channel, uint8_t note, uint32_t positionUs, uint64_t nowUs);
  void flushHold(PendingHold &hold, uint32_t endPositionUs, uint64_t nowUs);
  void flushAllHolds(uint32_t endPositionUs, uint64_t nowUs);
  void unlinkSlot(uint8_t track, uint16_t slot);
  uint16_t eraseNote(uint8_t track, uint16_t onSlot);
  bool layerHasEvents(uint8_t layer) const;
  void resetLayers();
  void startOverdubLayer();
  void forgetLayer(uint8_t layer);
  void deleteLayer(uint8_t layer);
  void setLayerHidden(uint8_t layer, bool hidden);
  bool dropOldestHiddenLayer();

  EventSlot slots_[kLoopEventPoolSize]{};
  LoopTrackState tracks_[kLoopTrackCount]{};
  uint16_t freeHead_ = kNoLoopEvent;
  uint16_t usedEvents_ = 0;
  uint32_t overflowCount_ = 0;
  uint32_t generationCounter_ = 0;
  uint64_t transportStartUs_ = 0;
  uint64_t recordStartUs_ = 0;
  uint32_t recordFixedLengthUs_ = 0;
  uint32_t recordQuantizeUs_ = 0;
  uint8_t selectedTrack_ = 0;
  uint8_t recordingTrack_ = 0;
  LoopTrackMode mode_ = LoopTrackMode::Manual;
  bool playing_ = false;
  bool paused_ = false;
  bool recordingArmed_ = false;
  bool recording_ = false;
  bool overdubbing_ = false;
  // The track's own event count at the instant this pass actually started
  // recording, so finishRecording can tell whether THIS pass added anything.
  // A plain count-is-nonzero check is right for a replacement, which always
  // starts from zero, but wrong for an overdub: the track already had
  // content before the pass began, so that check is always true regardless
  // of whether the pass captured a single new event.
  uint16_t recordStartCount_ = 0;
  bool importing_[kLoopTrackCount]{};
  uint16_t recordHeld_[16][128]{}; // FoxWizard: the held note's tag + 1, 0 not held
  PendingHold holds_[kHoldCount];
  uint8_t layers_[kMaxUndoLayers]{};  // overdub layers, oldest first, hidden ones included
  uint8_t layerCount_ = 0;
  uint8_t hidden_[kMaxUndoLayers]{};  // hidden layers in the order they were hidden
  uint8_t hiddenCount_ = 0;
  uint8_t layerHiddenBits_[32]{};
  uint8_t currentLayer_ = 0;          // the layer the pass being recorded writes
  uint32_t layerStamp_ = 0;
  EraseFn eraseFn_ = nullptr;
  void *eraseContext_ = nullptr;
};

}  // namespace fw
