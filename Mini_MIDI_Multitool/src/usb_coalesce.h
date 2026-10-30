#pragma once

// FoxWizard 1.125, fewer USB transfers for controller data. Every packet used to go to TinyUSB the moment it was queued, which starts a
// USB transfer for each one: 1,000 messages a second were 1,000 transfers a second, and with a flood coming in at the same time the unit
// stalled inside TinyUSB until the watchdog restarted it (see DIAGNOSTICS.md, known limit). USB has bandwidth to spare, so nothing is thinned.
// Controller data (pitch bend, pressure, poly pressure, most CCs) waits up to kUsbCoalesceUs for company and goes out in one transfer. Anything
// else, a note, program change, system message, a CC that must not wait (bank select, pedals, channel mode), or any real time packet, sends
// everything waiting at once. Order is kept, nothing is dropped, and the wait never grows past the first packet's deadline. Plain C++ so a
// computer can test it.

#include <stdint.h>

namespace fw {

constexpr uint32_t kUsbCoalesceUs = 2000;

// A USB MIDI event packet: byte 0 cable and code index, 1 status, 2 and 3 data. True for the controller data that may wait.
inline bool usbPacketIsContinuous(const uint8_t packet[4]) {
  const uint8_t status = packet[1] & 0xF0;
  if (status == 0xA0 || status == 0xD0 || status == 0xE0) return true; // poly pressure, channel pressure, pitch bend
  if (status != 0xB0) return false;
  const uint8_t cc = packet[2];
  if (cc == 0 || cc == 32) return false;       // bank select, ahead of a program change
  if (cc >= 64 && cc <= 69) return false;      // pedals and sostenuto, on or off, followed by a note
  if (cc >= 120) return false;                 // channel mode: all sound off, all notes off, reset
  return true;
}

struct UsbCoalescer {
  uint32_t holdUntilUs = 0; // 0 when no wait is under way, else the time the first waiting packet must go by
  bool urgent = false;      // something that may not wait is queued

  // A packet was put in the queue.
  void packetQueued(bool continuous, uint32_t nowUs) {
    if (!continuous) { urgent = true; return; }
    if (holdUntilUs == 0 && !urgent) holdUntilUs = (nowUs + kUsbCoalesceUs) | 1u; // |1 keeps it from reading as no wait
  }

  // Whether the queue may go to TinyUSB now. Real time packets never wait, and the packets queued with them go as well.
  bool mayWrite(uint32_t nowUs, bool realtimeWaiting) const {
    if (urgent || realtimeWaiting || holdUntilUs == 0) return true;
    return (int32_t)(nowUs - holdUntilUs) >= 0;
  }

  // The queue went empty: the next controller packet starts a new wait.
  void queueEmptied() { holdUntilUs = 0; urgent = false; }
};

}  // namespace fw
