#pragma once

// Shared by both Host Edition sketches. Keep the copies in Foxwizard_Host_Edition_Sidecar and
// Mini_MIDI_Multitool identical. Frame format and ports: Foxwizard Host Edition/README.md.

#include <stdint.h>
#include <string.h>

#include "ump.h"

constexpr uint32_t LINK_BAUD = 1000000;

constexpr uint8_t LINK_PORT_ALL = 0;      // main -> host: every USB device and every DIN OUT
constexpr uint8_t LINK_PORT_DIN = 5;      // host -> main: DIN IN. main -> host: DIN OUT 1, 6-9 = OUT 2-5
constexpr uint8_t LINK_PORT_CONTROL = 15; // both ways: link control frames, never MIDI
constexpr uint8_t LINK_PORT_COUNT = 16;

// USB Host slots (host firmware 6, main 1.74): 1-7. Slots 1-4 are link ports 1-4, slots 5-7 are link
// ports 10-12, which leaves 13 and 14 for BLE MIDI. In the port map frame's mounted-slots byte,
// slots 1-4 are bits 0-3 and slots 5-7 are bits 5-7, with bit 4 left to say a TRS board is fitted.
constexpr uint8_t HOST_SLOTS = 7;
inline uint8_t hostLinkPort(uint8_t slot) { return slot <= 4 ? slot : slot + 5; }
inline uint8_t hostSlotFromLink(uint8_t port) { return (port >= 1 && port <= 4) ? port : ((port >= 10 && port <= 12) ? port - 5 : 0); }
inline uint8_t hostMaskBit(uint8_t slot) { return slot <= 4 ? slot - 1 : slot; }

// Control frame types, carried in the CIN field of a port 15 frame.
constexpr uint8_t CONTROL_PORT_MAP = 1; // host -> main. byte 1 = port mask, 2 = fw version, 3 = drops
constexpr uint8_t CONTROL_FLOW = 2;     // both ways. byte 1 = 1 pause, 0 resume
constexpr uint8_t CONTROL_HOST_RESTART = 3; // host -> main. byte 1 = last restart reason, 2 = hub port, 3 = restart count
constexpr uint8_t CONTROL_BRIGHTNESS = 4; // main -> host. byte 1 = ledBrightnessStep (0-10), for the sidecar's status LED
constexpr uint8_t CONTROL_HOST_HUB = 6;    // host -> main. byte 1 = downstream jacks of the hubs attached (0 no hub), 2 = hubs attached, 3 = devices waiting for a free slot
constexpr uint8_t CONTROL_HOST_REPORT = 5; // host -> main. byte 1 = index, 2-3 = report bytes 2*index and 2*index+1

// The host report (host fw 3): what the host brain knew when it last restarted, sent a few times
// after a recovery restart. Multi-byte values are little-endian.
constexpr uint8_t HOST_REPORT_LAYOUT = 2;      // 1: 4 slots, 16 events from byte 112, 240 bytes
constexpr uint16_t HOST_REPORT_BYTES = 360;
constexpr uint8_t HOST_REPORT_PORT_BYTES = 24; // per slot, 7 of them from byte 16. Byte 0: bit 0 mounted, 1 writable, 2-4 its hub port, 5-6 tier
constexpr uint8_t HOST_REPORT_EVENTS = 22;     // 8 bytes each, from byte 184, oldest first
// Host report event types
constexpr uint8_t HOST_EVENT_BOOT = 1;     // a = restart reason
constexpr uint8_t HOST_EVENT_MOUNT = 2;    // VID and PID are in the port block
constexpr uint8_t HOST_EVENT_UNMOUNT = 3;
constexpr uint8_t HOST_EVENT_SKIP = 4;     // the port accepted nothing for 1 s, its output is being dropped
constexpr uint8_t HOST_EVENT_RESTART = 5;  // a = reason
constexpr uint8_t HOST_EVENT_FAULT = 6;    // a = USB driver fault code
constexpr uint8_t HOST_EVENT_FULL = 7;     // a = device address. It came while all four USB Host slots were taken, and waits
constexpr uint8_t HOST_EVENT_TRACE = 8;    // port = what (FOXWIZARD_TRACE_*), a and b as that says
constexpr uint8_t HOST_EVENT_DEVICE = 9;   // a USB device finished enumerating. port = its hub port, a = its address, b = speed (0 full, 1 low, 2 high)
// Where the host brain's loop was, for a restart the watchdog caused
constexpr uint8_t HOST_STAGE_USB_TASK = 1, HOST_STAGE_LINK_IN = 2, HOST_STAGE_DIN_IN = 3, HOST_STAGE_USB_IN = 4,
                  HOST_STAGE_DISPATCH = 5, HOST_STAGE_USB_OUT = 6, HOST_STAGE_DIN_OUT = 7, HOST_STAGE_CONTROL = 8,
                  HOST_STAGE_LINK_OUT = 9, HOST_STAGE_LEDS = 10;

