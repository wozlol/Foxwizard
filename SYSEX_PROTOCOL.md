# FoxWizard Mini MIDI Multitool, SysEx Protocol

IDENTITY_REPLY's `protocolVersion` says which version a board speaks:

- **1:** firmware 1.0-1.3. 41-byte routes.
- **2:** firmware 1.4 (Host Edition). 45-byte routes with port masks, and GET_STATUS for the sidecar.

Patchling picks the route size from it, so it talks to both.

Shared contract between the firmware (`Mini_MIDI_Multitool/Mini_MIDI_Multitool.ino`) and the
web app (`mmm_web_interface/js/midi.js`). If you change something here, change it in both places.

## Framing

```
F0 7D <deviceId> <cmd> [payload...] F7
```

- `7D` — MIDI Manufacturer ID reserved for non-commercial/educational use. No registration needed.
- `deviceId` = `0x4D` (`'M'`) — lets the device ignore other 0x7D traffic on a shared bus/host.
- `cmd` — one byte, see table below.
- `payload` — command-specific, always made of 7-bit-safe bytes (0x00–0x7F). Any raw 8-bit data
  (the 41-byte route struct) is wrapped with the **pack7/unpack7** encoding below before being
  placed in the payload. Plain scalar fields (indexes, steps, flags) are sent unpacked since
  they're already ≤0x7F or explicitly split into 7-bit chunks.

## pack7 / unpack7

Same scheme used by Roland/Yamaha bulk dumps: groups of up to 7 raw bytes become 8 sysex bytes.
Byte 0 of each group holds the high bit (bit 7) of each of the following up to 7 bytes, MSB-first;
the following bytes hold the low 7 bits of each raw byte.

```
pack7(raw[0..n)):
  for each group of up to 7 raw bytes:
    msbs = 0
    for i in 0..group.length: msbs |= ((group[i] >> 7) & 1) << i
    emit msbs
    for i in 0..group.length: emit group[i] & 0x7F

unpack7(packed[0..m)): inverse of the above, reading a leading msbs byte then up to 7 data bytes.
```

The route struct is 41 bytes → five groups of 7 + one group of 6 → packed to (5 × 8) + 7 = **47 bytes**.

## Commands

| cmd    | name                  | direction | payload (unpacked fields)                                   |
|--------|-----------------------|-----------|---------------------------------------------------------------|
| 0x01   | IDENTITY_REQUEST      | host→dev  | (none)                                                        |
| 0x02   | IDENTITY_REPLY        | dev→host  | protocolVersion, fwMajor, fwMinor, maxRoutes(2), name (ASCII, remainder of message) |
| 0x10   | GET_GLOBAL            | host→dev  | (none)                                                         |
| 0x13   | GET_STATUS            | host→dev  | (none). Protocol 2 |
| 0x14   | STATUS_DATA           | dev→host  | sidecarPresent (0/1), hostPortMask (bit0 = USB Host 1 has a USB MIDI device), sidecarDrops (saturates at 127), restartReason (0 none, 1 hang, 2 USB Host port stalled, 3 USB data sequence error, 4 USB buffer armed twice, 5 USB device stalled), restartPort (1-4, for reason 2), restartCount (recovery restarts since the sidecar powered up, saturates at 127). Protocol 2 |
| 0x11   | GLOBAL_DATA           | dev→host  | mode, buttonAction, buttonParamA, buttonParamB, buttonToggleMomentary, ledBrightnessStep |
| 0x12   | SET_GLOBAL            | host→dev  | same 6 bytes as GLOBAL_DATA                                   |
| 0x20   | GET_ROUTE_COUNT       | host→dev  | (none)                                                         |
| 0x21   | ROUTE_COUNT           | dev→host  | count(2)                                                       |
| 0x22   | GET_ROUTE             | host→dev  | index(2)                                                       |
| 0x23   | ROUTE_DATA            | dev→host  | index(2) + pack7(route struct, 41 bytes → 47 bytes)            |
| 0x24   | ADD_ROUTE             | host→dev  | pack7(route struct) — always appended at the end               |
| 0x25   | SET_ROUTE             | host→dev  | index(2) + pack7(route struct) — replaces in place, order unchanged |
| 0x26   | DELETE_ROUTE          | host→dev  | index(2) — removes and shifts subsequent routes down            |
| 0x27   | MOVE_ROUTE            | host→dev  | fromIndex(2) + toIndex(2) — same semantics as `Array.splice` reorder |
| 0x28   | CLEAR_ALL_ROUTES      | host→dev  | (none)                                                          |
| 0x30   | PREVIEW_BRIGHTNESS    | host→dev  | step(1, 0–9) — temporary, not persisted, LEDs flash then fade   |
| 0x31   | COMMIT                | host→dev  | (none) — writes current RAM state (global + all routes) to flash |
| 0x32   | FACTORY_RESET         | host→dev  | (none) — software-triggered equivalent of the boot-time button hold |
| 0x40   | FW_BEGIN              | host→dev  | size(3) + md5 (32 lowercase hex ASCII characters). Firmware 1.2+ |
| 0x41   | FW_DATA               | host→dev  | index(2) + pack7(up to 128 firmware bytes → up to 147 bytes). Firmware 1.2+ |
| 0x42   | FW_END                | host→dev  | (none). Checks size and MD5, stages the install, ACKs, then reboots. Firmware 1.2+ |
| 0x7E   | ACK                   | dev→host  | originalCmd(1), status(1: 0=OK,1=ERR_BAD_INDEX,2=ERR_BAD_DATA,3=ERR_FULL,4=ERR_UPDATE), checksum(1), extra(2, e.g. assigned index for ADD_ROUTE) |

