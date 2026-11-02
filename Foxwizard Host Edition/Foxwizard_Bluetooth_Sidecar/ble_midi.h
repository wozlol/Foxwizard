#pragma once

// Bluetooth MIDI (the MIDI over BLE spec) to and from the USB MIDI 1.0 event packets the rest of the FoxWizard uses. Header only
// and free of Arduino, so tests/ble_midi_test.cpp checks it on a computer.
//
// A BLE MIDI packet is one notification or write: a header byte (bit 7 set, bit 6 clear, then the top 6 bits of a 13 bit
// millisecond timestamp), then messages. Each message starts with a timestamp byte (bit 7 set, the low 7 bits) and a status byte,
// or just data bytes for running status. A SysEx can run on into the next packet, whose header is followed by data bytes at once,
// and ends with a timestamp byte and F7. Real time messages can sit inside a SysEx.

#include <stddef.h>
#include <stdint.h>

namespace fw {
namespace blemidi {

// Bytes in a USB MIDI packet for each Code Index Number, 0 = reserved.
constexpr uint8_t kCinBytes[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};

// BLE MIDI bytes to USB MIDI 1.0 packets. One per Bluetooth connection, because running status and a SysEx in progress belong to it.
class Parser {
 public:
  void reset() { running_ = 0; status_ = 0; have_ = 0; need_ = 0; inSysex_ = false; sxLen_ = 0; afterStamp_ = false; }

  // emit(const uint8_t packet[4]) is called for each packet made.
  template <typename Emit>
  void feed(const uint8_t *p, size_t n, Emit &&emit) {
    if (n == 0) return;
    size_t i = 0;
    if (p[0] & 0x80) i = 1; // the header
    afterStamp_ = false;
    for (; i < n; ++i) {
      const uint8_t b = p[i];
      if (b & 0x80) {
        if (!afterStamp_) { afterStamp_ = true; continue; } // a timestamp
        afterStamp_ = false;
        status(b, emit);
      } else if (inSysex_) {
        sysexByte(b, emit);
      } else {
        data(b, emit);
        afterStamp_ = false;
      }
    }
  }

 private:
  uint8_t running_ = 0;   // the last channel status, for running status
  uint8_t status_ = 0;    // the message being collected
  uint8_t data_[2] = {0, 0};
  uint8_t have_ = 0;
  uint8_t need_ = 0;
  bool inSysex_ = false;
  uint8_t sx_[3] = {0, 0, 0};
  uint8_t sxLen_ = 0;
  bool afterStamp_ = false;

  template <typename Emit>
  void sysexByte(uint8_t b, Emit &emit) {
    sx_[sxLen_++] = b;
    if (sxLen_ == 3) {
      const uint8_t pkt[4] = {0x04, sx_[0], sx_[1], sx_[2]};
      emit(pkt);
      sxLen_ = 0;
    }
  }

  template <typename Emit>
  void status(uint8_t b, Emit &emit) {
    if (b >= 0xF8) { // real time, even inside a SysEx
      const uint8_t pkt[4] = {0x0F, b, 0, 0};
      emit(pkt);
      return;
    }
    if (b == 0xF7) {
      if (inSysex_) { // the bytes collected, then F7, as 1 to 3 bytes
        uint8_t out[4] = {(uint8_t)(0x05 + sxLen_), 0, 0, 0};
        for (uint8_t k = 0; k < sxLen_; ++k) out[1 + k] = sx_[k];
        out[1 + sxLen_] = 0xF7;
        emit(out);
      }
      inSysex_ = false;
      sxLen_ = 0;
      have_ = need_ = 0;
      return;
    }
    if (b == 0xF0) {
      inSysex_ = true;
      sxLen_ = 0;
      sx_[sxLen_++] = 0xF0;
      running_ = 0;
      have_ = need_ = 0;
      return;
    }
    inSysex_ = false; // any other status cancels a SysEx that never ended
    sxLen_ = 0;
    status_ = b;
    have_ = 0;
    if (b < 0xF0) { // channel message
      running_ = b;
      need_ = (b >= 0xC0 && b <= 0xDF) ? 1 : 2;
    } else {
      running_ = 0;
      if (b == 0xF1 || b == 0xF3) need_ = 1;
      else if (b == 0xF2) need_ = 2;
      else if (b == 0xF6) need_ = 0;
      else { status_ = 0; need_ = 0; return; } // F4 and F5 are undefined
    }
    if (need_ == 0) finish(emit);
  }

