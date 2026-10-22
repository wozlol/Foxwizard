#include "looper.h"

#include <string.h>

namespace fw {

Looper::Looper() { reset(); }

void Looper::reset() {
  for (uint16_t i = 0; i < kLoopEventPoolSize; ++i) {
    slots_[i] = EventSlot{};
    slots_[i].next = (i + 1U < kLoopEventPoolSize) ? i + 1U : kNoLoopEvent;
  }
  for (LoopTrackState &track : tracks_) track = LoopTrackState{};
  freeHead_ = 0;
  usedEvents_ = 0;
  overflowCount_ = 0;
  generationCounter_ = 0;
  transportStartUs_ = 0;
  recordStartUs_ = 0;
  recordFixedLengthUs_ = 0;
  recordQuantizeUs_ = 0;
  selectedTrack_ = 0;
  recordingTrack_ = 0;
  mode_ = LoopTrackMode::Manual;
  playing_ = false;
  paused_ = false;
  recordingArmed_ = false;
  recording_ = false;
  overdubbing_ = false;
  recordStartCount_ = 0;
  memset(importing_, 0, sizeof(importing_));
  memset(recordHeld_, 0, sizeof(recordHeld_));
  for (PendingHold &h : holds_) h.used = false;
  resetLayers();
}

void Looper::resetLayers() {
  layerCount_ = hiddenCount_ = 0;
  memset(layerHiddenBits_, 0, sizeof(layerHiddenBits_));
  currentLayer_ = 0;
  ++layerStamp_;
}

void Looper::setLayerHidden(uint8_t layer, bool hidden) {
  if (hidden) layerHiddenBits_[layer >> 3] |= (1U << (layer & 7));
  else layerHiddenBits_[layer >> 3] &= ~(1U << (layer & 7));
  ++layerStamp_;
}

// Drops a layer from the bookkeeping, its events untouched.
void Looper::forgetLayer(uint8_t layer) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < layerCount_; ++i) if (layers_[i] != layer) layers_[n++] = layers_[i];
  layerCount_ = n;
  n = 0;
  for (uint8_t i = 0; i < hiddenCount_; ++i) if (hidden_[i] != layer) hidden_[n++] = hidden_[i];
  hiddenCount_ = n;
  setLayerHidden(layer, false);
}

// Removes a layer's events for good.
void Looper::deleteLayer(uint8_t layer) {
  LoopTrackState &track = tracks_[recordingTrack_];
  uint16_t previous = kNoLoopEvent;
  uint16_t slot = track.head;
  while (slot != kNoLoopEvent) {
    const uint16_t next = slots_[slot].next;
    if (slots_[slot].layer == layer) {
      if (previous == kNoLoopEvent) track.head = next;
      else slots_[previous].next = next;
      if (track.tail == slot) track.tail = previous;
      if (track.playCursor == slot) track.playCursor = next;
      freeSlot(slot);
      if (track.count) --track.count;
    } else {
      previous = slot;
    }
    slot = next;
  }
  forgetLayer(layer);
}

bool Looper::layerHasEvents(uint8_t layer) const {
  for (uint16_t slot = tracks_[recordingTrack_].head; slot != kNoLoopEvent; slot = slots_[slot].next) {
    if (slots_[slot].layer == layer) return true;
  }
  return false;
}

// Takes one event out of a track's list.
void Looper::unlinkSlot(uint8_t trackIndex, uint16_t slot) {
  LoopTrackState &track = tracks_[trackIndex];
  uint16_t previous = kNoLoopEvent;
  uint16_t s = track.head;
  while (s != kNoLoopEvent && s != slot) { previous = s; s = slots_[s].next; }
  if (s == kNoLoopEvent) return;
  const uint16_t next = slots_[slot].next;
  if (previous == kNoLoopEvent) track.head = next;
  else slots_[previous].next = next;
  if (track.tail == slot) track.tail = previous;
  if (track.playCursor == slot) track.playCursor = next;
  freeSlot(slot);
  if (track.count) --track.count;
}