16-bit values (`index`, `count`, `maxRoutes`) are sent as two 7-bit bytes: `low = v & 0x7F`,
`high = (v >> 7) & 0x7F`. Routes max out at 64 (firmware 1.0 reported 256).

Every host→device command other than GET_*/IDENTITY_REQUEST gets an ACK. The web app waits for the
matching ACK (with a timeout + one retry) before sending the next message, since bulk
operations are sequential round trips.

**Confirmation, not just "OK."** `checksum` is `(sum of the raw, unpacked payload bytes the device
actually parsed and applied) mod 128` — a 7-bit-safe running sum. For SET_GLOBAL it's the checksum
of the 6 global bytes just applied; for ADD_ROUTE/SET_ROUTE it's the checksum of the 41 raw route
bytes just stored (post-unpack7, i.e. what's actually sitting in the route slot now — not merely
"message parsed OK"). The web app computes the same checksum locally over what it *sent* and
compares it against the ACK's checksum. A mismatch (or a timeout) means the device's state doesn't
provably match what was sent, and the web app retries that message before moving on — this is what
gives the UI real certainty that the device has the new settings, not just that a command arrived.

## Firmware update (FW_BEGIN / FW_DATA / FW_END)

Firmware 1.2 and later. Patchling decides by the version in IDENTITY_REPLY, so 1.0 units never
receive these commands. Firmware 1.0 ignores them anyway.

- `size(3)` is three 7-bit bytes, low first: `size & 0x7F`, `(size >> 7) & 0x7F`, `(size >> 14) & 0x7F`.
- **FW_BEGIN** starts a new update, discarding any half-sent one. ACK checksum is `checksum7` of the
  35 payload bytes.
- **FW_DATA** carries chunk `index` (0, 1, 2, ...), 128 raw bytes each, the last one shorter.
  Chunks must arrive in order. ACK checksum is `checksum7` of the raw chunk, ACK `extra` is the
  index. A repeat of the last accepted chunk (its ACK was lost) is ACKed again without being
  written twice. A wrong index gets ERR_BAD_INDEX with the expected index in `extra`.