// Why the host brain last restarted, for CONTROL_HOST_RESTART and GET_STATUS.
constexpr uint8_t RESTART_NONE = 0;  // powered up normally
constexpr uint8_t RESTART_HANG = 1;  // watchdog: the loop stopped, usually the USB driver halting on a device error
constexpr uint8_t RESTART_WEDGE = 2; // queued output to one USB Host port stopped draining
constexpr uint8_t RESTART_DATA_SEQ = 3; // the USB driver hit a data sequence (DATA0/DATA1) mismatch
constexpr uint8_t RESTART_BUF_CTRL = 4; // the USB driver armed a buffer that was already armed
constexpr uint8_t RESTART_STALL = 5;    // the USB driver was asked to clear a device's stalled endpoint

constexpr uint32_t FLOW_REPEAT_MS = 100; // flow state is resent this often
constexpr uint32_t FLOW_EXPIRY_MS = 500; // a pause with no repeat this long is treated as resume

// The link carries UMP, the MIDI 2.0 packet (ump.h), one 32-bit word to a frame. Each side converts to and from the USB
// MIDI 1.0 packets its own code uses at the edge of the link, so a MIDI 2.0 message passes through unchanged and a MIDI 1.0
// one is a UMP packet that carries it exactly.
//
// A frame is 6 bytes. Byte 0 is the only one with bit 7 set: bits 6-3 the port, bit 2 set on the first word of a message.
// Bytes 1-5 are the 32 bit word, 7 bits at a time, most significant first (4 bits in byte 1). A message is 1 to 4 words, its
// length the Message Type of its first word, so a message type added later passes through without being understood.
constexpr uint8_t LINK_FRAME_BYTES = 6;

struct LinkFrame {
  uint8_t b[LINK_FRAME_BYTES];
};

struct MidiPacket {
  uint8_t b[4];
};

inline void encodeLinkWord(uint8_t port, uint32_t word, bool first, LinkFrame &f) {
  f.b[0] = 0x80 | ((port & 0x0F) << 3) | (first ? 0x04 : 0x00);
  f.b[1] = (uint8_t)((word >> 28) & 0x0F);
  f.b[2] = (uint8_t)((word >> 21) & 0x7F);
  f.b[3] = (uint8_t)((word >> 14) & 0x7F);
  f.b[4] = (uint8_t)((word >> 7) & 0x7F);
  f.b[5] = (uint8_t)(word & 0x7F);
}

// Returns the port.
inline uint8_t decodeLinkWord(const LinkFrame &f, uint32_t &word, bool &first) {
  word = ((uint32_t)(f.b[1] & 0x0F) << 28) | ((uint32_t)f.b[2] << 21) | ((uint32_t)f.b[3] << 14) | ((uint32_t)f.b[4] << 7) | f.b[5];
  first = (f.b[0] & 0x04) != 0;
  return (f.b[0] >> 3) & 0x0F;
}

// Control frames (port 15) are one word whose 4 bytes are the type then 3 bytes of data.
inline void encodeLinkFrame(uint8_t port, const uint8_t pkt[4], LinkFrame &f) {
  encodeLinkWord(port, ((uint32_t)pkt[0] << 24) | ((uint32_t)pkt[1] << 16) | ((uint32_t)pkt[2] << 8) | pkt[3], true, f);
}

inline uint8_t decodeLinkFrame(const LinkFrame &f, uint8_t pkt[4]) {
  uint32_t word; bool first;
  const uint8_t port = decodeLinkWord(f, word, first);
  pkt[0] = (uint8_t)(word >> 24); pkt[1] = (uint8_t)(word >> 16); pkt[2] = (uint8_t)(word >> 8); pkt[3] = (uint8_t)word;
  return port;
}

// Clock, Start, Continue, Stop, Active Sensing, Reset. These skip every normal queue.
inline bool isRealtimeWord(uint32_t w) { return (w >> 28) == 0x1 && ((w >> 16) & 0xFF) >= 0xF8; }