// Erases a note: its Note On, and the Note Off that ends it (the next one for the same channel,
// note and tag, wrapping around the loop). Returns the event that followed the Note On.
uint16_t Looper::eraseNote(uint8_t trackIndex, uint16_t onSlot) {
  LoopTrackState &track = tracks_[trackIndex];
  const EventSlot on = slots_[onSlot];
  auto endsIt = [&](uint16_t s) {
    const EventSlot &e = slots_[s];
    const uint8_t type = e.status & 0xF0;
    return (type == 0x80 || (type == 0x90 && e.data2 == 0)) && (e.status & 0x0F) == (on.status & 0x0F) &&
           e.data1 == on.data1 && e.tag == on.tag && e.layer == on.layer;
  };
  uint16_t off = kNoLoopEvent;
  for (uint16_t s = on.next; s != kNoLoopEvent && off == kNoLoopEvent; s = slots_[s].next) if (endsIt(s)) off = s;
  for (uint16_t s = track.head; s != onSlot && s != kNoLoopEvent && off == kNoLoopEvent; s = slots_[s].next) if (endsIt(s)) off = s;
  uint16_t next = on.next;
  if (off == next && off != kNoLoopEvent) next = slots_[off].next;
  if (off != kNoLoopEvent) unlinkSlot(trackIndex, off);
  unlinkSlot(trackIndex, onSlot);
  return next;
}

// Memory full: the layer undone longest ago goes for good, so recording can carry on.
bool Looper::dropOldestHiddenLayer() {
  if (hiddenCount_ == 0) return false;
  deleteLayer(hidden_[0]);
  return true;
}

// A new overdub pass gets a layer of its own. Past kMaxUndoLayers the oldest becomes permanent:
// part of the take if it's showing, gone if it was undone.
void Looper::startOverdubLayer() {
  if (layerCount_ >= kMaxUndoLayers) {
    const uint8_t oldest = layers_[0];
    if (layerHidden(oldest)) {
      deleteLayer(oldest);
    } else {
      LoopTrackState &track = tracks_[recordingTrack_];
      for (uint16_t slot = track.head; slot != kNoLoopEvent; slot = slots_[slot].next) {
        if (slots_[slot].layer == oldest) slots_[slot].layer = 0;
      }
      forgetLayer(oldest);
    }
  }
  uint8_t id = 1;
  for (; id != 0; ++id) {
    bool used = false;
    for (uint8_t i = 0; i < layerCount_; ++i) used |= layers_[i] == id;
    if (!used) break;
  }
  if (id == 0) id = 1; // can't happen with 16 layers out of 255 ids
  layers_[layerCount_++] = id;
  currentLayer_ = id;
}

bool Looper::unOverdub(uint64_t nowUs) {
  if (recording_ && overdubbing_) finishRecording(nowUs); // the pass so far is the newest layer
  for (int8_t i = static_cast<int8_t>(layerCount_) - 1; i >= 0; --i) {
    const uint8_t layer = layers_[i];
    if (layerHidden(layer)) continue;
    setLayerHidden(layer, true);
    hidden_[hiddenCount_++] = layer;
    return true;
  }
  return false;
}

bool Looper::reOverdub() {
  if (hiddenCount_ == 0) return false;
  setLayerHidden(hidden_[--hiddenCount_], false);
  return true;
}

uint16_t Looper::visibleCount() const {
  uint16_t count = 0;
  for (uint16_t slot = tracks_[0].head; slot != kNoLoopEvent; slot = slots_[slot].next) {
    if (!layerHidden(slots_[slot].layer)) ++count;
  }
  return count;
}

uint16_t Looper::allocateSlot() {
  if (freeHead_ == kNoLoopEvent) {
    ++overflowCount_;
    return kNoLoopEvent;
  }
  const uint16_t slot = freeHead_;
  freeHead_ = slots_[slot].next;
  slots_[slot].next = kNoLoopEvent;
  ++usedEvents_;
  return slot;
}

void Looper::freeSlot(uint16_t slot) {
  if (slot == kNoLoopEvent || slot >= kLoopEventPoolSize) return;
  slots_[slot] = EventSlot{};
  slots_[slot].next = freeHead_;
  freeHead_ = slot;
  if (usedEvents_ > 0) --usedEvents_;
}

bool Looper::insertEvent(uint8_t trackIndex, const LoopMidiEvent &event) {
  if (trackIndex >= kLoopTrackCount) return false;
  const uint16_t slot = allocateSlot();
  if (slot == kNoLoopEvent) return false;
  slots_[slot].setEvent(event);
  LoopTrackState &track = tracks_[trackIndex];
  if (track.head == kNoLoopEvent) {
    track.head = track.tail = slot;
  } else if (slots_[track.tail].atUs <= event.atUs) {
    slots_[track.tail].next = slot;
    track.tail = slot;
  } else if (event.atUs < slots_[track.head].atUs) {
    slots_[slot].next = track.head;
    track.head = slot;
  } else {
    uint16_t previous = track.head;
    while (slots_[previous].next != kNoLoopEvent &&
           slots_[slots_[previous].next].atUs <= event.atUs) {
      previous = slots_[previous].next;
    }
    slots_[slot].next = slots_[previous].next;
    slots_[previous].next = slot;
  }
  ++track.count;
  return true;
}