- **FW_END** checks that every byte arrived and the MD5 matches, stages the file for the OTA
  bootloader, ACKs, and reboots 300 ms later. The bootloader installs the new firmware during that
  reboot and the device re-enumerates. FW_END is never retried by the host, since a staged update
  refuses a second FW_END but still installs.
- ERR_UPDATE carries the Arduino `Updater` error code in `extra`.
- Nothing is installed until FW_END succeeds, so a failed or abandoned transfer leaves the running
  firmware untouched. Saved settings live in the EEPROM area, which the update never touches.
- The file is the sketch `.bin`, staged in the LittleFS partition, so the firmware must be built
  with Flash Size `2MB (Sketch: 1792KB, FS: 256KB)`. `mmm_web_interface/firmware/build_firmware.sh`
  does this.

## Terminology: Poly Aftertouch vs. Channel Pressure

MIDI has two unrelated messages both colloquially called "aftertouch," and this protocol (and the
web UI) is careful to distinguish them:

- **Poly Aftertouch** (a.k.a. Polyphonic Key Pressure, status `0xA0`) carries a **note number plus
  a pressure value** — each currently-held note can report independent pressure. Rare in real
  hardware (mostly MPE/high-end controllers).
- **Channel Pressure** (a.k.a. Channel Aftertouch, status `0xD0`) carries **one pressure value for
  the whole channel**, no note number. This is what most keyboards that advertise "aftertouch"
  actually send — a single sensor reading regardless of which key you're pressing.

The web UI labels these "Poly Aftertouch" and "Ch Pressure Aftertouch" respectively to keep them
from being confused with each other.

## Route struct (41 raw bytes in protocol 1, 45 in protocol 2, before pack7)

| byte  | field                    | notes |
|-------|--------------------------|-------|
| 0     | flags: bit7=enabled, bit6=port masks in use (protocol 2), bits1-0=inputDevice | inputDevice: 0=TRS, 1=USB, 2=Both. Used when bit6 is clear. Protocol 2 hosts still fill it in for older readers |
| 1     | inputChannelBitmask low 8   | bit0=ch1 … bit7=ch8 |
| 2     | inputChannelBitmask high 8  | bit0=ch9 … bit7=ch16 |
| 3     | dataType enable flags    | bit0=note, bit1=cc, bit2=programChange, bit3=pitchBend, bit4=poly aftertouch, bit5=channel pressure, bit6=sysex, bit7=poly aftertouch remapped to CC (see below) |
| 4     | noteRangeStart (0–127)   | only meaningful if bit0 of byte3 set |
| 5     | noteRangeEnd (0–127)     | |
| 6     | ccRangeStart (0–127)     | only meaningful if bit1 of byte3 set |
| 7     | ccRangeEnd (0–127)       | |
| 8     | outputDevice             | 0=None, 1=TRS, 2=USB, 3=Both. Used when byte0 bit6 is clear |
| 9     | outputChannelBitmask low 8  | |
| 10    | outputChannelBitmask high 8 | |
| 11    | transpose                | encoded as `semitone + 64`, range -64..+63 |
| 12    | velocityScale (10-200)   | percent applied to outgoing Note On velocity before the existing "never send 0" floor; 100 = unchanged. Note Off is unaffected. |
| 13    | ccMapStart (0–127)       | destination start CC for the mapped cc range; must satisfy `ccMapStart + (ccRangeEnd-ccRangeStart) <= 127` (enforced by the web UI) |
| 14    | atMapCC (0–127)          | destination CC number for remapped poly aftertouch, only meaningful if bit7 of byte3 is set (see below) |
| 15    | cpMapCC: bit7=channel pressure remapped to CC, bits6-0=destination CC number | same remap idea as byte14, but self-contained (its own enable bit) since channel pressure isn't one of the byte3 dataType flags' bit-per-flag scheme |
| 16    | flags2: bit0=Mono Retrig, bit1=Round Robin, bit2=System Common, bit3=System Realtime (Transport, everything but Clock), bit4=MIDI Clock | see "Mono Retrig / Round Robin" and "System Common / Realtime / Clock" below |
| 17-40 | name (24 bytes)          | ASCII, zero-padded (not necessarily NUL-terminated if it fills all 24 bytes). The device stores and returns this verbatim but never interprets it — it's purely a label. The web UI enforces the 24-character limit and restricts input to printable ASCII before ever building the wire bytes. |
| 41-42 | inPorts, little endian (protocol 2) | port mask, used when byte0 bit6 is set. Inputs are bits 0-6 |
| 43-44 | outPorts, little endian (protocol 2) | port mask, used when byte0 bit6 is set |

