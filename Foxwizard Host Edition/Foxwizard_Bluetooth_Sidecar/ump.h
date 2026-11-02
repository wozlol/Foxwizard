#pragma once

// UMP, the Universal MIDI Packet of MIDI 2.0, and its translations to and from the USB MIDI 1.0 event packets the
// FoxWizard uses today. Header only, no memory allocation, and the same file is in both brains (the sidecar keeps a copy
// beside its sketch, like foxwizard_link.h).
//
// What's here is the current spec: UMP Message Types and their lengths, the USB MIDI 1.0 packet to UMP conversion in
// both directions (channel voice, system, and 7-bit SysEx), and the MIDI 1.0 / MIDI 2.0 Protocol translation for channel
// voice messages with the specified scaling. Message types this doesn't know are still carried whole, because a UMP
// message's length comes from its first word.

#include <stdint.h>

namespace fw {
namespace ump {

enum MessageType : uint8_t {
  kUtility = 0x0, kSystem = 0x1, kMidi1ChannelVoice = 0x2, kSysex7 = 0x3, kMidi2ChannelVoice = 0x4,
  kData128 = 0x5, kFlexData = 0xD, kStream = 0xF
};

// Number of 32-bit words in the message whose first word is w, from its Message Type.
inline uint8_t wordCount(uint32_t w) {
  switch (w >> 28) {
    case 0x0: case 0x1: case 0x2: case 0x6: case 0x7: return 1;
    case 0x3: case 0x4: case 0x8: case 0x9: case 0xA: return 2;
    case 0xB: case 0xC: return 3;
    default: return 4; // 0x5, 0xD, 0xE, 0xF
  }
}

inline uint8_t messageType(uint32_t w) { return (uint8_t)(w >> 28); }
inline uint8_t group(uint32_t w) { return (uint8_t)((w >> 24) & 0x0F); }

// ---- Scaling between bit widths, the spec's min-center-max method -------------------------------------------------------

// Up: the value's centre stays the centre and its maximum stays the maximum.
inline uint32_t scaleUp(uint32_t value, uint8_t srcBits, uint8_t dstBits) {
  const uint8_t scaleBits = (uint8_t)(dstBits - srcBits);
  uint32_t shifted = value << scaleBits;
  const uint32_t srcCenter = 1u << (srcBits - 1);
  if (value <= srcCenter) return shifted;
  const uint8_t repeatBits = (uint8_t)(srcBits - 1);
  const uint32_t repeatValue = value & ((1u << repeatBits) - 1u);
  uint32_t repeatShifted = scaleBits > repeatBits ? repeatValue << (scaleBits - repeatBits) : repeatValue >> (repeatBits - scaleBits);
  while (repeatShifted != 0) {
    shifted |= repeatShifted;
    repeatShifted >>= repeatBits;
  }
  return shifted;
}

// Down: the top bits.
inline uint32_t scaleDown(uint32_t value, uint8_t srcBits, uint8_t dstBits) { return value >> (srcBits - dstBits); }

// ---- USB MIDI 1.0 event packets to UMP ----------------------------------------------------------------------------------

inline uint32_t makeMidi1(uint8_t g, uint8_t status, uint8_t d1, uint8_t d2) {
  return (0x2u << 28) | ((uint32_t)(g & 0x0F) << 24) | ((uint32_t)status << 16) | ((uint32_t)d1 << 8) | d2;
}
inline uint32_t makeSystem(uint8_t g, uint8_t status, uint8_t d1, uint8_t d2) {
  return (0x1u << 28) | ((uint32_t)(g & 0x0F) << 24) | ((uint32_t)status << 16) | ((uint32_t)d1 << 8) | d2;
}

// Takes USB MIDI 1.0 event packets one at a time and returns the UMP words they make. The cable number is the group.
// SysEx is gathered into UMP 7-bit packets of up to 6 data bytes per group, so a message can come out a little after its
// last packet went in. Never more than 4 words out for one packet in.
struct UsbMidiToUmp {
  uint8_t buffer[16][6];
  uint8_t count[16];     // data bytes waiting, 0-6
  bool open[16];         // inside a SysEx message
  bool started[16];      // its start packet has gone out

  void reset() {
    for (uint8_t g = 0; g < 16; ++g) { count[g] = 0; open[g] = false; started[g] = false; }
  }