void Looper::selectTrack(uint8_t track) {
  if (track < kLoopTrackCount) selectedTrack_ = track;
}

bool Looper::trackHasContent(uint8_t track) const {
  return track < kLoopTrackCount && tracks_[track].count > 0 && !tracks_[track].hidden;
}

void Looper::armRecord(uint8_t track, uint32_t fixedLengthUs, bool overdub) {
  if (track >= kLoopTrackCount) return;
  recordingTrack_ = track;
  selectedTrack_ = track;
  recordFixedLengthUs_ = fixedLengthUs;
  recordingArmed_ = !overdub;
  recording_ = overdub;
  overdubbing_ = overdub;
  recordStartUs_ = overdub ? transportStartUs_ : 0;
  // Overdub starts recording immediately, right here, rather than waiting for
  // a first note the way a replacement arm does, so its start count has to be
  // captured now too.
  if (overdub) recordStartCount_ = tracks_[track].count;
  if (overdub) startOverdubLayer();
  memset(recordHeld_, 0, sizeof(recordHeld_));
  for (PendingHold &h : holds_) h.used = false;
  // Arming stays non-destructive.  A cleared track keeps its undo material
  // until the first captured event actually replaces it, so the armed target
  // can still move to another track without losing anything.
  if (overdub) tracks_[track].generation = ++generationCounter_;
}

bool Looper::beginArmedRecording(uint64_t nowUs) {
  if (!recordingArmed_) return false;
  recordingArmed_ = false;
  recording_ = true;
  // The armed target may have moved onto a populated track after arming, so
  // the kind of pass is decided here rather than at arm time.  Audible content
  // is layered onto; empty and cleared tracks take a replacement recording.
  LoopTrackState &track = tracks_[recordingTrack_];
  overdubbing_ = playing_ && track.lengthUs > 0 && track.count > 0 && !track.hidden;
  if (overdubbing_) {
    recordStartUs_ = transportStartUs_;
    recordStartCount_ = track.count;
    startOverdubLayer();
  } else {
    recordStartUs_ = nowUs;
    permanentlyClear(recordingTrack_);
    recordStartCount_ = 0;
    currentLayer_ = 0;
  }
  track.generation = ++generationCounter_;
  return true;
}

bool Looper::capture(uint64_t nowUs, const LoopMidiEvent &input) {
  const uint8_t type = input.status & 0xF0;
  const bool noteOn = type == 0x90 && input.data2 > 0;
  if (recordingArmed_) {
    if (!noteOn) return false;
    beginArmedRecording(nowUs);
  }
  if (!recording_) return false;

  LoopTrackState &track = tracks_[recordingTrack_];
  uint32_t atUs = 0;
  if (overdubbing_ && playing_ && track.lengthUs > 0) {
    atUs = static_cast<uint32_t>((nowUs - track.cycleStartUs) % track.lengthUs);
  } else {
    const uint64_t elapsed = nowUs - recordStartUs_;
    if (recordFixedLengthUs_ > 0 && elapsed >= recordFixedLengthUs_) return false;
    atUs = static_cast<uint32_t>(elapsed);
  }
  // FoxWizard 1.80: a note an overdub holds is put in whole when it's let go, wherever the hold lands.
  // Put in at the press and the release as they come, a hold that crosses the top of the loop, or lasts
  // longer than the loop, ended up cutting itself off partway through the loop.
  if (overdubbing_ && playing_ && track.lengthUs > 0 && (type == 0x90 || type == 0x80)) {
    if (noteOn) {
      if (startHold(input.status & 0x0F, input.data1, input.data2, input.tag, atUs, nowUs)) return true;
    } else if (endHold(input.status & 0x0F, input.data1, atUs, nowUs)) {
      return true;
    }
  }
  LoopMidiEvent event = input;
  event.layer = overdubbing_ ? currentLayer_ : 0;
  if (recordQuantizeUs_ > 0) {
    atUs = static_cast<uint32_t>(
        ((static_cast<uint64_t>(atUs) + recordQuantizeUs_ / 2U) / recordQuantizeUs_) *
        recordQuantizeUs_);
    const uint32_t limit = overdubbing_ ? track.lengthUs : recordFixedLengthUs_;
    if (limit > 0 && atUs >= limit) atUs = limit - 1U;
  }
  event.atUs = atUs;
  while (!insertEvent(recordingTrack_, event)) {
    if (!dropOldestHiddenLayer()) return false; // full: undone layers make room first
  }

  if (type == 0x90 || type == 0x80) {
    const uint8_t channel = input.status & 0x0F;
    recordHeld_[channel][input.data1] = noteOn ? static_cast<uint16_t>(input.tag + 1U) : 0;
  }
  return true;
}