**Port masks** (protocol 2): bit0 TRS/DIN, bit1 USB Device, bits 2-5 USB Host 1-4 (sidecar hub
ports), bit6 TRS 2 (sidecar DIN IN and DIN OUT 1), bits 7-10 TRS 3-6 (sidecar DIN OUT 2-5, outputs
only). Bits 0-1 match the old inputDevice/outputDevice meaning, so a route with byte0 bit6 clear
(saved or sent by protocol 1) keeps working unchanged. A protocol 1 host sends 41 bytes, and the
firmware fills the missing port masks with zero.

Without a matching route, USB Adaptor Mode sends every input to every port except the one it came in
on, and Router Mode sends every input to every port. Sidecar ports count only while the sidecar is
present.

**Aftertouch → CC remap.** Normally a route with poly aftertouch enabled (byte3 bit4) passes it
through unmodified (same note number, pressure value becomes the CC-equivalent data byte). If byte3
bit7 is also set, the route instead emits the pressure value as a CC message on `atMapCC` — the note
number is dropped, and the output message's identity for dedup/fan-out purposes becomes
`(outputChannel, CC, atMapCC)` rather than `(outputChannel, Aftertouch, noteNumber)`, so it can
collide (and last-route-wins) with a route's ordinary CC output on the same number. The same idea
applies to channel pressure via byte15's own enable bit and `cpMapCC` — channel pressure only ever
carries one value with no note number to begin with, so the remap is even more direct: the single
pressure value becomes the CC value verbatim. The web UI shows both as an indented "AT to CC" switch
that only appears once the parent switch (Poly Aftertouch / Ch Pressure Aftertouch) is turned on.

**Velocity Scale** (byte12). A percent (10-200, 100 = unchanged) applied to outgoing Note On
velocity in `sendRouteNote()`, before the existing "never send velocity 0 as a Note On" floor —
scaling a soft hit down could otherwise round to 0, which running-status-aware receivers would read
as a Note Off. Note Off velocity is untouched (`sendRouteNote` always sends 0 for Note Off
regardless). Applies uniformly whether the note came from plain passthrough or a Mono Retrig
recall, since both funnel through the same function.

**Multiple held notes mapped through poly aftertouch → CC use note priority, not averaging.**
Collapsing several notes' independent pressure streams onto one CC destination needs *some* rule for
which note wins — naively forwarding whichever note's message arrived most recently would make the
output jump erratically as a chord's fingers move independently. Instead, a small per-channel
note-hold stack (`noteStacks[inputDevice][channel]`, depth 128 — the actual max distinct notes a
MIDI channel can ever have held at once, not a heuristic cap — in the firmware) tracks every currently
held note's own last-reported pressure, most-recently-triggered on top. Only the top note's pressure
drives the mapped CC. When that note releases, the CC immediately falls back to the next-held note's
own last-reported pressure (not a stale/frozen value — every held note's pressure is tracked
continuously, whether it's on top or not, from its own real incoming aftertouch messages). Once
nothing is held, the CC goes to 0. This mirrors ordinary "last note priority" behavior familiar from
mono synths, applied here to a single CC lane instead of a single voice.

## Mono Retrig / Round Robin (flags2 bits 0-1)

Both are Note-specific behaviors, handled entirely outside `applyRouteOutput` (which no longer
touches `MK_NOTE` at all — Note events are dispatched straight to `sendRouteNote()` or
`handleMonoRetrigNote()` from `processIncomingEvent`). Design mirrors the proven implementation in the
ARPnMIDI project (`legatoHeldCount`/`legatoHeldOrder`/`nextRoundRobinChannel` in its `max_main_brain`
sketch), adapted from ARPnMIDI's single global pipeline to per-route state here (`routeNoteTracks`,
depth 16 per route — melodic/mono-ish playing doesn't need one slot per possible note number the way
the aftertouch fix needed 128, and 256 routes × 128 slots would be a wasteful ~32KB regardless of
whether anything used it).

