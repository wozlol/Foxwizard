# Foxwizard Mini MIDI Multitool — SysEx Protocol v1

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
  (the 37-byte route struct) is wrapped with the **pack7/unpack7** encoding below before being
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

The route struct is 37 bytes → five groups of 7 + one group of 2 → packed to (5 × 8) + 3 = **43 bytes**.

## Commands

| cmd    | name                  | direction | payload (unpacked fields)                                   |
|--------|-----------------------|-----------|---------------------------------------------------------------|
| 0x01   | IDENTITY_REQUEST      | host→dev  | (none)                                                        |
| 0x02   | IDENTITY_REPLY        | dev→host  | protocolVersion, fwMajor, fwMinor, maxRoutes(2), name (ASCII, remainder of message) |
| 0x10   | GET_GLOBAL            | host→dev  | (none)                                                         |
| 0x11   | GLOBAL_DATA           | dev→host  | mode, buttonAction, buttonParamA, buttonParamB, buttonToggleMomentary, ledBrightnessStep |
| 0x12   | SET_GLOBAL            | host→dev  | same 6 bytes as GLOBAL_DATA                                   |
| 0x20   | GET_ROUTE_COUNT       | host→dev  | (none)                                                         |
| 0x21   | ROUTE_COUNT           | dev→host  | count(2)                                                       |
| 0x22   | GET_ROUTE             | host→dev  | index(2)                                                       |
| 0x23   | ROUTE_DATA            | dev→host  | index(2) + pack7(route struct, 13 bytes → 15 bytes)            |
| 0x24   | ADD_ROUTE             | host→dev  | pack7(route struct) — always appended at the end               |
| 0x25   | SET_ROUTE             | host→dev  | index(2) + pack7(route struct) — replaces in place, order unchanged |
| 0x26   | DELETE_ROUTE          | host→dev  | index(2) — removes and shifts subsequent routes down            |
| 0x27   | MOVE_ROUTE            | host→dev  | fromIndex(2) + toIndex(2) — same semantics as `Array.splice` reorder |
| 0x28   | CLEAR_ALL_ROUTES      | host→dev  | (none)                                                          |
| 0x30   | PREVIEW_BRIGHTNESS    | host→dev  | step(1, 0–9) — temporary, not persisted, LEDs flash then fade   |
| 0x31   | COMMIT                | host→dev  | (none) — writes current RAM state (global + all routes) to flash |
| 0x32   | FACTORY_RESET         | host→dev  | (none) — software-triggered equivalent of the boot-time button hold |
| 0x7E   | ACK                   | dev→host  | originalCmd(1), status(1: 0=OK,1=ERR_BAD_INDEX,2=ERR_BAD_DATA,3=ERR_FULL), checksum(1), extra(2, e.g. assigned index for ADD_ROUTE) |

16-bit values (`index`, `count`, `maxRoutes`) are sent as two 7-bit bytes: `low = v & 0x7F`,
`high = (v >> 7) & 0x7F`. Since routes max out at 256, `high` never exceeds 2.

Every host→device command other than GET_*/IDENTITY_REQUEST gets an ACK. The web app waits for the
matching ACK (with a timeout + one retry) before sending the next message, since 256-route bulk
operations are sequential round trips.

**Confirmation, not just "OK."** `checksum` is `(sum of the raw, unpacked payload bytes the device
actually parsed and applied) mod 128` — a 7-bit-safe running sum. For SET_GLOBAL it's the checksum
of the 6 global bytes just applied; for ADD_ROUTE/SET_ROUTE it's the checksum of the 13 raw route
bytes just stored (post-unpack7, i.e. what's actually sitting in the route slot now — not merely
"message parsed OK"). The web app computes the same checksum locally over what it *sent* and
compares it against the ACK's checksum. A mismatch (or a timeout) means the device's state doesn't
provably match what was sent, and the web app retries that message before moving on — this is what
gives the UI real certainty that the device has the new settings, not just that a command arrived.

## Route struct (37 raw bytes, before pack7)

| byte  | field                    | notes |
|-------|--------------------------|-------|
| 0     | flags: bit7=enabled, bits1-0=inputDevice | inputDevice: 0=TRS, 1=USB, 2=Both |
| 1     | inputChannelBitmask low 8   | bit0=ch1 … bit7=ch8 |
| 2     | inputChannelBitmask high 8  | bit0=ch9 … bit7=ch16 |
| 3     | dataType enable flags    | bit0=note, bit1=cc, bit2=programChange, bit3=pitchBend, bit4=aftertouch(poly), bit5=channelPressure, bit6=sysex |
| 4     | noteRangeStart (0–127)   | only meaningful if bit0 of byte3 set |
| 5     | noteRangeEnd (0–127)     | |
| 6     | ccRangeStart (0–127)     | only meaningful if bit1 of byte3 set |
| 7     | ccRangeEnd (0–127)       | |
| 8     | outputDevice             | 0=None, 1=TRS, 2=USB, 3=Both |
| 9     | outputChannelBitmask low 8  | |
| 10    | outputChannelBitmask high 8 | |
| 11    | transpose                | encoded as `semitone + 64`, range -64..+63 |
| 12    | ccMapStart (0–127)       | destination start CC for the mapped cc range; must satisfy `ccMapStart + (ccRangeEnd-ccRangeStart) <= 127` (enforced by the web UI) |
| 13-36 | name (24 bytes)          | ASCII, zero-padded (not necessarily NUL-terminated if it fills all 24 bytes). The device stores and returns this verbatim but never interprets it — it's purely a label. The web UI enforces the 24-character limit and restricts input to printable ASCII before ever building the wire bytes. |

## Global struct (6 raw bytes, sent unpacked — small enough not to need pack7)

| byte | field                  | notes |
|------|------------------------|-------|
| 0    | mode                   | 0=USB Adaptor Mode, 1=Router Mode |
| 1    | buttonAction           | 0=panic, 1=pitchUp, 2=pitchDown, 3=mod, 4=ccMomentary, 5=ccToggle, 6=octaveUp, 7=octaveDown, 8=noteMomentary, 9=noteToggle |
| 2    | buttonParamA           | pitch/mod: speed (0-127). cc momentary/toggle: cc number. note momentary/toggle: note number (0-127). octave up/down: unused (0) |
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
   `(outputChannel, messageKind, number)` — `number` is the note number for Note/Aftertouch, the
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
flash (rp2040 core's `EEPROM.h`, ~9.5KB reserved: 6 bytes global + up to 256×37 raw bytes ≈ 9.3KB +
a small header/magic-number/count). The web app now edits entirely locally and only talks to the
device when you explicitly click "Load from Device" or "Save to Device" — earlier revisions pushed
every field edit live, but that could race with itself (see the SET_GLOBAL/SET_ROUTE section above)
and made "have I actually saved this?" unclear. "Save to Device" pushes the full local state
(global + every route, in order) and then calls `COMMIT` so it's persisted to flash in one shot.

`FACTORY_RESET` (software) and the boot-time button-hold (hardware) both reset RAM to: Mode = USB
Adaptor, button = Panic, brightness step = 10 (100%), zero routes — then persist that to flash.