bool Looper::startHold(uint8_t channel, uint8_t note, uint8_t velocity, uint16_t tag, uint32_t positionUs, uint64_t nowUs) {
  for (PendingHold &h : holds_) { // pressed again without being let go: the first hold ends here
    if (h.used && h.channel == channel && h.note == note) { flushHold(h, positionUs, nowUs); break; }
  }
  for (PendingHold &h : holds_) {
    if (h.used) continue;
    h = PendingHold{true, channel, note, velocity, currentLayer_, tag, positionUs, static_cast<uint32_t>(nowUs)};
    return true;
  }
  return false; // no room to keep it: it goes in as played
}

bool Looper::endHold(uint8_t channel, uint8_t note, uint32_t positionUs, uint64_t nowUs) {
  for (PendingHold &h : holds_) {
    if (!h.used || h.channel != channel || h.note != note) continue;
    flushHold(h, positionUs, nowUs);
    return true;
  }
  return false;
}

// Puts a finished hold into the loop. Held for the whole loop or longer, it's held everywhere. Crossing
// the top once, it's two pieces, the press to the end and the start to the release. Otherwise it's one.
void Looper::flushHold(PendingHold &h, uint32_t endPositionUs, uint64_t nowUs) {
  const uint32_t length = tracks_[recordingTrack_].lengthUs;
  const uint32_t heldUs = static_cast<uint32_t>(nowUs) - h.startUs;
  const uint32_t last = length > 0 ? length - 1U : 0U;
  const uint8_t on = static_cast<uint8_t>(0x90 | h.channel), off = static_cast<uint8_t>(0x80 | h.channel);
  auto put = [&](uint32_t at, uint8_t status, uint8_t velocity) {
    const LoopMidiEvent e{at, status, h.note, velocity, h.tag, h.layer};
    while (!insertEvent(recordingTrack_, e)) { if (!dropOldestHiddenLayer()) break; }
  };
  if (heldUs >= length || length == 0) {
    put(0, on, h.velocity);
    put(last, off, 0);
  } else if (h.positionUs <= endPositionUs) {
    put(h.positionUs, on, h.velocity);
    put(endPositionUs, off, 0);
  } else {
    put(h.positionUs, on, h.velocity);
    put(last, off, 0);
    put(0, on, h.velocity);
    put(endPositionUs, off, 0);
  }
  h.used = false;
}

void Looper::flushAllHolds(uint32_t endPositionUs, uint64_t nowUs) {
  for (PendingHold &h : holds_) if (h.used) flushHold(h, endPositionUs, nowUs);
}

void Looper::closeHeldRecordingNotes(uint32_t boundaryUs) {
  for (uint8_t channel = 0; channel < 16; ++channel) {
    for (uint8_t note = 0; note < 128; ++note) {
      if (!recordHeld_[channel][note]) continue;
      insertEvent(recordingTrack_, LoopMidiEvent{boundaryUs,
          static_cast<uint8_t>(0x80 | channel), note, 0,
          static_cast<uint16_t>(recordHeld_[channel][note] - 1U),
          static_cast<uint8_t>(overdubbing_ ? currentLayer_ : 0)}); // the pass's own layer
      recordHeld_[channel][note] = 0;
    }
  }
}

bool Looper::finishRecording(uint64_t nowUs) {
  if (!recording_ && !recordingArmed_) return false;
  if (recordingArmed_) {
    recordingArmed_ = false;
    return false;
  }
  LoopTrackState &track = tracks_[recordingTrack_];
  if (!overdubbing_) {
    const uint64_t elapsed = nowUs - recordStartUs_;
    track.lengthUs = recordFixedLengthUs_ ? recordFixedLengthUs_ :
        static_cast<uint32_t>(elapsed > UINT32_MAX ? UINT32_MAX : elapsed);
    if (playing_) {
      // The take begins where the performer began it, which is usually not the
      // top of the transport cycle. That offset is the track's phase from now
      // on and is restored every time playback starts again.
      track.cycleStartUs = nowUs;
      track.playCursor = track.head;
      captureTrackPhase(recordingTrack_);
    } else {
      track.startOffsetUs = 0;
    }
  }
  if (track.lengthUs == 0) track.lengthUs = 1;
  if (!overdubbing_) track.storedLengthUs = track.lengthUs;
  uint32_t closeAtUs = track.lengthUs - 1U;
  if (overdubbing_ && playing_) {
    closeAtUs = static_cast<uint32_t>((nowUs - track.cycleStartUs) % track.lengthUs);
  }
  if (overdubbing_) flushAllHolds(closeAtUs, nowUs);
  closeHeldRecordingNotes(closeAtUs);
  // An overdub pass that captured nothing isn't a layer to undo.
  if (overdubbing_ && currentLayer_ && !layerHasEvents(currentLayer_)) forgetLayer(currentLayer_);
  currentLayer_ = 0;
  recording_ = false;
  overdubbing_ = false;
  memset(recordHeld_, 0, sizeof(recordHeld_));
  for (PendingHold &h : holds_) h.used = false;
  // Whether THIS pass captured anything, not just whether the track holds
  // anything at all: an overdub track already had content before the pass
  // began, so track.count alone cannot tell the two apart. A pass that added
  // nothing must not read as a completed take to whatever asked.
  return track.count > recordStartCount_;
}

