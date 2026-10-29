# FoxWizard Mini MIDI Multitool, SysEx Protocol

IDENTITY_REPLY's `protocolVersion` says which version a board speaks:

- **1:** firmware 1.0-1.3. 41-byte routes.
- **2:** 45-byte routes with port masks, and GET_STATUS for the sidecar.
- **3:** Global clock settings, LEARN, and the clock in STATUS_DATA.
- **4:** Host Edition test builds from 1.6. 53-byte routes with feature fields, and feature blocks.

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
| 0x14   | STATUS_DATA           | dev→host  | sidecarPresent (0/1), hostPortMask (bit n = USB Host n+1 has a USB MIDI device, 7 bits from 1.74), sidecarDrops (saturates at 127), restartReason (0 none, 1 hang, 2 USB Host port stalled, 3 USB data sequence error, 4 USB buffer armed twice, 5 USB device stalled), restartPort (1-4, for reason 2), restartCount (recovery restarts since the sidecar powered up, saturates at 127). Protocol 2. Protocol 3 adds bpm(2) and clockStatus (0 clock unused, 1 master, 2 following, 3 following with no clock arriving, 4 Auto leading with its own clock, from 1.92). Firmware 1.44 adds hasTrs (1 = a TRS board is added on top of the bare sidecar module, so TRS 2-6 exist). 1.71 adds hubKnown, hubJacks, hubCount and hostWaiting (devices with no free slot), 1.74 the host brain's firmware version, 1.83 how hard the main loop has worked since the last status, then reset: longest pass in microseconds (2 bytes, low 7 bits first), loops stopped at the link input limit (0-127), loops input was held back because an output queue was filling (0-127), and the fullest the link, TRS and USB queues got as 0-127 of their size. 1.87 adds four counts, 0-127 each, since the last status: a Note On for a key already down, a Note Off for a key not down, the loop's Note Offs that nothing of the loop's was holding, and the loop's Note Ons for a note it was still holding |
| 0x15   | LEARN                 | host→dev  | (none). Protocol 3. The next Note On or CC from any input is consumed and answered with LEARN_DATA. Gives up after 10 s |
| 0x16   | LEARN_DATA            | dev→host  | type (1 note, 2 CC), channel (0-15), number, port bit (0 TRS/DIN … 6 TRS 2) |
| 0x17   | GET_FEATURE           | host→dev  | type. Protocol 4 |
| 0x18   | FEATURE_DATA          | dev→host  | type + pack7(block). Protocol 4 |
| 0x19   | SET_FEATURE           | host→dev  | type + pack7(block). ACK checksum is checksum7 of the block as stored, ACK extra is the type. Protocol 4 |
| 0x11   | GLOBAL_DATA           | dev→host  | mode, buttonAction, buttonParamA, buttonParamB, buttonToggleMomentary, ledBrightnessStep. Protocol 3 adds clockSource, bpm(2), beatsPerBar, beatUnit, clockOut, tapType, tapChannel, tapNumber, see Global clock |
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
| 0x37   | GET_PRESETS           | host→dev  | (none). Protocol 5 |
| 0x38   | PRESET_DATA           | dev→host  | slotCount (1-16), activeSlot (0-15), slots holding a saved preset as a 16-bit mask in three 7-bit bytes, low first |
| 0x39   | SET_PRESET_SLOTS      | host→dev  | slotCount (1-16). ACK checksum is the slot count, extra is the active slot. ERR_FULL when the flash filesystem is not available |
| 0x3A   | SELECT_PRESET         | host→dev  | slot (0-15). ACKs once the slot is live, checksum is the active slot, extra the slot count. ERR_BAD_INDEX past the slot count |
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
  with Flash Size `2MB (Sketch: 1MB, FS: 1MB)`. `mmm_web_interface/firmware/build_firmware.sh`
  does this.

## Global clock (protocol 3)

GLOBAL_DATA and SET_GLOBAL grow from 6 to 15 bytes. A protocol 1-2 host sends 6, and the device
leaves the clock settings alone.