// Messages that leave notes or sustain stuck if lost. Same set as ARPnMIDI's secondary link, plus sustain release, for
// MIDI 1.0 and MIDI 2.0 channel voice messages. Judged on a message's first word, and its other words follow it.
inline bool isCriticalWord(uint32_t w) {
  const uint8_t type = (uint8_t)(w >> 28), op = (uint8_t)((w >> 16) & 0xF0);
  const uint8_t d1 = (uint8_t)(w >> 8), d2 = (uint8_t)w;
  if (type == 0x2) {
    if (op == 0x80) return true;
    if (op == 0x90 && d2 == 0) return true;
    if (op == 0xB0 && (d1 == 120 || d1 == 123)) return true;
    if (op == 0xB0 && d1 == 64 && d2 < 64) return true;
  } else if (type == 0x4) {
    if (op == 0x80) return true;
    if (op == 0xB0 && (d1 == 120 || d1 == 123 || d1 == 64)) return true; // sustain's value is in the second word
  }
  return false;
}

// Puts a message's words back together from frames. A real time message can sit between the words of another, it goes first
// on the link, so it comes out whole without disturbing the one in progress. A frame marked as a first word that arrives
// mid-message drops what was waiting, and the words of a message whose start was lost are ignored.
struct LinkMessageReader {
  uint32_t words[4];
  uint8_t have = 0;
  uint8_t need = 0;

  // Returns the number of words in out when a message is whole, else 0.
  uint8_t feed(uint32_t word, bool first, uint32_t out[4]) {
    if (first) {
      if (isRealtimeWord(word)) { out[0] = word; return 1; }
      have = 0;
      need = fw::ump::wordCount(word);
    } else if (need == 0) {
      return 0;
    }
    words[have++] = word;
    if (have < need) return 0;
    for (uint8_t i = 0; i < need; ++i) out[i] = words[i];
    const uint8_t n = need;
    have = need = 0;
    return n;
  }
};

// Collects link bytes into frames. Only byte 0 of a frame has bit 7 set, so any glitch costs at
// most the frame it hit.
struct LinkFrameReader {
  LinkFrame frame;
  uint8_t len = 0;

  bool feed(uint8_t byte) {
    if (byte & 0x80) {
      frame.b[0] = byte;
      len = 1;
      return false;
    }
    if (len == 0) return false;
    frame.b[len++] = byte;
    if (len < LINK_FRAME_BYTES) return false;
    len = 0;
    return true;
  }
};

// Bounded FIFO that keeps its last CriticalReserve slots for critical messages, so Note Offs
// still fit after ordinary traffic fills it. Same policy as ARPnMIDI's secondary link queue.
// Main loop only, never touched from an interrupt.
template <typename T, uint16_t Capacity, uint16_t CriticalReserve>
class MidiQueue {
 public:
  static_assert(CriticalReserve < Capacity - 1, "reserve must leave room for ordinary traffic");

  uint16_t size() const { return (uint16_t)((head_ + Capacity - tail_) % Capacity); }
  bool empty() const { return head_ == tail_; }

  uint16_t freeSlots(bool critical) const {
    const uint16_t usable = (uint16_t)(Capacity - 1 - (critical ? 0 : CriticalReserve));
    const uint16_t used = size();
    return used >= usable ? 0 : (uint16_t)(usable - used);
  }

  bool push(const T &value, bool critical) {
    if (freeSlots(critical) == 0) {
      ++dropped_;
      if (critical) ++criticalDropped_;
      return false;
    }
    entries_[head_] = value;
    head_ = (uint16_t)((head_ + 1) % Capacity);
    const uint16_t used = size();
    if (used > highWater_) highWater_ = used;
    return true;
  }

  T *peek() { return empty() ? nullptr : &entries_[tail_]; }
  T *peekAt(uint16_t i) { return i < size() ? &entries_[(tail_ + i) % Capacity] : nullptr; } // the i-th waiting entry
  void pop() { if (!empty()) tail_ = (uint16_t)((tail_ + 1) % Capacity); }
  void clear() { head_ = tail_ = 0; }

  void countDrop(bool critical) {
    ++dropped_;
    if (critical) ++criticalDropped_;
  }
  uint32_t dropped() const { return dropped_; }
  uint32_t criticalDropped() const { return criticalDropped_; }
  uint16_t highWater() const { return highWater_; }

 private:
  T entries_[Capacity];
  uint16_t head_ = 0;
  uint16_t tail_ = 0;
  uint16_t highWater_ = 0;
  uint32_t dropped_ = 0;
  uint32_t criticalDropped_ = 0;
};

// Puts a whole message into a frame queue or none of it, so a message never goes out in part.
template <typename Q>
inline bool pushLinkMessage(Q &q, uint8_t port, const uint32_t *words, uint8_t n, bool critical) {
  if (q.freeSlots(critical) < n) {
    q.countDrop(critical);
    return false;
  }
  for (uint8_t i = 0; i < n; ++i) {
    LinkFrame f;
    encodeLinkWord(port, words[i], i == 0, f);
    q.push(f, critical);
  }
  return true;
}