void Looper::cancelRecording() {
  recordingArmed_ = false;
  recording_ = false;
  overdubbing_ = false;
  memset(recordHeld_, 0, sizeof(recordHeld_));
  for (PendingHold &h : holds_) h.used = false;
}

// Where a track sits inside the shared transport cycle. Tracks are allowed to
// begin at different points, and that relationship is what has to survive a
// stop or a clear and undo, so it is stored rather than recomputed from zero.
void Looper::captureTrackPhase(uint8_t trackIndex) {
  if (trackIndex >= kLoopTrackCount) return;
  LoopTrackState &track = tracks_[trackIndex];
  if (track.lengthUs == 0) {
    track.startOffsetUs = 0;
    return;
  }
  if (track.cycleStartUs >= transportStartUs_) {
    track.startOffsetUs =
        static_cast<uint32_t>((track.cycleStartUs - transportStartUs_) % track.lengthUs);
  } else {
    const uint32_t behind =
        static_cast<uint32_t>((transportStartUs_ - track.cycleStartUs) % track.lengthUs);
    track.startOffsetUs = behind == 0 ? 0 : track.lengthUs - behind;
  }
}

// Resume continues a cycle that was already running, so an event sitting
// exactly on the resume point has been played and is skipped. Starting a cycle
// places the transport on that point, so an event there is still due.
void Looper::seekTrackTo(LoopTrackState &track, uint64_t nowUs,
                                  uint32_t positionUs, bool skipEventsAtPosition) {
  track.cycleStartUs = nowUs - positionUs;
  track.playCursor = track.head;
  while (track.playCursor != kNoLoopEvent) {
    const uint32_t atUs = slots_[track.playCursor].atUs;
    if (atUs > positionUs || (atUs == positionUs && !skipEventsAtPosition)) break;
    track.playCursor = slots_[track.playCursor].next;
  }
}

void Looper::resetPlaybackCursors(uint64_t nowUs) {
  transportStartUs_ = nowUs;
  for (LoopTrackState &track : tracks_) {
    if (track.lengthUs == 0) {
      track.playCursor = track.head;
      track.cycleStartUs = nowUs;
      continue;
    }
    // Restore the track to the point in its own loop that belongs at the top of
    // the transport cycle, so every track comes back in the same alignment it
    // had when it was recorded.
    const uint32_t offset = track.startOffsetUs % track.lengthUs;
    const uint32_t positionUs = offset == 0 ? 0 : track.lengthUs - offset;
    seekTrackTo(track, nowUs, positionUs, false);
  }
}

void Looper::start(uint64_t nowUs) {
  if (!hasAnyData()) return;
  playing_ = true;
  paused_ = false;
  for (LoopTrackState &track : tracks_) track.pausedPositionUs = 0;
  resetPlaybackCursors(nowUs);
}

void Looper::pause(uint64_t nowUs, ReleaseFn release, void *context) {
  if (!playing_) return;
  if (release) {
    for (uint8_t track = 0; track < kLoopTrackCount; ++track) release(context, track);
  }
  for (LoopTrackState &track : tracks_) {
    track.pausedPositionUs = track.lengthUs > 0
        ? static_cast<uint32_t>((nowUs - track.cycleStartUs) % track.lengthUs) : 0;
    track.playCursor = track.head;
  }
  playing_ = false;
  paused_ = true;
}

void Looper::resume(uint64_t nowUs) {
  if (playing_ || !hasAnyData()) return;
  if (!paused_) {
    start(nowUs);
    return;
  }
  playing_ = true;
  paused_ = false;
  transportStartUs_ = nowUs;
  for (uint8_t index = 0; index < kLoopTrackCount; ++index) {
    LoopTrackState &track = tracks_[index];
    if (track.lengthUs == 0) continue;
    const uint32_t positionUs = track.pausedPositionUs % track.lengthUs;
    seekTrackTo(track, nowUs, positionUs, true);
    track.pausedPositionUs = 0;
    // Resume keeps absolute positions, so the stored phase is re-expressed
    // against the new transport origin instead of drifting away from it.
    captureTrackPhase(index);
  }
}

