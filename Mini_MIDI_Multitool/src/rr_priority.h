#pragma once

// FoxWizard 1.108, Round Robin Priority. A route with Round Robin on and this mode sends each new note to the first free
// channel of its selected channels, in rank order, and when every channel is down the oldest note gives its channel up.
// Rank is the lowest channel number first unless the route has a custom order, kept in one block shared by all routes.
//
// The shared block, kRrOrderPoolBytes long, is a list of entries: route index + 1 (0 ends the list), the number of channels
// (1-16), then (count + 1) / 2 bytes of channels, 4 bits each, the first in the high nibble. A route with no entry ranks its
// channels lowest first and costs nothing. Everything here is plain C++ so a computer can test it.

#include <stdint.h>

namespace fw {

constexpr uint8_t kRrOrderPoolBytes = 96;

// The entry for a route: its channel count and where the nibbles start, 0 and untouched when there is none or the block is
// damaged (a count out of range, or an entry running past the end).
inline uint8_t rrOrderFind(const uint8_t *pool, uint16_t route, const uint8_t **nibbles) {
  uint8_t pos = 0;
  while (pos + 2 <= kRrOrderPoolBytes && pool[pos] != 0) {
    const uint8_t count = pool[pos + 1];
    const uint8_t bytes = (uint8_t)((count + 1) / 2);
    if (count == 0 || count > 16 || pos + 2 + bytes > kRrOrderPoolBytes) return 0;
    if (pool[pos] == (uint8_t)(route + 1)) { *nibbles = pool + pos + 2; return count; }
    pos = (uint8_t)(pos + 2 + bytes);
  }
  return 0;
}

// How many bytes of the block are in use, for saving only that much.
inline uint8_t rrOrderUsed(const uint8_t *pool) {
  uint8_t pos = 0;
  while (pos + 2 <= kRrOrderPoolBytes && pool[pos] != 0) {
    const uint8_t count = pool[pos + 1];
    const uint8_t bytes = (uint8_t)((count + 1) / 2);
    if (count == 0 || count > 16 || pos + 2 + bytes > kRrOrderPoolBytes) break;
    pos = (uint8_t)(pos + 2 + bytes);
  }
  return pos;
}

// The channels of outMask in rank order. The route's own order first, for the channels still selected, then any selected
// channel it doesn't name, lowest first. Returns how many, up to 16.
inline uint8_t rrRanked(uint16_t outMask, const uint8_t *pool, uint16_t route, uint8_t out[16]) {
  uint8_t n = 0;
  uint16_t taken = 0;
  const uint8_t *nibbles = nullptr;
  const uint8_t count = rrOrderFind(pool, route, &nibbles);
  for (uint8_t i = 0; i < count; ++i) {
    const uint8_t c = (i & 1) ? (nibbles[i >> 1] & 0x0F) : (uint8_t)(nibbles[i >> 1] >> 4);
    if ((outMask & (1u << c)) && !(taken & (1u << c))) { out[n++] = c; taken |= (uint16_t)(1u << c); }
  }
  for (uint8_t c = 0; c < 16; ++c) {
    if ((outMask & (1u << c)) && !(taken & (1u << c))) out[n++] = c;
  }
  return n;
}

// A route was deleted: its entry goes and the routes after it move down one.
inline void rrOrderRemoveRoute(uint8_t *pool, uint16_t route) {
  uint8_t copy[kRrOrderPoolBytes];
  const uint8_t used = rrOrderUsed(pool);
  uint8_t pos = 0, write = 0;
  for (uint8_t i = 0; i < used; ++i) copy[i] = pool[i];
  while (pos < used) {
    const uint8_t bytes = (uint8_t)(2 + (copy[pos + 1] + 1) / 2);
    if (copy[pos] != (uint8_t)(route + 1)) {
      for (uint8_t i = 0; i < bytes; ++i) pool[write + i] = copy[pos + i];
      if (copy[pos] > (uint8_t)(route + 1)) pool[write] = (uint8_t)(copy[pos] - 1);
      write = (uint8_t)(write + bytes);
    }
    pos = (uint8_t)(pos + bytes);
  }
  for (uint8_t i = write; i < kRrOrderPoolBytes; ++i) pool[i] = 0;
}

// A route moved from one place to another: the entries follow their routes.
inline void rrOrderMoveRoute(uint8_t *pool, uint16_t from, uint16_t to) {
  const uint8_t used = rrOrderUsed(pool);
  uint8_t pos = 0;
  while (pos < used) {
    const uint16_t r = (uint16_t)(pool[pos] - 1);
    uint16_t now = r;
    if (r == from) now = to;
    else if (from < to && r > from && r <= to) now = (uint16_t)(r - 1);
    else if (from > to && r >= to && r < from) now = (uint16_t)(r + 1);
    pool[pos] = (uint8_t)(now + 1);
    pos = (uint8_t)(pos + 2 + (pool[pos + 1] + 1) / 2);
  }
}

// Which channel a new note takes. slots are the route's tracked notes: a Slot has note (0xFF free), channel (0-15 where it
// sounds, 0xFF none, 0xFE given up to a newer note) and order (a counter that wraps, newer is later). self is the slot of
// the note being placed, which doesn't count as down. victim is the slot whose note must be cut to make room, -1 if none.
struct PriorityPick {
  uint8_t channel;
  int8_t victim;
};

template <typename Slot>
PriorityPick rrPick(const uint8_t *ranked, uint8_t n, const Slot *slots, uint8_t depth, const Slot *self) {
  uint16_t down = 0;
  for (uint8_t i = 0; i < depth; ++i) {
    if (&slots[i] == self || slots[i].note == 0xFF || slots[i].channel >= 16) continue;
    down |= (uint16_t)(1u << slots[i].channel);
  }
  for (uint8_t r = 0; r < n; ++r) {
    if (!(down & (1u << ranked[r]))) return PriorityPick{ranked[r], -1};
  }
  // Every selected channel is down: the oldest of those notes gives its channel up.
  uint16_t selected = 0;
  for (uint8_t r = 0; r < n; ++r) selected |= (uint16_t)(1u << ranked[r]);
  int8_t victim = -1;
  for (uint8_t i = 0; i < depth; ++i) {
    if (&slots[i] == self || slots[i].note == 0xFF || slots[i].channel >= 16 || !(selected & (1u << slots[i].channel))) continue;
    if (victim < 0 || (int16_t)(slots[i].order - slots[victim].order) < 0) victim = (int8_t)i;
  }
  if (victim < 0) return PriorityPick{ranked[0], -1};
  return PriorityPick{slots[victim].channel, victim};
}

// A Note On under Priority. emit(channel, on, outNote) sends a Note On or Note Off, transpose turns a slot's note into its
// pitch. A key pressed again while down first releases where it was. Nothing happens without a slot or a channel.
template <typename Slot, typename Emit>
void rrNoteOn(Slot *slots, uint8_t depth, uint16_t &orderCounter, Slot *slot, bool stampOrder, const uint8_t *ranked, uint8_t n,
              int16_t transpose, Emit emit) {
  if (!slot || n == 0) return;
  if (slot->channel < 16) emit(slot->channel, false, (int16_t)(slot->note + transpose));
  slot->channel = 0xFF;
  const PriorityPick pick = rrPick(ranked, n, slots, depth, slot);
  if (pick.victim >= 0) {
    Slot &v = slots[pick.victim];
    emit(v.channel, false, (int16_t)(v.note + transpose));
    v.channel = 0xFE;
  }
  slot->channel = pick.channel;
  if (stampOrder) slot->order = ++orderCounter;
  emit(pick.channel, true, (int16_t)(slot->note + transpose));
}

// A Note Off under Priority: where the note sounds, unless its channel was given up, then nothing.
template <typename Slot, typename Emit>
void rrNoteOff(Slot *slot, int16_t transpose, Emit emit) {
  if (!slot) return;
  if (slot->channel == 0xFE) { slot->channel = 0xFF; return; }
  if (slot->channel >= 16) return;
  const uint8_t channel = slot->channel;
  slot->channel = 0xFF;
  emit(channel, false, (int16_t)(slot->note + transpose));
}

}  // namespace fw
