#pragma once

// Shared by both Host Edition sketches. Keep the copies in Foxwizard_Host_Edition_Sidecar and
// Mini_MIDI_Multitool identical. Frame format and ports: Foxwizard Host Edition/README.md.

#include <stdint.h>
#include <string.h>

constexpr uint32_t LINK_BAUD = 1000000;

constexpr uint8_t LINK_PORT_ALL = 0;      // main -> host: every USB device and every DIN OUT
constexpr uint8_t LINK_PORT_DIN = 5;      // host -> main: DIN IN. main -> host: DIN OUT 1, 6-9 = OUT 2-5
constexpr uint8_t LINK_PORT_CONTROL = 15; // both ways: link control frames, never MIDI
constexpr uint8_t LINK_PORT_COUNT = 16;

// Control frame types, carried in the CIN field of a port 15 frame.
constexpr uint8_t CONTROL_PORT_MAP = 1; // host -> main. byte 1 = port mask, 2 = fw version, 3 = drops
constexpr uint8_t CONTROL_FLOW = 2;     // both ways. byte 1 = 1 pause, 0 resume
constexpr uint8_t CONTROL_HOST_RESTART = 3; // host -> main. byte 1 = last restart reason, 2 = hub port, 3 = restart count

// Why the host brain last restarted, for CONTROL_HOST_RESTART and GET_STATUS.
constexpr uint8_t RESTART_NONE = 0;  // powered up normally
constexpr uint8_t RESTART_HANG = 1;  // watchdog: the loop stopped, usually the USB driver halting on a device error
constexpr uint8_t RESTART_WEDGE = 2; // queued output to one USB Host port stopped draining
constexpr uint8_t RESTART_DATA_SEQ = 3; // the USB driver hit a data sequence (DATA0/DATA1) mismatch
constexpr uint8_t RESTART_BUF_CTRL = 4; // the USB driver armed a buffer that was already armed
constexpr uint8_t RESTART_STALL = 5;    // the USB driver was asked to clear a device's stalled endpoint

constexpr uint32_t FLOW_REPEAT_MS = 100; // flow state is resent this often
constexpr uint32_t FLOW_EXPIRY_MS = 500; // a pause with no repeat this long is treated as resume

struct LinkFrame {
  uint8_t b[5];
};

struct MidiPacket {
  uint8_t b[4];
};

// Clock, Start, Continue, Stop, Active Sensing, Reset. These skip every normal queue.
inline bool isRealtimePacket(const uint8_t pkt[4]) {
  uint8_t cin = pkt[0] & 0x0F;
  return (cin == 0x05 || cin == 0x0F) && pkt[1] >= 0xF8;
}

// Messages that leave notes or sustain stuck if lost. Same set as ARPnMIDI's secondary link,
// plus sustain release.
inline bool isCriticalPacket(const uint8_t pkt[4]) {
  uint8_t type = pkt[1] & 0xF0;
  if (type == 0x80) return true;
  if (type == 0x90 && pkt[3] == 0) return true;
  if (type == 0xB0 && (pkt[2] == 120 || pkt[2] == 123)) return true;
  if (type == 0xB0 && pkt[2] == 64 && pkt[3] < 64) return true;
  return false;
}

inline void encodeLinkFrame(uint8_t port, const uint8_t pkt[4], LinkFrame &f) {
  f.b[0] = 0x80 | (((pkt[0] >> 4) & 0x07) << 4) | (pkt[0] & 0x0F);
  f.b[1] = ((port & 0x0F) << 3) | ((pkt[1] >> 7) & 1) | (((pkt[2] >> 7) & 1) << 1) | (((pkt[3] >> 7) & 1) << 2);
  f.b[2] = pkt[1] & 0x7F;
  f.b[3] = pkt[2] & 0x7F;
  f.b[4] = pkt[3] & 0x7F;
}

// Returns the port.
inline uint8_t decodeLinkFrame(const LinkFrame &f, uint8_t pkt[4]) {
  uint8_t msbs = f.b[1] & 0x07;
  pkt[0] = (((f.b[0] >> 4) & 0x07) << 4) | (f.b[0] & 0x0F);
  pkt[1] = f.b[2] | ((msbs & 1) << 7);
  pkt[2] = f.b[3] | (((msbs >> 1) & 1) << 7);
  pkt[3] = f.b[4] | (((msbs >> 2) & 1) << 7);
  return (f.b[1] >> 3) & 0x0F;
}

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
    if (len < 5) return false;
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