void Looper::stop(ReleaseFn release, void *context) {
  if (release) {
    for (uint8_t track = 0; track < kLoopTrackCount; ++track) release(context, track);
  }
  playing_ = false;
  paused_ = false;
  for (LoopTrackState &track : tracks_) {
    track.playCursor = track.head;
    track.pausedPositionUs = 0;
  }
}

bool Looper::resizeTrack(uint8_t trackIndex, uint32_t lengthUs,
                                  uint64_t nowUs, ReleaseFn release,
                                  void *context) {
  if (trackIndex >= kLoopTrackCount || lengthUs == 0) return false;
  if ((recording_ || recordingArmed_) && recordingTrack_ == trackIndex) return false;
  LoopTrackState &track = tracks_[trackIndex];
  if (track.count == 0 || track.lengthUs == 0) return true;

  const uint32_t storedLength = track.storedLengthUs > 0
      ? track.storedLengthUs : track.lengthUs;
  if (lengthUs > storedLength) {
    uint32_t required = 0;
    for (uint64_t offset = storedLength; offset < lengthUs; offset += storedLength) {
      uint16_t slot = track.head;
      for (uint16_t visited = 0; visited < track.count && slot != kNoLoopEvent;
           ++visited, slot = slots_[slot].next) {
        if (static_cast<uint64_t>(slots_[slot].atUs) + offset < lengthUs) {
          ++required;
        }
      }
    }
    if (required > freeEvents()) {
      ++overflowCount_;
      return false;
    }

    const uint16_t sourceCount = track.count;
    for (uint64_t offset = storedLength; offset < lengthUs; offset += storedLength) {
      uint16_t slot = track.head;
      for (uint16_t visited = 0; visited < sourceCount && slot != kNoLoopEvent;
           ++visited) {
        const uint16_t next = slots_[slot].next;
        LoopMidiEvent copy = slots_[slot].event();
        const uint64_t copyTime = static_cast<uint64_t>(copy.atUs) + offset;
        if (copyTime < lengthUs) {
          copy.atUs = static_cast<uint32_t>(copyTime);
          if (!insertEvent(trackIndex, copy)) return false;
        }
        slot = next;
      }
    }
    track.storedLengthUs = lengthUs;
  }

  const bool wasAudible = playing_ && audible(trackIndex);
  const uint32_t oldLengthUs = track.lengthUs;
  track.lengthUs = lengthUs;
  if (playing_) {
    uint64_t positionUs = oldLengthUs > 0
        ? (nowUs - track.cycleStartUs) % oldLengthUs : 0;
    positionUs %= lengthUs;
    if (wasAudible && release) release(context, trackIndex);
    seekTrackTo(track, nowUs, static_cast<uint32_t>(positionUs), true);
    captureTrackPhase(trackIndex);
  } else {
    track.playCursor = track.head;
    track.startOffsetUs %= lengthUs;
  }
  return true;
}

bool Looper::audible(uint8_t trackIndex) const {
  if (trackIndex >= kLoopTrackCount) return false;
  bool anySolo = false;
  for (const LoopTrackState &track : tracks_) anySolo |= track.solo && !track.hidden;
  const LoopTrackState &track = tracks_[trackIndex];
  return !importing_[trackIndex] && !track.hidden && !track.muted && (!anySolo || track.solo);
}

void Looper::collectHeldNotes(uint8_t track, HeldNoteFn fn, void *context) const {
  if (track >= kLoopTrackCount || !fn) return;
  const LoopTrackState &state = tracks_[track];
  // Nothing between head and the cursor to replay, either because the track
  // is empty or because the cursor sits at the very top of its cycle.
  if (state.head == kNoLoopEvent || state.head == state.playCursor) return;

  // FoxWizard: the tag (+ 1, 0 not held) says where the note plays back to. Hidden layers don't sound.
  static uint16_t heldTag[16][128];
  static uint8_t velocity[16][128];
  memset(heldTag, 0, sizeof(heldTag));
  uint16_t slot = state.head;
  while (slot != kNoLoopEvent && slot != state.playCursor) {
    const LoopMidiEvent event = slots_[slot].event();
    const uint8_t type = event.status & 0xF0;
    if ((type == 0x90 || type == 0x80) && event.data1 <= 127 && !layerHidden(event.layer)) {
      const uint8_t channel = event.status & 0x0F;
      const bool on = type == 0x90 && event.data2 > 0;
      heldTag[channel][event.data1] = on ? static_cast<uint16_t>(event.tag + 1U) : 0;
      if (on) velocity[channel][event.data1] = event.data2;
    }
    slot = slots_[slot].next;
  }
  for (uint8_t channel = 0; channel < 16; ++channel) {
    for (uint8_t note = 0; note < 128; ++note) {
      if (heldTag[channel][note]) {
        fn(context, track, static_cast<uint16_t>(heldTag[channel][note] - 1U), channel, note, velocity[channel][note]);
      }
    }
  }
}

