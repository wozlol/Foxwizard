# MIDI 2.0 Plan

How the FoxWizard gets to MIDI 2.0 without breaking MIDI 1.0 gear.

## Where we are (firmware 1.115, host brain 8)

**What we can say: MIDI 2.0 ready, not MIDI 2.0 compatible.** The groundwork is in and tested. The USB port still reports MIDI 1.0, and nothing a MIDI 2.0 computer sees has changed.

| Step | Status |
|---|---|
| `ump.h`: UMP lengths, scaling, USB MIDI 1.0 to UMP and back, MIDI 2.0 to 1.0 translation (`Mini_MIDI_Multitool/tests/ump_test.cpp`) | Done, tested on a computer |
| The link between the brains carries UMP, 6-byte frames, a message is 1 to 4 words (`foxwizard_link.h`, `tests/link_test.cpp`) | Done, on hardware since 1.99 (host brain 8) |
| Each brain converts at its own edge, so everything inside stays MIDI 1.0 | Done |
| A MIDI 2.0 message arriving over the link for a MIDI 1.0 port is translated down by the spec's scaling | Done in the library, nothing sends one yet |
| Main brain USB device MIDI 2.0 class | Not started. Needs the vendored upstream TinyUSB, Adafruit 3.7.7 has no MIDI 2.0 classes |
| Sidecar MIDI 2.0 host class for MIDI 2.0 devices | Not started. The sidecar's TinyUSB has `midi2_host`. Needs a MIDI 2.0 device to test |
| Internal events at 16-bit velocity and 32-bit controllers | Not started. Everything inside is MIDI 1.0 (7-bit), the looper slot is 12 bytes |
| Groups in routes, per-port protocol display, MIDI-CI | Not started |

**Rules that keep MIDI 1.0 safe** (the fear is a unit that says MIDI 2.0 and then breaks MIDI 1.0 features):
- Inside the FoxWizard stays MIDI 1.0 until it is changed on purpose, one step at a time.
- The USB MIDI 2.0 device mode is a switch, off by default. Alt setting 0 stays byte-identical to today.
- The first MIDI 2.0 mode declares only the MIDI 1.0 protocol inside UMP.
- Holding the front button at power-up forces plain MIDI 1.0 USB, since Patchling needs USB MIDI to reach the unit.
- TRS and DIN are MIDI 1.0 only. The claim is MIDI 2.0 over USB, never over DIN.
- Branding: the specs are free to build against. The MIDI 2.0 logo needs the MIDI Association's logo license and its checklist. Until then the words are "MIDI 1.0 today, MIDI 2.0 planned", and "MIDI 2.0 ready" for the groundwork above.

**What the main brain can still spare** (measured, see `DIAGNOSTICS.md`): RAM 83% used with 44 KB of heap free and the lowest it has been 29 KB. The main stack is 2 KB and has reached 1,200 bytes. The saved settings image is 4,024 of 4,096 bytes at the worst case. Flash is 18% used. The upstream TinyUSB swap and wider events each need a fresh measurement before and after, and a floor of 20 KB free heap and 500 bytes of free stack.

**A lesson that applies to the MIDI 2.0 USB code:** call TinyUSB in batches. One call per 4-byte packet starts a USB transfer each time, and under a flood the unit stalled for 4 seconds until its watchdog restarted it (fixed in 1.111 and 1.113 with `tud_midi_n_packet_write_n` and `tud_midi_n_packet_read_n`). The UMP streams of the MIDI 2.0 class must be read and written the same way.

## What MIDI 2.0 adds

- **UMP (Universal MIDI Packet):** 32, 64 and 128-bit packets in place of MIDI 1.0 bytes.
- **16 groups of 16 channels,** so 256 channels on one connection.
- **Higher resolution:** 16-bit velocity, 32-bit controllers and pitch bend.
- **Per-note controllers and per-note pitch bend.**
- **Jitter-reduction timestamps.**
- **MIDI-CI:** devices discover each other, agree on a protocol, and share profiles and properties.
- MIDI 1.0 stays valid inside UMP, so every MIDI 2.0 device also speaks MIDI 1.0.

## Where each port stands

| Port | MIDI 2.0 path |
|---|---|
| USB device (to the computer) | USB MIDI 2.0 class: alt setting 0 is MIDI 1.0, alt setting 1 is native UMP. The host picks. |
| USB host (sidecar hub ports) | USB MIDI 2.0 host class, the same two alt settings, per device. |
| TRS and DIN | MIDI 1.0 only. No MIDI 2.0 transport is defined for 5-pin or TRS. |
| Link between the brains | A FoxWizard protocol, free to carry UMP. |

## Software already in hand

- The sidecar's vendored TinyUSB (hathach/tinyusb 4c195b7) includes `midi2_host` and `midi2_device`, validated on RP2040.
- The main brain uses Adafruit TinyUSB 3.7.7, which has no MIDI 2.0 classes. It would need the same vendored upstream copy the sidecar uses.

## Plan

### 1. Internal events at MIDI 2.0 resolution

- One internal note and controller event: group, channel, 16-bit velocity, 32-bit value, optional per-note attribute.
- MIDI 1.0 input is scaled up on arrival, using the MIDI Association's default translation.
- MIDI 1.0 outputs scale back down on the way out.
- Routes, Scale, Auto Chord, arp, Echo, Stutter and the looper all work on these events, so nothing is lost when both ends speak MIDI 2.0.
- The looper stores 16-bit velocity. Its 12-byte event slot has room once the tag is narrowed.

### 2. Link frames carry UMP

- The 5-byte link frame becomes a UMP word plus port and group.
- The sidecar stops converting to MIDI 1.0 for MIDI 2.0 devices.

### 3. Per-port protocol

- Each port remembers whether it is talking MIDI 1.0 or 2.0, from USB alt setting negotiation and MIDI-CI.
- TRS and DIN are always MIDI 1.0.
- Patchling shows the protocol on each port chip.

### 4. Routes gain a group

- A route's input and output channels become group plus channel, defaulting to group 1, so saved routes keep working.
- The route format grows using its spare bytes, with a protocol version bump, as the feature blocks did.

### 5. USB device side

- The main brain moves to vendored upstream TinyUSB and enables the MIDI 2.0 device class.
- It offers both alt settings, so older computers still see a MIDI 1.0 FoxWizard.
- Endpoint Discovery and Function Blocks name each FoxWizard port to the computer.

### 6. MIDI-CI

- Answer Discovery, Protocol Negotiation and Endpoint Info.
- Property Exchange could later carry Patchling's settings in place of the FoxWizard sysex protocol.

## Order

1. Internal events at full resolution, MIDI 1.0 everywhere still. Testable with today's gear. **Not started.**
2. Vendored upstream TinyUSB on the main brain, MIDI 1.0 class only. Same behavior, new stack.
3. USB MIDI 2.0 device class on the main brain.
4. UMP over the link (**done**), and USB MIDI 2.0 host on the sidecar (not started).
5. Groups on routes, in firmware and Patchling.
6. MIDI-CI discovery and negotiation.

## Costs

- **RAM:** wider events grow the looper, Echo and Stutter memory. The looper pool shrinks or slots grow to 16 bytes.
- **Flash:** the MIDI 2.0 classes and MIDI-CI add code, well within the 2 MB flash.
- **Testing:** needs a MIDI 2.0 host (macOS 14 or later, or Windows MIDI Services) and a MIDI 2.0 controller.
