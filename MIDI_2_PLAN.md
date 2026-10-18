# MIDI 2.0 Plan

How the FoxWizard gets to MIDI 2.0 without breaking MIDI 1.0 gear.

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

1. Internal events at full resolution, MIDI 1.0 everywhere still. Testable with today's gear.
2. Vendored upstream TinyUSB on the main brain, MIDI 1.0 class only. Same behavior, new stack.
3. USB MIDI 2.0 device class on the main brain.
4. UMP over the link, and USB MIDI 2.0 host on the sidecar.
5. Groups on routes, in firmware and Patchling.
6. MIDI-CI discovery and negotiation.

## Costs

- **RAM:** wider events grow the looper, Echo and Stutter memory. The looper pool shrinks or slots grow to 16 bytes.
- **Flash:** the MIDI 2.0 classes and MIDI-CI add code, well within the 2 MB flash.
- **Testing:** needs a MIDI 2.0 host (macOS 14 or later, or Windows MIDI Services) and a MIDI 2.0 controller.