void Looper::tick(uint64_t nowUs, EmitFn emit, ReleaseFn release,
                           void *context, uint16_t maxEvents) {
  if (recording_ && !overdubbing_ && recordFixedLengthUs_ > 0 &&
      nowUs - recordStartUs_ >= recordFixedLengthUs_) {
    finishRecording(recordStartUs_ + recordFixedLengthUs_);
    if (!playing_) start(recordStartUs_ + recordFixedLengthUs_);
  }
  if (!playing_) return;
  uint16_t processed = 0;
  for (uint8_t trackIndex = 0; trackIndex < kLoopTrackCount; ++trackIndex) {
    LoopTrackState &track = tracks_[trackIndex];
    if (track.count == 0 || track.lengthUs == 0) continue;
    if (nowUs - track.cycleStartUs >= track.lengthUs) {
      if (release) release(context, trackIndex);
      const uint64_t cycles = (nowUs - track.cycleStartUs) / track.lengthUs;
      track.cycleStartUs += cycles * track.lengthUs;
      track.playCursor = track.head;
    }
    const bool canEmit = audible(trackIndex);
    const uint32_t elapsed = static_cast<uint32_t>(nowUs - track.cycleStartUs);
    while (track.playCursor != kNoLoopEvent && processed < maxEvents) {
      const EventSlot &slot = slots_[track.playCursor];
      if (slot.atUs > elapsed) break;
      if (canEmit && eraseFn_ && (slot.status & 0xF0) == 0x90 && slot.data2 && !layerHidden(slot.layer) &&
          eraseFn_(eraseContext_, slot.event())) {
        track.playCursor = eraseNote(trackIndex, track.playCursor); // never played, and gone
        ++processed;
        continue;
      }
      if (canEmit && emit && !layerHidden(slot.layer)) emit(context, trackIndex, slot.event());
      track.playCursor = slot.next;
      ++processed;
    }
    if (processed >= maxEvents) break;
  }
}

void Looper::releaseIfAudibilityChanged(uint8_t track, bool wasAudible,
                                                  ReleaseFn release, void *context) {
  if (wasAudible && !audible(track) && release) release(context, track);
}

void Looper::safeClear(uint8_t track, ReleaseFn release, void *context) {
  if (track >= kLoopTrackCount) return;
  // A pending arm survives its own track being cleared. There is nothing
  // captured yet, so tidying up old content underneath it is not a reason to
  // abandon the take: the performer stays ready to play, right where they
  // were, with the old material now out of the way. An armed-and-recording
  // pass is different: it already holds real captured events, so clearing its
  // own track out from under it still aborts it, the same as before.
  if (recording_ && recordingTrack_ == track) cancelRecording();
  // An empty track must not become hidden.  Undo can never bring it back, and a
  // phantom hidden track inverts every "is anything cleared" decision above.
  if (tracks_[track].count == 0) {
    if (release) release(context, track);
    return;
  }
  tracks_[track].hidden = true;
  if (release) release(context, track);
}

void Looper::undoClear(uint8_t track) {
  if (track >= kLoopTrackCount || tracks_[track].count == 0) return;
  // Bringing the old content back means abandoning whatever take was pending
  // for this track: undo and a fresh replacement are opposite choices, so
  // choosing one cancels the other rather than leaving a stale arm behind.
  if ((recording_ || recordingArmed_) && recordingTrack_ == track) cancelRecording();
  tracks_[track].hidden = false;
}

void Looper::permanentlyClear(uint8_t trackIndex) {
  if (trackIndex >= kLoopTrackCount) return;
  uint16_t slot = tracks_[trackIndex].head;
  while (slot != kNoLoopEvent) {
    const uint16_t next = slots_[slot].next;
    freeSlot(slot);
    slot = next;
  }
  tracks_[trackIndex] = LoopTrackState{};
  resetLayers();
}

void Looper::clearAll(ReleaseFn release, void *context) {
  if (release) {
    for (uint8_t track = 0; track < kLoopTrackCount; ++track) release(context, track);
  }
  for (uint8_t track = 0; track < kLoopTrackCount; ++track) permanentlyClear(track);
  cancelRecording();
  playing_ = false;
  paused_ = false;
}