  template <typename Emit>
  void data(uint8_t b, Emit &emit) {
    if (status_ == 0 || need_ == 0) { // running status, or a stray byte
      if (running_ == 0) return;
      status_ = running_;
      need_ = (running_ >= 0xC0 && running_ <= 0xDF) ? 1 : 2;
      have_ = 0;
    }
    data_[have_++] = b;
    if (have_ >= need_) finish(emit);
  }

  template <typename Emit>
  void finish(Emit &emit) {
    uint8_t cin;
    if (status_ < 0xF0) cin = status_ >> 4;
    else if (status_ == 0xF1 || status_ == 0xF3) cin = 0x2;
    else if (status_ == 0xF2) cin = 0x3;
    else cin = 0x5; // F6
    const uint8_t pkt[4] = {cin, status_, need_ > 0 ? data_[0] : (uint8_t)0, need_ > 1 ? data_[1] : (uint8_t)0};
    emit(pkt);
    have_ = 0;
    if (status_ >= 0xF0) { status_ = 0; need_ = 0; }
  }
};

// USB MIDI 1.0 packets to the bytes of one BLE MIDI packet. Add packets until add() says no room, send out(), clear(), go on.
// timestamp is the millisecond clock, only its low 13 bits are used.
class Writer {
 public:
  static constexpr uint16_t kMax = 512;

  void clear() { len_ = 0; }
  const uint8_t *bytes() const { return buf_; }
  uint16_t size() const { return len_; }
  bool empty() const { return len_ == 0; }
  // The most this packet can add: a stamp, three bytes, and for a SysEx end a second stamp and F7.
  void setLimit(uint16_t limit) { limit_ = limit > kMax ? kMax : limit; }

  // Returns false if the packet does not fit in what is left. A SysEx that began in an earlier packet carries on without a stamp.
  bool add(const uint8_t pkt[4], uint16_t timestamp) {
    const uint8_t cin = pkt[0] & 0x0F;
    const uint8_t count = kCinBytes[cin];
    if (count == 0) return true; // reserved, nothing to send
    const uint8_t hi = (uint8_t)(0x80 | ((timestamp >> 7) & 0x3F));
    const uint8_t lo = (uint8_t)(0x80 | (timestamp & 0x7F));
    uint8_t tmp[8];
    uint8_t n = 0;
    if (cin == 0x4) { // SysEx starts or goes on, three bytes
      if (pkt[1] == 0xF0) tmp[n++] = lo;
      tmp[n++] = pkt[1]; tmp[n++] = pkt[2]; tmp[n++] = pkt[3];
    } else if (cin == 0x5 || cin == 0x6 || cin == 0x7) {
      if (cin == 0x5 && pkt[1] != 0xF7) { // F6, tune request
        tmp[n++] = lo; tmp[n++] = pkt[1];
      } else { // a SysEx ends here, maybe one that also starts here
        uint8_t k = 1;
        if (pkt[1] == 0xF0) { tmp[n++] = lo; tmp[n++] = 0xF0; k = 2; }
        for (; k < count; ++k) tmp[n++] = pkt[k]; // the data bytes, the last byte of the packet is the F7
        tmp[n++] = lo; tmp[n++] = 0xF7;
      }
    } else { // channel, system common, real time
      tmp[n++] = lo;
      for (uint8_t k = 0; k < count; ++k) tmp[n++] = pkt[1 + k];
    }
    const uint16_t header = len_ == 0 ? 1 : 0;
    if (len_ + header + n > limit_) return false;
    if (header) buf_[len_++] = hi;
    for (uint8_t k = 0; k < n; ++k) buf_[len_++] = tmp[k];
    return true;
  }

 private:
  uint8_t buf_[kMax];
  uint16_t len_ = 0;
  uint16_t limit_ = 20;
};

}  // namespace blemidi
}  // namespace fw