- **Mono Retrig** (flags2 bit0): poly→mono conversion with last-note priority. Every Note On for
  the route is tracked (note, velocity, a monotonic press-order counter) and sent through
  immediately; only the most-recently-pressed held note is ever the one "active." Releasing the
  active note sends its own Note Off, then — if another note is still held — a fresh Note On for
  whichever held note has the highest order value (the "recall"), so lifting a key snaps back to
  whatever you're still holding rather than going silent. Off-then-on (not overlapped) matches
  ARPnMIDI's own working implementation, not a from-scratch guess. Helps drive samplers/synths that
  don't have their own mono retrig mode behave more like an analog CV mono synth's last-note
  priority.
- **Round Robin** (flags2 bit1): each new Note On cycles to the next of the route's selected output
  channels (a rotating per-route cursor) instead of fanning out to all of them — spreads a
  mono-ish note stream across multiple channels/voices. The channel actually used is remembered per
  note (in the same `routeNoteTracks` slot) so that note's own Note Off targets the same channel,
  not wherever the cursor has since rotated to.

Both can be enabled on the same route (Mono Retrig decides *which* note is sounding, Round Robin
decides *which channel* it goes out on) or independently. `routeNoteTracks` is indexed by route
position, so it's fully reset on any structural route-list change (add/set/delete/move/clear/load) —
this only ever happens during an infrequent bulk Save-to-Device/Load-from-Device operation, never
live per-edit, so a brief reset of in-flight Mono Retrig/Round Robin state at that moment is
expected, not a bug.

## System Common / Realtime / Clock (flags2 bits 2-4)

Three MIDI message categories neither the routing engine nor the web UI previously exposed at all:

- **System Common** (`MK_COMMON`): MTC Quarter Frame (`0xF1`), Song Position Pointer (`0xF2`), Song
  Select (`0xF3`), Tune Request (`0xF6`). Before this, these were silently dropped on both DIN and
  USB — on DIN, the parser correctly buffered their data bytes but then called into the routing
  engine with `status & 0xF0`, which collapses `0xF1`/`0xF2`/`0xF3` all down to `0xF0` (not a case
  the router handles, so nothing happened); Tune Request's own zero-data-byte case never called
  through to anything at all; on USB, MIDI-USB CINs `0x02`/`0x03` (2-byte/3-byte System Common) had
  no handling and fell to the default no-op. All of that is now fixed as part of adding real routing
  support for these message types, not just a UI addition on top of already-working plumbing.
- **MIDI Clock** (`MK_CLOCK`): Clock (`0xF8`) only, split out from the rest of System Realtime.
  Unlike every other message here, Clock is continuous chatter during playback (24 messages per
  quarter note) rather than a rare one-off event, so it's worth gating independently of Start/Stop —
  you might want a device synced to clock without also passing it Stop/Reset, or vice versa.
- **System Realtime** (`MK_REALTIME`): everything else in the realtime range — Start (`0xFA`),
  Continue (`0xFB`), Stop (`0xFC`), Active Sensing (`0xFE`), System Reset (`0xFF`). This whole
  category (Clock included, before the split) *was* already forwarded before this feature existed —
  but unconditionally, via `applyModeDefault`, with no route ever getting a say and no way to turn
  it off. It now goes through the same route-matching (and same Mode-based fallthrough when no
  route claims it) as everything else.