void Looper::setMuted(uint8_t track, bool muted, ReleaseFn release, void *context) {
  if (track >= kLoopTrackCount) return;
  const bool wasAudible = audible(track);
  tracks_[track].muted = muted;
  releaseIfAudibilityChanged(track, wasAudible, release, context);
}

void Looper::setSolo(uint8_t track, bool solo, ReleaseFn release, void *context) {
  if (track >= kLoopTrackCount) return;
  bool wasAudible[kLoopTrackCount];
  for (uint8_t i = 0; i < kLoopTrackCount; ++i) wasAudible[i] = audible(i);
  tracks_[track].solo = solo;
  for (uint8_t i = 0; i < kLoopTrackCount; ++i) {
    releaseIfAudibilityChanged(i, wasAudible[i], release, context);
  }
}

bool Looper::hasAnyData() const {
  for (const LoopTrackState &track : tracks_) if (track.count > 0 && !track.hidden) return true;
  return false;
}

uint8_t Looper::oldestPopulatedTrack() const {
  uint8_t result = selectedTrack_;
  uint32_t oldest = UINT32_MAX;
  for (uint8_t i = 0; i < kLoopTrackCount; ++i) {
    if (tracks_[i].count > 0 && tracks_[i].generation < oldest) {
      oldest = tracks_[i].generation;
      result = i;
    }
  }
  return result;
}

// The most recently recorded layer, cleared or not. Stepping back to the last
// thing played is what a performer means by the previous layer.
uint8_t Looper::newestPopulatedTrack() const {
  uint8_t result = selectedTrack_;
  uint32_t newest = 0;
  bool found = false;
  for (uint8_t i = 0; i < kLoopTrackCount; ++i) {
    if (tracks_[i].count == 0) continue;
    if (!found || tracks_[i].generation >= newest) {
      newest = tracks_[i].generation;
      result = i;
      found = true;
    }
  }
  return result;
}

bool Looper::visitEvents(VisitFn visitor, void *context) const {
  if (!visitor) return false;
  for (uint8_t track = 0; track < kLoopTrackCount; ++track) {
    uint16_t slot = tracks_[track].head;
    while (slot != kNoLoopEvent) {
      // A save keeps what's showing: undone layers don't outlive a power cycle.
      if (!layerHidden(slots_[slot].layer) && !visitor(context, track, slots_[slot].event())) return false;
      slot = slots_[slot].next;
    }
  }
  return true;
}

bool Looper::restoreEvent(uint8_t track, const LoopMidiEvent &event) {
  return insertEvent(track, event);
}

void Looper::setRestoredTrackState(uint8_t track, uint32_t lengthUs,
                                            uint32_t storedLengthUs,
                                            uint32_t generation, bool muted,
                                            bool solo, bool hidden,
                                            uint32_t startOffsetUs) {
  if (track >= kLoopTrackCount) return;
  tracks_[track].startOffsetUs = lengthUs > 0 ? startOffsetUs % lengthUs : 0;
  tracks_[track].lengthUs = lengthUs;
  tracks_[track].storedLengthUs = storedLengthUs >= lengthUs
      ? storedLengthUs : lengthUs;
  tracks_[track].generation = generation;
  tracks_[track].muted = muted;
  tracks_[track].solo = solo;
  tracks_[track].hidden = hidden;
  generationCounter_ = generation > generationCounter_ ? generation : generationCounter_;
}

void Looper::beginImport(uint8_t track, uint32_t lengthUs,
                                  ReleaseFn release, void *context) {
  if (track >= kLoopTrackCount || lengthUs == 0) return;
  if (release && audible(track)) release(context, track);
  if (recordingTrack_ == track) cancelRecording();
  permanentlyClear(track);
  selectedTrack_ = track;
  tracks_[track].lengthUs = lengthUs;
  tracks_[track].storedLengthUs = lengthUs;
  tracks_[track].generation = ++generationCounter_;
  importing_[track] = true;
}

bool Looper::importEvent(uint8_t track, const LoopMidiEvent &event) {
  if (track >= kLoopTrackCount || !importing_[track] ||
      event.atUs >= tracks_[track].lengthUs) return false;
  return insertEvent(track, event);
}

void Looper::finishImport(uint8_t track, uint64_t boundaryUs) {
  if (track >= kLoopTrackCount || !importing_[track]) return;
  importing_[track] = false;
  tracks_[track].playCursor = tracks_[track].head;
  tracks_[track].cycleStartUs = boundaryUs;
  captureTrackPhase(track);
}

}  // namespace fw