| byte | field | notes |
|------|-------|-------|
| 6 | clockSource | 0 the FoxWizard is master, 1-14 follow port bit 0-13 (TRS/DIN, USB Device, USB Host 1-4, TRS 2, then from 12 USB Host 5-7). 8-11 are outputs only and do nothing. 15 Auto (1.92, the factory setting from 1.98): follows the clock of the first port to send one while that clock arrives, leads with its own at the tempo it had while none does |
| 7-8 | bpm | two 7-bit bytes, low first, 20-300 |
| 9 | beatsPerBar | time signature top, 1-16 |
| 10 | beatUnit | time signature bottom: 2, 4, 8, 16 |
| 11 | clockOut | 1 = send MIDI clock to every port while master |
| 12 | tapType | tap tempo trigger: 0 none, 1 note, 2 CC |
| 13 | tapChannel | 0-15, 16 any |
| 14 | tapNumber | note or CC number |

The settings only apply while something uses the clock: for now buttonAction 10 (Tap Tempo), later
the looper and timed route features. Until then clock routes as it always has. While in use:

- **Follow:** clock from the chosen port drives the clock engine and routes as usual. Clock from
  every other port is discarded. Start, Continue, and Stop still route from every port.
- **Master:** all incoming clock is discarded. With clockOut on, the FoxWizard sends its own clock
  to every port.
- **Tap tempo:** the button (buttonAction 10) or the tap trigger note or CC. The trigger is consumed,
  its Note Off and zero CC values included. Tapping only sets the tempo while master. A tapped tempo
  is kept in RAM until the next COMMIT.

The clock engine is ported from ARPnMIDI (`src/clock_engine.*`, `src/musical_time.*`), 96 pulses per
quarter note internally.

## Feature blocks (protocol 4)

Settings for a feature live in one shared block, not in every route. A route switches the feature on
with its bit in the route's `features` field. A route from a protocol 1-3 host arrives without the
feature fields, so every feature is off for it.

Blocks are saved after the routes as `F` `X`, then (type, size, data) per block, then 0. A save
without the marker loads every block as zeros. A block saved by a newer firmware with more bytes is
cut to what this one knows, a shorter one is zero-filled. Zero means off or unused everywhere.