  static uint32_t sysexWord0(uint8_t g, uint8_t status, uint8_t n, const uint8_t *d) {
    return (0x3u << 28) | ((uint32_t)g << 24) | ((uint32_t)status << 20) | ((uint32_t)n << 16) |
           ((uint32_t)(n > 0 ? d[0] : 0) << 8) | (n > 1 ? d[1] : 0);
  }
  static uint32_t sysexWord1(uint8_t n, const uint8_t *d) {
    uint32_t w = 0;
    for (uint8_t i = 2; i < 6; ++i) w |= (uint32_t)(i < n ? d[i] : 0) << (24 - 8 * (i - 2));
    return w;
  }

  // One SysEx packet out. status: 0 complete, 1 start, 2 continue, 3 end.
  uint8_t emit(uint8_t g, uint8_t status, uint32_t *out) {
    out[0] = sysexWord0(g, status, count[g], buffer[g]);
    out[1] = sysexWord1(count[g], buffer[g]);
    count[g] = 0;
    return 2;
  }

  uint8_t pushSysexByte(uint8_t g, uint8_t b, uint32_t *out) {
    uint8_t n = 0;
    if (b == 0xF0) { // a start inside a message abandons it
      open[g] = true; started[g] = false; count[g] = 0;
      return 0;
    }
    if (b == 0xF7) {
      if (!open[g]) return 0;
      n = emit(g, started[g] ? 3 : 0, out);
      open[g] = false; started[g] = false;
      return n;
    }
    if (!open[g]) return 0;
    if (count[g] == 6) { // the 6 waiting aren't the last, so they go out now
      n = emit(g, started[g] ? 2 : 1, out);
      started[g] = true;
    }
    buffer[g][count[g]++] = b;
    return n;
  }

  uint8_t push(const uint8_t pkt[4], uint32_t out[4]) {
    const uint8_t cin = pkt[0] & 0x0F;
    const uint8_t g = (pkt[0] >> 4) & 0x0F;
    switch (cin) {
      case 0x8: case 0x9: case 0xA: case 0xB: case 0xE: // 3 byte channel voice
        out[0] = makeMidi1(g, pkt[1], pkt[2], pkt[3]);
        return 1;
      case 0xC: case 0xD: // 2 byte channel voice
        out[0] = makeMidi1(g, pkt[1], pkt[2], 0);
        return 1;
      case 0x2: // 2 byte system common
        out[0] = makeSystem(g, pkt[1], pkt[2], 0);
        return 1;
      case 0x3: // 3 byte system common
        out[0] = makeSystem(g, pkt[1], pkt[2], pkt[3]);
        return 1;
      case 0xF: // single byte, real time
        out[0] = makeSystem(g, pkt[1], 0, 0);
        return 1;
      case 0x4: case 0x5: case 0x6: case 0x7: { // SysEx start or continue (4), ends with 1, 2, 3 bytes
        if (cin == 0x5 && pkt[1] >= 0xF1 && pkt[1] != 0xF7) { // a single byte system common message, F6 tune request
          out[0] = makeSystem(g, pkt[1], 0, 0);
          return 1;
        }
        const uint8_t bytes = cin == 0x4 ? 3 : (uint8_t)(cin - 0x4);
        uint8_t total = 0;
        for (uint8_t i = 0; i < bytes; ++i) {
          uint32_t made[4];
          const uint8_t n = pushSysexByte(g, pkt[1 + i], made);
          for (uint8_t k = 0; k < n; ++k) out[total + k] = made[k];
          total += n;
        }
        return total;
      }
      default:
        return 0; // 0 and 1 are reserved, 0xF handled
    }
  }
};

// ---- UMP to USB MIDI 1.0 event packets ----------------------------------------------------------------------------------

inline uint8_t systemPacketCin(uint8_t status) {
  switch (status) {
    case 0xF1: case 0xF3: return 0x2;
    case 0xF2: return 0x3;
    case 0xF6: return 0x5;
    default: return 0xF; // real time
  }
}

// Takes whole UMP messages and gives USB MIDI 1.0 event packets. MIDI 2.0 channel voice messages are translated down first.
// A SysEx can leave up to 2 bytes waiting for the packet that completes the next three, per group.
struct UmpToUsbMidi {
  uint8_t spare[16][2];
  uint8_t spareCount[16];
  bool open[16];

  void reset() {
    for (uint8_t g = 0; g < 16; ++g) { spareCount[g] = 0; open[g] = false; }
  }