None of the three carry a channel nibble or a note/CC number, so unlike every channel-addressed
kind: `routeMatches()` skips the input-channel-bitmask check entirely for all of them (same
treatment Sysex already got), and on the output side there's no per-channel fan-out or
`PendingMsg`/dedup table to speak of — `resolveDestination()` (shared with Sysex, just parameterized
by `MsgKind` now instead of being sysex-only) picks a single destination port(s) by
last-matching-route-wins, exactly mirroring how Sysex destination resolution already worked, and the
message is sent straight through via `forwardCommonOrRealtime()`, which classifies the status byte
into `MK_CLOCK`/`MK_REALTIME`/`MK_COMMON` itself so call sites just pass the raw byte through.

Fixing System Common's USB path also surfaced (and fixed) a latent bug in `sendUsbBytes()`: its
fallback case for any non-channel status byte used CINs `0x06`/`0x04` for 2-byte/3-byte messages —
those are actually *Sysex-continuation* CINs, correct only for the CIN 0x06/0x04 sysex-ending cases
they were originally written for, and would have mistagged a Song Position Pointer or similar as
part of whatever sysex stream happens to be in flight on the receiving end. Never triggered before
(nothing previously sent a non-channel message with `len` 2 or 3 through that path), so this was
harmless until System Common needed it — now corrected to the proper `0x02`/`0x03` System Common
CINs per the USB-MIDI spec.

Because every route defaults to *every* top-level input data type enabled (see "Defaults" below),
Common, Realtime, and Clock all pass through unmodified out of the box on a fresh route, same as
before this feature existed for Realtime — the difference is it's now an explicit, per-route,
turn-off-able choice instead of an unconditional firmware behavior.

## Defaults

New routes default to **every top-level input data type switch on** (Note Range, CC Range, Program
Change, Pitch Bend, Clock, Ch Pressure Aftertouch, Poly Aftertouch, Sysex, Common, Realtime — i.e.
`typeFlags = 0x7F` plus `flags2` bits 2-4 set). `typeFlags` bit7 (`aftertouchMap`, a sub-switch, not
top-level) and `flags2` bits 0-1 (Mono Retrig/Round Robin, Output-side, not Input) stay off. Both
Input and Output device default to Both, one channel selected each side. The intent is a
freshly-added route is immediately a working full passthrough
for whatever device/channel you've set, narrowed down from there — not an inert route that passes
nothing until manually configured. The web UI's per-route "All" switch next to the Input heading
flips every one of those top-level switches at once (checked whenever all of them currently are),
without touching sub-switches, range values, or the Output-side switches.

## Global struct (6 raw bytes, sent unpacked — small enough not to need pack7)

| byte | field                  | notes |
|------|------------------------|-------|
| 0    | mode                   | 0=USB Adaptor Mode, 1=Router Mode |
| 1    | buttonAction           | 0=panic, 1=pitchUp, 2=pitchDown, 3=mod, 4=ccMomentary, 5=ccToggle, 6=octaveUp, 7=octaveDown, 8=noteMomentary, 9=noteToggle |
| 2    | buttonParamA           | pitch/mod: ramp speed (0-127). 127 is an instant jump to the target value; 0 is the slowest ramp, taking 4.0s to sweep the full range, squared in between (seconds = 4.0 * ((127 - value) / 127)^2 — more resolution near the fast end). cc momentary/toggle: cc number. note momentary/toggle: note number (0-127). octave up/down: unused (0) |
| 3    | buttonParamB           | channel, 0-15 = ch1-16, 16 = All. unused (0) for panic |
| 4    | buttonToggleMomentary  | octave up/down only: 0=momentary, 1=toggle. unused elsewhere |
| 5    | ledBrightnessStep      | 0-10, representing 0%-100% of the balanced max levels (LED_OUT_MAX/LED_USB_MAX/LED_IN_MAX) in 10% increments. 0 is fully off. |

## Routing engine semantics

For every incoming MIDI event (from DIN-in or USB-in):