| type | block | size | notes |
|------|-------|------|-------|
| 3 | Scale | 8 | root (0-11), scaleType (0-11: Major, Minor, Harmonic Major, Harmonic Minor, Melodic Major, Melodic Minor, Major Blues, Minor Blues, Major Pentatonic, Minor Pentatonic, Dorian, Mixolydian), 6 spare (Layout and chord are per route). Firmware 1.11 |
| 4-7 | Arp slots 1-4 | 48 each | mode (0 Up, 1 Down, 2 Up-Down 1, 3 Up-Down 2, 4 Drum Roll/Chord Pulse, 5 Random, 6 Up 4-Step, 7 Rhythm, 8 Ostinato, 9 Octave Walk, 10 Fifth, 11 Bass+Chord, 12 Chord+Run, 13 As Played, 14 Converge, 15 Diverge, 16 Thumb, 17 Pinky), default division + 1 (fw::MusicalDivision, 0 = 1/16), octaves (1-4, 0 = 1), gate % (5-100, 0 = 50), velocity (1-127 fixed, 0 = each note's own), holdType (0 none, 2 CC), holdChannel (0-15, 16 any), holdNumber, aftertouch velocity (1 = on), bypass channel (0 off, 1-16, 1.19), bass channel (0 off, 1-16, 1.19), default trigger type, channel, number (1.24), follows live channel (1.40), 1 spare, 7 division squares of (division + 1, 0 = unused, triggerType 0 none 1 note 2 CC, triggerChannel 0-15 16 any, triggerNumber), 4 spare. Firmware 1.13, 48 bytes and squares 1.16 |
| 8 | Echo | 48 | division + 1 (0 = 1/8), repeats (1-32, 0 = 4), level % of the note's velocity for the first echo (1-100, 0 = 70), default trigger type, channel, number (1.24), before route (1.49: 1 = hears the route's input), momentary (1.58: 1 = sounds only while a square is held), global (1.58: 1 = echoes everything sent), bounce (1.59: signed -8..8, positive repeats come closer), natural fade (1.59: 1 = each repeat loses the same share of loudness), 5 spare, 7 division squares as in the arp block, 4 spare. Firmware 1.23 |
| 9 | Stutter | 48 | default division + 1 (0 = 1/8, 20 = 2 bars, 21 = 4 bars, 1.58), runout bars (1-16, 0 or 127 = no runout, 1.53), default trigger type, channel, number, before route (1.49), latching (1.50), global (1.56: loops everything sent), 8 spare, 7 division squares as in the arp block (division codes as the default), 4 spare. Firmware 1.24 |
| 10 | Looper | 48 | bars (0 Free, 1-16), quantize (0 off, else fw::MusicalDivision + 1), auto overdub (1.29), record inputs (0 outputs, 1 inputs), double tap clears (1.29: 0 = holding the button clears and a double tap stops, 1 the other way), loop unrouted off (1.48: 1 = skip notes no route matches), looper button trigger type, channel, number (1.49), record CCs (1.55), restart on clock start (1.93: 1 plays the loop again from its top on a Start from the followed clock), 37 spare. Firmware 1.27 |
| 11 | Note to Feature | 80 | 16 entries of (source note + 1, 0 = unused, then a 4-byte action). Firmware 1.40 |
| 12 | CC to Feature | 80 | 16 entries of (source CC + 1, 0 = unused, then a 4-byte action). Firmware 1.40 |
| 13 | Launchpad buttons | 64 | 16 actions of 4 bytes, top row then side column. Firmware 1.40 |
| 14 | Per-Note Parameter Lock | 8 | delay (0 None, else fw::MusicalDivision + 1), glide (1 = each CC moves from its last value to the locked one over the delay), 6 spare. The locks themselves live in the device's flash, not in this block. Firmware 1.59 |

**Note to CC:** only the first route with the switch on uses the block. A mapped note from that
route's inputs becomes a CC on each of its output channels and ports instead of a note. Unmapped
notes play as usual.

**Launchpad:** for older Launchpads (original, S, Mini mk1/mk2) in their default X-Y mode, where a
pad sends note 16 × row + column (top-left 0), the round side buttons are column 8, and the top round
buttons are CC 104-111. A route with the switch on turns the grid into the Launchpad's own drum rack
layout, without sending the Launchpad a mode change: four 4×4 blocks from 36, bottom-left 36-51,
top-left 52-67, bottom-right 68-83, top-right 84-99, each block left to right then up. Route note
filters and everything after see the remapped note. Every pad note, side-button note, and top-button
CC is first sent straight back to the Launchpad's port and channel unchanged, so its light comes on.
That port is then left out of everything the route sends for that message, in any mode. Firmware
1.9. The round buttons send the note (velocity 127,
then a Note Off) or CC (127, then 0) set for them on the route's output channels and ports, whatever
data types the route lets through.