  // Appends SysEx bytes in threes. final writes the closing packet too.
  uint8_t sysexBytes(uint8_t g, const uint8_t *data, uint8_t n, bool first, bool last, uint8_t out[][4]) {
    uint8_t stream[12];
    uint8_t len = 0;
    for (uint8_t i = 0; i < spareCount[g]; ++i) stream[len++] = spare[g][i];
    spareCount[g] = 0;
    if (first) { // F0 leads the message
      // spare is always empty at a start, but keep the order right if a message was abandoned
      len = 0;
      stream[len++] = 0xF0;
    }
    for (uint8_t i = 0; i < n; ++i) stream[len++] = data[i];
    if (last) stream[len++] = 0xF7;
    uint8_t packets = 0, at = 0;
    const uint8_t cable = (uint8_t)(g << 4);
    while (len - at >= 3 && !(last && len - at == 3)) {
      out[packets][0] = cable | 0x4; out[packets][1] = stream[at]; out[packets][2] = stream[at + 1]; out[packets][3] = stream[at + 2];
      at += 3; ++packets;
    }
    const uint8_t rest = (uint8_t)(len - at);
    if (last) {
      out[packets][0] = cable | (uint8_t)(0x4 + rest); // 5, 6 or 7
      out[packets][1] = stream[at]; out[packets][2] = rest > 1 ? stream[at + 1] : 0; out[packets][3] = rest > 2 ? stream[at + 2] : 0;
      ++packets;
      open[g] = false;
    } else {
      for (uint8_t i = 0; i < rest; ++i) spare[g][i] = stream[at + i];
      spareCount[g] = rest;
      open[g] = true;
    }
    return packets;
  }

  // The MIDI 1.0 messages a MIDI 2.0 channel voice message turns into, as type 2 words. 0 if it has no MIDI 1.0 form.
  static uint8_t midi2ToMidi1(const uint32_t w[2], uint32_t out[6]) {
    const uint8_t g = group(w[0]);
    const uint8_t status = (uint8_t)(w[0] >> 16);
    const uint8_t opcode = status >> 4, ch = status & 0x0F;
    const uint8_t b1 = (uint8_t)(w[0] >> 8), b0 = (uint8_t)w[0];
    switch (opcode) {
      case 0x9: { // Note On, velocity 0 stays a note, so it goes down to 1
        uint8_t v = (uint8_t)scaleDown(w[1] >> 16, 16, 7);
        if (v == 0) v = 1;
        out[0] = makeMidi1(g, 0x90 | ch, b1, v);
        return 1;
      }
      case 0x8:
        out[0] = makeMidi1(g, 0x80 | ch, b1, (uint8_t)scaleDown(w[1] >> 16, 16, 7));
        return 1;
      case 0xA:
        out[0] = makeMidi1(g, 0xA0 | ch, b1, (uint8_t)scaleDown(w[1], 32, 7));
        return 1;
      case 0xB:
        out[0] = makeMidi1(g, 0xB0 | ch, b1, (uint8_t)scaleDown(w[1], 32, 7));
        return 1;
      case 0xD:
        out[0] = makeMidi1(g, 0xD0 | ch, (uint8_t)scaleDown(w[1], 32, 7), 0);
        return 1;
      case 0xE: {
        const uint32_t v = scaleDown(w[1], 32, 14);
        out[0] = makeMidi1(g, 0xE0 | ch, (uint8_t)(v & 0x7F), (uint8_t)(v >> 7));
        return 1;
      }
      case 0xC: { // Program Change, with its bank as Bank Select first if the bank is valid
        uint8_t n = 0;
        if (b0 & 0x01) {
          out[n++] = makeMidi1(g, 0xB0 | ch, 0, (uint8_t)((w[1] >> 8) & 0x7F));
          out[n++] = makeMidi1(g, 0xB0 | ch, 32, (uint8_t)(w[1] & 0x7F));
        }
        out[n++] = makeMidi1(g, 0xC0 | ch, (uint8_t)((w[1] >> 24) & 0x7F), 0);
        return n;
      }
      case 0x2: case 0x3: { // RPN, NRPN: four controller changes
        const uint8_t msb = opcode == 0x2 ? 101 : 99, lsb = opcode == 0x2 ? 100 : 98;
        const uint32_t v14 = scaleDown(w[1], 32, 14);
        out[0] = makeMidi1(g, 0xB0 | ch, msb, b1 & 0x7F);
        out[1] = makeMidi1(g, 0xB0 | ch, lsb, b0 & 0x7F);
        out[2] = makeMidi1(g, 0xB0 | ch, 6, (uint8_t)(v14 >> 7));
        out[3] = makeMidi1(g, 0xB0 | ch, 38, (uint8_t)(v14 & 0x7F));
        return 4;
      }
      default:
        return 0; // per-note controllers, per-note pitch bend, per-note management, relative controllers
    }
  }