1. **Fan-out matching.** Walk the route list in order (index 0 → count-1). For each *enabled*
   route whose `inputDevice`, `inputChannelBitmask` (channel-addressed types only), and relevant
   `dataType` flag match — and whose note/cc number falls in range for note/cc types — compute the
   transformed output message(s) (transpose applied for notes, range-shifted CC number for cc,
   passthrough value otherwise). One route can still produce multiple messages if its
   `outputChannelBitmask` sets more than one channel.

2. **Last-route-wins dedup, per physical port.** `outputDevice=Both` is expanded into two
   independent candidate sends (one for the TRS wire, one for the USB wire) before deduping — fan-out
   to genuinely different destinations is never treated as a duplicate. Within each physical port's
   candidate stream, each computed output message has an *identity key* of
   `(outputChannel, messageKind, number)` — `number` is the note number for Note/Poly Aftertouch, the
   CC number for CC, and unused for ProgramChange/PitchBend/ChannelPressure/Sysex. Upsert each
   computed message into a scratch table keyed this way — a later-evaluated route (higher index)
   **overwrites** an earlier one with the same key *for that port*. This is what makes route order
   meaningful beyond fan-out, and is exactly why routes are reorderable in the UI (drag to reorder =
   change priority). After all routes are evaluated, transmit exactly one message per surviving
   table entry per port.

3. **Per-data-type fallthrough.** If **no enabled route matched** for a given (inputDevice,
   channel, dataType) combination, the event passes through unmodified per the current **Mode**:
   - **USB Adaptor Mode** (factory default): USB-in → DIN-out only. DIN-in → USB-out only. (This
     is exactly today's firmware behavior, unchanged.)
   - **Router Mode**: DIN-in → DIN-out *and* USB-out. USB-in → USB-out *and* DIN-out. (Full
     merge/broadcast — everything received on either port is echoed to both ports unless a custom
     route intercepts that specific data type.)

   Fallthrough is evaluated independently per data type — e.g. a route that only checks "cc" for
   a channel leaves that channel's notes, program changes, etc. to the default Mode behavior.

Sysex messages other than our own `F0 7D 4D …` framing are treated as data type "sysex" for
routing/fallthrough purposes and are never inspected/mutated, just forwarded whole.

## Persistence

Global struct + full route list live in RAM during operation (routes are looked at on every MIDI
event, so RAM residency matters for speed). `COMMIT` writes the current RAM state to EEPROM-backed
flash (rp2040 core's `EEPROM.h`, 4 KB maximum).

Saved layout `FOX2` (firmware 1.2 and later): magic(4), globalBytes(1), routeBytes(1), global,
routeCount(2), routes. The stored sizes let a later firmware with bigger structs read an older save.
It copies the bytes that were saved and leaves the rest zero, so new fields go at the end of their
struct and zero must mean off. `FOX1` (firmware 1.0) has no size bytes, a 6-byte global, and 41-byte
routes, and loads the same way. The first COMMIT rewrites it as `FOX2`.

64 routes leave each route room to grow from 41 to 63 bytes within 4 KB. The firmware's static_assert
fails the build if MAX_ROUTES times the route size stops fitting. Growing the route struct also
changes the wire format, which needs a PROTOCOL_VERSION bump. Patchling doesn't check
PROTOCOL_VERSION yet, so that change has to add the check.

Firmware 1.0 asked for more than 4 KB, which the core silently caps, so it only kept the first 99
routes after a restart. The web app now edits entirely locally and only talks to the
device when you explicitly click "Load from Device" or "Save to Device" — earlier revisions pushed
every field edit live, but that could race with itself (see the SET_GLOBAL/SET_ROUTE section above)
and made "have I actually saved this?" unclear. "Save to Device" pushes the full local state
(global + every route, in order) and then calls `COMMIT` so it's persisted to flash in one shot.

`FACTORY_RESET` (software) and the boot-time button-hold (hardware) both reset RAM to: Mode = USB
Adaptor, button = Panic, brightness step = 10 (100%), zero routes — then persist that to flash.