**Scale:** root and scale are global (Patchling's yellow Scale row). Each route with the switch on maps each note it lets through, after Launchpad and Note to CC,
with its own Layout:

- **Next Note Up:** a note outside the scale moves up to the next scale note.
- **White Keys:** each white key plays the next scale degree, keeping the keyboard's octaves. A black
  key plays the degree of the white key below it. Same as ARPnMIDI.
- **Drum Pad 5ths, 4ths, 3rds:** a 4-wide pad grid from note 36, pad 0 playing the root at 36 + root.
  scaleIndex = column + row × rowOffset, rowOffset 4, 3, 2. Right is the next scale note, up a row
  is 4, 3, or 2 scale notes more.

All Layouts count in scale positions, so they work the same for 5, 6, and 7-note scales. A note
mapped outside 0-127 is dropped.

**Auto Chord:** each route with the switch on plays its own chord for each note. On a route that also
has Scale, the positions are scale positions counted from the played note's own position, so the
chord stays in key. Otherwise they're semitones above it. Patchling clears positions beyond the
current scale's size, so a hidden position never sounds. Mono Retrig routes play single notes.
Firmware 1.12.

Each held note's Note Off releases exactly what its Note On played, even if the settings changed in
between. Output notes shared by several held notes are counted, so releasing one doesn't cut another.

**Arp:** a route with an arpSlot holds the notes it lets through (after Launchpad, Note to CC, Scale,
Auto Chord) into that slot instead of playing them, and the slot's steps go out through the route
like ordinary notes. Ported from ARPnMIDI's classic arp: Up-Down 1 plays the end notes once, Up-Down
2 twice, and Drum plays every held note at once on each step. Each full pass through the notes moves
up an octave, for the slot's octave count. A note lasts gate % of a step, at least 15 ms.

All slots step on the clock engine's pulses (96 per quarter), so they share one grid as master or
following. As master, a fresh start (no other arp running) restarts the grid 6 ms after the first key,
so the whole chord pressed is caught. Joining a running arp, or following another clock, lines up
with that grid's next step. Hold works like a sustain pedal on its CC (64 and up holds): notes stay
after release, and a new key after every key is up starts a fresh phrase.

Modes 13-17 (1.20) are note orders played round, a full pass moving up an octave: As Played (the
order pressed, restarting with each new phrase), Converge (outside in), Diverge (inside out), Thumb
(the lowest note between each of the others), Pinky (the highest between each of the others).

Fifth (10) plays each held note then a fifth above it, through every held note (1.21): 4 scale positions up on a Scale route, a perfect fifth otherwise (1.22). The other modes 6-12 are ARPnMIDI's 16-step patterns: each step picks a held note by index (low to high), with
octave and semitone offsets, a rest, or every held note. On a Scale route, a pattern note outside
the key snaps up to the next scale note. While a division square's note or CC (64 and up) is held,
the arp plays that division from its next grid step, the last one pressed winning, then returns to
the default. The trigger doesn't pass through. Aftertouch velocity takes channel pressure or poly aftertouch from the
route's inputs and maps it to velocity 42-127, as ARPnMIDI's drum roll does, and the pressure still
passes through. Bypass plays the held notes unarped on its channel, and Bass plays the lowest held note an
octave down on its channel, retriggering when the lowest note changes. Both follow hold, go out the
route's ports with its transpose, and can share a channel with each other or the route's outputs. Panic also makes every arp let go of its notes, hold included. When a hub
device unplugs, the notes it was holding are released as if their Note Offs arrived. An arp route keeps the
global clock in use, so its settings apply.

**Echo:** the one route with feature bit 4 repeats each note it sends, on the same channel and ports,
at the echo division, from ARPnMIDI's echo engine. Echoes fade evenly so the last of the repeats is
the quietest, and each echo is no longer than the played note or 3/4 of the echo spacing. Pressing a
division square's note or CC (64 and up) latches that division, pressing it again goes back to the
default. The trigger doesn't pass through. An Echo route keeps the global clock in use. Panic stops
every echo.

**Default square triggers (1.24):** a trigger type, channel, and number on a division table's default
square. On the arp it turns arpeggiating off and on (1.26): while off, the route plays the held notes
straight and Bypass and Bass keep playing, and turning it on with notes held starts like a fresh press. On Echo it turns echoing off (echoes already sounding fade out) and on.
Either lasts until routes change or power off. On Stutter the default square stutters like the others.

**Stutter (1.24):** the one route with feature bit 5 remembers the last 512 notes it sent. Holding a
division square's note or CC (64 and up), the default square included, loops that length of what it
just played, from ARPnMIDI's repeater: the route's live notes stop and stay muted, and the loop feeds
Echo. With several held, the last one pressed loops. Releasing every trigger stops it. The runout
lets go by itself after its bars, counted from the first press, until a trigger is pressed again.
Panic stops it.

**Looper (1.27):** button action 11, one loop from ARPnMIDI's looper engine. It records the notes the
routes send, replayed to the same ports, or with Record Inputs the notes coming in, replayed through
the routes as if they came in again. The lights show its state: red recording or overdubbing, yellow
armed, green playing, off when stopped or cleared. It powers up stopped, never armed.

Play/arm (the button, or a mapped Looper action) acts at once on a single tap:

| State | Single tap |
|---|---|
| Stopped and cleared | Arms, recording from the next note |
| Armed | Unarms |
| Recording | Free: ends the take and plays it. Measures: cancels and re-arms |
| Playing | Overdub on or off |
| Stopped with a loop | Plays |

A double tap (a second tap within 1 second) and holding the button half a second each first put back
what the first tap did, then one clears and the other stops (1.29). With Double Tap Clears off, the
default, holding clears and a double tap stops. Clear, from the state before the first tap: a cleared
or armed looper brings its loop back and plays it, a take in progress is cancelled, a playing or
stopped loop stops and clears (undoable). Stop: the loop stops, kept, so a tap plays it.

Stop/clear (a mapped Looper action) works as ARPnMIDI's: it ends a take, stops, and the next press
clears, or brings back a cleared loop. A Free take sets the BPM, taking the loop as one bar, unless
following another clock (Auto does while it leads). Auto Overdub (1.29) turns overdub on as a new take starts playing, until a
tap turns it off. Panic stops the loop without clearing it. Arming never discards a cleared loop's
undo, only the first note of a new take does. A take that starts while notes are already sounding on the side
being recorded (a held bass note, a chord already down) begins with them (1.30). A take or overdub
only records a note's end if it recorded its start, so releasing a note held from before an overdub
leaves the loop alone (1.31).

The loop holds 6144 events (a note is 2), in 12-byte slots (1.28). It's saved to the flash area as
/loop.fw once the looper is stopped and no note has come in or gone out for 5 seconds, through a
temporary file, with a checksum checked at power on (anything wrong leaves the looper empty). The
host brain is paused while flash is written. A firmware update removes the saved loop only if the
flash area can't fit both, and the update's staged file is removed at power on once installed.

**Launchpad ports (1.38):** a port a Launchpad route's grid or button presses come in on becomes a
Launchpad port, owned by that route, until routes change. No note goes there as it is. Every note
headed to it, from any route and channel (arp, Bypass, Bass, echoes, stutter, looper playback),
lights each grid pad that plays that note on the owning route (through the grid, Scale, Auto Chord
and transpose), on channel 1. The velocity sets the color by the note's real channel (1.39): yellow
(62), orange (47), green (60), red (15), round again, with channel 10 always red. Presses echo back
as they come in, in their channel's color.

**Route rewrites (1.35):** adding, setting, deleting, moving or clearing routes forgets which keys
hold which notes, so 50 ms after the last one, All Notes Off (CC 123) goes to every port on every
channel. Patchling's Autosave saves while notes play, so this keeps them from sticking.

**The mapper (1.40):** Note to Feature (feature bit 0), CC to Feature (bit 6) and
the Launchpad buttons run the same actions, 4 bytes each: type, a, b, spare. A note presses and
releases. A CC presses when it crosses 64 and releases below, and every value also reaches value
actions. CC to Feature matches the route's input ports and channels only, not its data type filters.
Mapped notes and CCs don't pass through. Messages go to the route's outputs and channels.

| Type | Action | a | b | Acts on |
|---|---|---|---|---|
| 0 | Off | | | |
| 1 | Note | note | | press (velocity, 127 from a CC), release |
| 2 | CC Momentary | CC | | press 127, release 0 |
| 3 | CC Toggle | CC | | press, 127 and 0 in turn |
| 4 | CC Value | CC | | a note's velocity (0 on release), or every CC value |
| 5 | CC Level | CC | level | press, stays |
| 6 | Transport | 0 Play, 1 Stop, 2 Continue, 3 Pause, 4 Record, 5 Record Exit, 6 Rewind, 7 Fast Forward, 8 Return to Start | | press: MMC (F0 7F 7F 06 cmd F7, Locate to 0 for Return to Start), plus MIDI Start for Play, Continue for Continue, Stop for Stop and Pause. Not to Launchpad ports |
| 7 | Program Change | program | | press |
| 8 | Program Up/Down | 1 up, 0 down | | press, from the last program sent, wrapping |
| 9 | Live Out Channel | channel 0-15 | | press: the route sends only on that channel for the session, pressed again it's back. Notes sounding on the old channels get All Notes Off, and the route's arp lets go |
| 10 | Live Velocity | 0 follows the value, 1-20 = 10-200% | | follows: every CC value or press velocity sets 10-200%. Fixed: press sets it, pressed again it's back. Takes the place of the route's Velocity % for the session |
| 11 | Looper | 0 1-Button (as the button in Looper mode, hold included), 1 Play/Arm/Rec, 2 Stop/Clear/Undo, 3 Play, 4 Stop, 5 Pause, 6 Overdub, 7 Clear, 8 Un-Clear, 9 Rec/Arm, 10 Layer Undo, 11 Layer Redo, 12 Double, 13 Half (10-13 from 1.60), 14 Note Delete (1.66) | | press (1-Button: press and release) |
| 12 | Tap Tempo | | | press |
| 13 | Live Ch Up/Down | 1 up, 0 down | | press: the live out channel steps, wrapping |
| 14 | Arp Div Up/Down | 1 up, 0 down | | press: the route's arp division steps |
| 15 | Stutter Length | 0 1/8 bar, 1 1/4, 2 1/2, 3 1 bar, 4 2 bars, 5 4 bars | | press and release, as a Stutter square |
| 16 | LP Quad | | | press: Break Off a Quad off and on |
| 17 | Echo On/Off | | | press |
| 18 | Note Toggle | note | | press, on and off in turn |
| 19 | Pitch Ramp | 1 up, 0 down | length: 0 instant, else division + 1 | press bends over the length, release returns |
| 20 | Mod Ramp | | length | as Pitch Ramp, on CC 1 |
| 21 | CC Ramp | CC | length | as Pitch Ramp, on that CC |
| 22 | Panic | | | press |
| 23 | Octave Up/Down | 1 up, 0 down | 0 momentary, 1 toggle | press shifts the route an octave |
| 24 | Clear P-Locks | | | press |

Live out channel and velocity are session only, cleared when routes change. With a live out channel on,
a route's arp is bypassed (notes play straight on the live channel, no Bypass or Bass) unless its
Follows Live Ch Change switch is on (arp block byte 14), then it arpeggiates onto the live channel.
Double and Half (1.60) with no loop playing (none recorded, or a cleared one) change the Length setting in
RAM for the next take instead, Free counting as 1 bar, and a take armed or recording at a set length changes
length at once (1.73). Launchpad buttons light to show their action's state (1.73), and blink the bar count
for Double and Half.
Note Delete (1.66) is held: press and release. While the loop plays, the notes the user plays don't pass
(Note to Feature notes excepted, unless they're also a division trigger, which the loop records and Note Delete erases) and each erases, from the loop, the notes it stands for as playback reaches
their Note On, and the Note Off that ends them. Recording inputs, a key stands for its own channel and note. Recording
after routes, for the notes its routes would make from it (Scale, Auto Chord, transpose, arp octaves) on any channel.
Notes are erased for good. A key pressed before Note Delete, or after it's let go, plays as normal.
Echo and Stutter apply either way. Stop starts the next Play from the top, Pause carries on from
where it stopped. Rec/Arm arms, and pressed again while armed starts recording at once. Types 1 and
2 are unused.

**PLAY_NOTE (0x33, 1.34):** on (1/0), note, velocity. The note goes to every port on every channel,
for Patchling's piano. No reply.

**CLEAR_PLOCKS (0x34, 1.59):** no payload. Forgets every Per-Note Parameter Lock. No reply.

**GET_HOST_REPORT (0x35, 1.63):** payload: page (0 or 1). Reply HOST_REPORT_DATA (0x36): page, complete (1 when every piece of the report has arrived from the host brain), then pack7 of that page's 120 bytes. The 240-byte report is the host brain's record of its last recovery restart, layout in `foxwizard_link.h`. All zero until a host brain with firmware 3 or later restarts.

## Presets (protocol 5, firmware 1.102)

A slot is a copy of the saved settings image (the `FOX2` layout below) in the flash filesystem, `/preset01.fw` to `/preset16.fw`. The slot count and the active slot are in `/device.fw` and are not part of any preset. The EEPROM keeps the last saved image as before, so a unit with one slot works exactly as in earlier firmware.

- COMMIT writes the EEPROM and, with more than one slot, the active slot's file. Once a slot has a file it is kept up to date.
- SELECT_PRESET, the front button (button action 12) and the mapper action Preset (type 25, a: 0 next, 1 previous, 2-17 slot 1-16) all queue a change that runs at the top of the next main loop pass. It first releases every key held on any input, in at the input through the routes as they are (1.105), so each route, arp and mapped action sends the Note Offs and releases it owes, on the channels it sent them on. Then it stops what Panic stops (arps, Echo, Stutter, the loop, which stays), sends Reset All Controllers, All Notes Off and All Sound Off to each port on the channels the routes send to (every enabled route's output ports and channels, its Live Ch override and Break Off a Quad channel) and on any channel a note has been sent on (1.103, not Panic's burst to every channel of every port), reads the file into RAM, and flashes the slot number (140 ms on, 220 ms off). A slot without a file loads the factory defaults.
- The unit can change slot by itself, so a host that saves must check first. STATUS_DATA ends with the active slot and the slot count (1.103, bytes 26 and 27). Saving an editor that shows another slot writes that slot's settings over the live one.
- Kept across a change: slot count, active slot, LED brightness, and button action 12.
- With more than one slot, a mapping to Preset in Note to Feature, CC to Feature or the Launchpad table sets button action 12. SET_GLOBAL and SET_FEATURE apply this too.
- The active slot is written to `/device.fw` once the unit has been quiet for 2 s, since a flash write holds the clock. At power on the unit loads the file of the active slot.
- FACTORY_RESET and the boot-time button hold delete every slot file and `/device.fw`.

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
| 45-46 | features, little endian (protocol 4) | feature switches: bit0 Note to CC, bit1 Launchpad, bit2 Scale, bit3 Auto Chord |
| 47    | arpSlot (protocol 4) | 0 none, 1-4: this route feeds that arp slot. Firmware 1.13 |
| 48    | scaleLayout (protocol 4) | Layout of a Scale route: 0 Next Note Up, 1 White Keys, 2 Drum Pad 5ths, 3 Drum Pad 4ths, 4 Drum Pad 3rds. Firmware 1.14 |
| 49-50 | chordMask, little endian (protocol 4) | Auto Chord positions of this route: bit k = scale position k+1 on a Scale route, else k semitones. Firmware 1.15 |
| 51    | launchpadFlags (protocol 4) | bit0: a Launchpad route plays an octave down, its output shifted like a -12 transpose, lights included. Firmware 1.43 |
| 52    | spare (protocol 4) | zero. Room for later route features without another wire format change |

**Port masks** (protocol 2): bit0 TRS/DIN, bit1 USB Device, bits 2-5 USB Host 1-4 (sidecar hub
ports), bit6 TRS 2 (sidecar DIN IN and DIN OUT 1), bits 7-10 TRS 3-6 (sidecar DIN OUT 2-5, outputs
only), bits 11-13 USB Host 5-7 (firmware 1.74, host firmware 6). Bits 14-15 are spare, kept for BLE MIDI. Bits 0-1 match the old inputDevice/outputDevice meaning, so a route with byte0 bit6 clear
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
| 1    | buttonAction           | 0=panic, 1=pitchUp, 2=pitchDown, 3=mod, 4=ccMomentary, 5=ccToggle, 6=octaveUp, 7=octaveDown, 8=noteMomentary, 9=noteToggle, 10=tapTempo, 11=looper, 12=preset |
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

60 routes (64 before 1.48) leave each route room to grow from 41 to 63 bytes within 4 KB. The firmware's static_assert
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