  static uint8_t midi1WordToPacket(uint32_t w, uint8_t pkt[4]) {
    const uint8_t cable = (uint8_t)(group(w) << 4);
    const uint8_t status = (uint8_t)(w >> 16), d1 = (uint8_t)(w >> 8), d2 = (uint8_t)w;
    const uint8_t opcode = status >> 4;
    pkt[0] = cable | opcode; pkt[1] = status; pkt[2] = d1; pkt[3] = (opcode == 0xC || opcode == 0xD) ? 0 : d2;
    return 1;
  }

  // Up to 8 packets out for one message in. words is the whole message, n its word count.
  uint8_t push(const uint32_t *words, uint8_t n, uint8_t out[][4]) {
    const uint32_t w = words[0];
    const uint8_t g = group(w);
    switch (messageType(w)) {
      case kMidi1ChannelVoice:
        return midi1WordToPacket(w, out[0]);
      case kSystem: {
        const uint8_t status = (uint8_t)(w >> 16);
        out[0][0] = (uint8_t)(g << 4) | systemPacketCin(status); out[0][1] = status; out[0][2] = (uint8_t)(w >> 8);
        out[0][3] = (uint8_t)w;
        if (systemPacketCin(status) == 0x5 || systemPacketCin(status) == 0xF) out[0][2] = out[0][3] = 0;
        if (systemPacketCin(status) == 0x2) out[0][3] = 0;
        return 1;
      }
      case kMidi2ChannelVoice: {
        if (n < 2) return 0;
        uint32_t m1[6];
        const uint8_t made = midi2ToMidi1(words, m1);
        for (uint8_t i = 0; i < made; ++i) midi1WordToPacket(m1[i], out[i]);
        return made;
      }
      case kSysex7: {
        if (n < 2) return 0;
        const uint8_t status = (uint8_t)((w >> 20) & 0x0F), bytes = (uint8_t)((w >> 16) & 0x0F);
        uint8_t data[6] = {(uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(words[1] >> 24), (uint8_t)(words[1] >> 16),
                           (uint8_t)(words[1] >> 8), (uint8_t)words[1]};
        const uint8_t count = bytes > 6 ? 6 : bytes;
        return sysexBytes(g, data, count, status == 0 || status == 1, status == 0 || status == 3, out);
      }
      default:
        return 0; // utility, flex data, data 128 and stream messages aren't MIDI 1.0
    }
  }
};

// ---- MIDI 1.0 channel voice up to MIDI 2.0, for a port that talks the MIDI 2.0 Protocol -----------------------------------

// A type 2 word to the type 4 words. Returns 2, or 0 if it has no MIDI 2.0 form here (System messages stay as they are).
inline uint8_t midi1ToMidi2(uint32_t w, uint32_t out[2]) {
  if (messageType(w) != kMidi1ChannelVoice) return 0;
  const uint8_t g = group(w), status = (uint8_t)(w >> 16), d1 = (uint8_t)(w >> 8), d2 = (uint8_t)w;
  const uint8_t opcode = status >> 4, ch = status & 0x0F;
  auto head = [&](uint8_t op, uint8_t i1, uint8_t i0) {
    return (0x4u << 28) | ((uint32_t)g << 24) | ((uint32_t)((op << 4) | ch) << 16) | ((uint32_t)i1 << 8) | i0;
  };
  switch (opcode) {
    case 0x9:
      if (d2 == 0) { out[0] = head(0x8, d1, 0); out[1] = 0x8000u << 16; return 2; } // a Note On with velocity 0 is a Note Off, at velocity 0x8000 (64) as the spec says
      out[0] = head(0x9, d1, 0); out[1] = scaleUp(d2, 7, 16) << 16; return 2;
    case 0x8:
      out[0] = head(0x8, d1, 0); out[1] = scaleUp(d2, 7, 16) << 16; return 2;
    case 0xA: out[0] = head(0xA, d1, 0); out[1] = scaleUp(d2, 7, 32); return 2;
    case 0xB: out[0] = head(0xB, d1, 0); out[1] = scaleUp(d2, 7, 32); return 2;
    case 0xC: out[0] = head(0xC, 0, 0); out[1] = (uint32_t)(d1 & 0x7F) << 24; return 2;
    case 0xD: out[0] = head(0xD, 0, 0); out[1] = scaleUp(d1, 7, 32); return 2;
    case 0xE: out[0] = head(0xE, 0, 0); out[1] = scaleUp((uint32_t)(d1 & 0x7F) | ((uint32_t)(d2 & 0x7F) << 7), 14, 32); return 2;
    default: return 0;
  }
}

}  // namespace ump
}  // namespace fw
