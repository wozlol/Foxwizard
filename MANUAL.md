# FoxWizard Mini MIDI Multitool, User Manual

## Overview

USB-C to TRS MIDI adapter and router. Two operating modes, up to 60
programmable routes, live performance features (Arp, Scale, Auto Chord,
Echo, Stutter, Looper, Launchpad, Per-Note Parameter Lock), all set up
from a web app called FoxWizard Patchling.

The Host Edition adds seven USB Host ports and more TRS/DIN ports. See
[`Foxwizard Host Edition/README.md`](Foxwizard%20Host%20Edition/README.md).

## Connections

- USB-C: connects to a computer or USB MIDI host.
- TRS MIDI IN: 3.5mm TRS-A MIDI input.
- TRS MIDI OUT: 3.5mm TRS-A MIDI output.
- Button: single multi-function button.

## Host Edition ports

The Host Edition adds a host brain to the FoxWizard, and with it more ports. All of them route
through the same routes.

| Port | In | Out | Notes |
|---|---|---|---|
| USB Device (USB-C) | 1 | 1 | to a computer |
| TRS/DIN | 1 | 1 | the FoxWizard's own |
| USB Host 1-7 | 7 | 7 | one device each. The board's hub has four jacks, a 7-port hub or two chained 4-port hubs gives all seven |
| TRS 2 | 1 | 1 | needs the TRS board |
| TRS 3-6 | | 4 | needs the TRS board |

That is **13 MIDI connections** on the board, 7 inputs and 11 outputs to route between, and a hub
takes it to 16 connections, 10 inputs and 14 outputs. Without the TRS board it is 7 connections on the
board, 6 inputs and 6 outputs. Up to 60 routes.

Up to seven USB devices work at once, whichever hub jacks they're plugged into. An eighth waits and
starts working when a port frees up. Patchling shows only the USB Host ports there's room for. With no
hub, that's one, for a device plugged straight in. With a hub it's the hub's jacks, up to seven, and the
next port shows up once a device is plugged into the one before it. A hub made of more than one chip,
or a hub plugged into a hub, works too. Seven devices on bus power is a lot for one USB supply, so
power the hub if they don't all come up.

## Status LEDs

- IN LED: flashes on TRS MIDI input activity.
- USB LED: flashes on USB MIDI activity.
- OUT LED: flashes on TRS MIDI output activity.
- Brightness: 0-10 steps, set with the Lights knob in Patchling.

With Button Action set to Looper, the lights show the looper instead:
OUT recording, IN armed, USB playing. After Double or Half, all three
flash together once per bar of the new loop length.

## Modes

Set in Patchling with the Main Mode switch.

- **Adaptor** (default): USB in to TRS out. TRS in to USB out.
- **Router**: TRS in to TRS out and USB out. USB in to USB out and
  TRS out. Full merge, unless a route intercepts a data type.

A message no route matches falls through to the current mode.

## Button

One action, set in Patchling with Button Action:

| Action | Settings |
|---|---|
| Panic (default) | |
| Looper | the looper's one-button control, see Looper |
| Pitch Up, Pitch Down, Mod | Length (Instant, or a division up to 1/1), Channel |
| CC Momentary, CC Toggle | CC Number, Channel |
| Octave Up, Octave Down | Momentary or Toggle, Channel |
| Note Momentary, Note Toggle | Note, Channel |
| Tap Tempo | |

## Routes

Each route filters and transforms MIDI by:

| Field | Options |
|---|---|
| Input | any input port |
| Input Channels | any of 1-16 |
| Data Types | Note Range, CC Range, Program Change, Pitch Bend, Clock, Ch Pressure AT, Poly AT, SysEx, Common, Realtime (Transport) |
| AT to CC | Ch Pressure AT or Poly AT remapped to a chosen CC |
| Output | any output port, or none to block |
| Output Channels | any of 1-16 |
| Transpose | -64 to +63 semitones |
| Velocity % | 10-200% |
| CC Map Start | shift the CC range to begin at this CC |
| Mono Retrig | poly to mono, last-note priority |
| Round Robin | each new note to the next selected output channel, or a random one (Rand) |
| Looper | whether this route's notes are recorded (shown once the looper is set up) |

Routes are evaluated top to bottom. Later routes override earlier ones
targeting the same output. Drag to reorder in Patchling.

Below these, each route has a switch for each feature. Switches that are
off stay on top, and features switched on open up below them.

## Global Clock

Shown while something uses the clock (Arp, Echo, Stutter, Looper, ramps,
Tap Tempo).

| Setting | Options |
|---|---|
| BPM | 20-300, while the FoxWizard is master |
| Clock | FoxWizard is master, or Follow a port. Following ignores clock from every other port. |
| Send Clock to every port | master only |
| Time Signature | 1-16 beats over 2, 4, 8 or 16 |
| Tap Tempo Trigger | a Note or CC, with Learn |

A loop recorded at Free length sets the tempo from its length.

## Divisions

Arp, Echo and Stutter each have a division table: a default division,
plus up to 7 more squares. Each square is a division and a learned Note
or CC trigger. Pressing a square's trigger switches to its division.

Divisions run 1/1 to 1/64T, with dotted (D) and triplet (T) values.
Stutter also has 2 bars and 4 bars.

## Scale and Auto Chord

The Scale row sets one root and scale for the whole FoxWizard, and shows
them on a piano and a guitar neck. Scales: Major, Minor, Harmonic Major,
Harmonic Minor, Melodic Major, Melodic Minor, Major Blues, Minor Blues,
Major Pentatonic, Minor Pentatonic, Dorian, Mixolydian.

- **Scale** (per route): keeps the route's notes in the scale. Layout:
  Next Note Up, White Keys, Drum Pad 5ths, Drum Pad 4ths, Drum Pad 3rds.
- **Auto Chord** (per route): each note plays a chord. Pick its notes
  as scale positions with Scale on, or as intervals without it.

## Arp

Up to 4 routes can arp at once, all on the Global Clock.

| Setting | Options |
|---|---|
| Mode | Up, Down, Up-Down 1, Up-Down 2, Drum Roll/Chord Pulse, Random, As Played, Converge, Diverge, Thumb, Pinky, Up 4-Step, Rhythm, Ostinato, Octave Walk, Fifth, Bass+Chord, Chord+Run |
| Octaves | 1-4 |
| Gate % | 5-100 |
| Velocity | Played, Fixed, or Aftertouch |
| Bypass ch | also sends the played notes, unarped, on this channel |
| Bass ch | also plays the lowest held note an octave down on this channel |
| Hold | a CC that holds the arp's notes, with Learn |
| Division | division table. The default square's trigger turns the arp off and on. |

## Echo

Each note the route plays repeats at the echo division, fading out.

| Setting | What it does |
|---|---|
| Global Echo | echoes everything the FoxWizard sends, not just this route |
| Echo Latching | on (default): a trigger press turns Echo off and on, or latches a division. Off: Echo sounds only while a square is held. |
| Echo After Route | on (default): echoes what the route sends. Off: echoes the notes coming in, so the repeats go through the route too (arped, scaled). |
| Repeats | 1-32, each a step quieter than the last |
| Level % | how loud the repeats start |
| Bounce | each gap is a fraction of the last, heard from the first repeat. Bounce 1/2 halves it every time, like a ball settling fast, then 2/3, 3/4 out to 8/9. Slow 2/1 to 9/8 makes the gaps grow instead, up to 4 times the division. Off keeps them even. |
| Natural Fade | each repeat loses the same share of loudness, like a bouncing ball, instead of an even fade |
| Division | division table |

## Stutter

Pressing a division square repeats that length of what was just played,
over and over. Live notes still play over the top. If a stutter is longer
than what's been played so far, it waits until there's enough.

| Setting | What it does |
|---|---|
| Global Stutter | stutters everything the FoxWizard sends, not just this route |
| Stutter Latching | off (default): stutters while a square is held. On: a press starts it, the same press again stops it. |
| Stutter After Route | on (default): stutters what the route sends. Off: stutters the notes coming in, so the repeats go through the route too. |
| Runout Bars | Off (default), or stop by itself after 2, 4, 8 or 16 bars |
| Division | division table, up to 4 bars |

Pressing another Echo or Stutter square while one is on switches to it.

## Per-Note Parameter Lock (P-Lock)

Hold a key and move a CC: that CC value is locked to the key, and sent
again each time the key plays. Up to 256 locks, kept through power off.

| Setting | What it does |
|---|---|
| Glide | off: the locked values jump. On: they slide from the last value sent. |
| Delay | None sends the locked values before the note. A division waits that long first, or with Glide on, slides over that long. |
| Clear Locks | forgets every lock |

## Note to Feature and CC to Feature

Up to 16 Notes and 16 CCs, each set to an action, with Learn. The same
action list drives the Launchpad buttons.

| Action | |
|---|---|
| Note, Note Toggle | |
| CC Momentary, CC Toggle, CC Value, CC Level | |
| Pitch Ramp, Mod Ramp, CC Ramp | sweep over a division |
| Transport | Play, Stop, Continue, Pause, Record, Record Exit, Rewind, Fast Forward, Return to Start (MMC, and MIDI Start/Stop/Continue) |
| Program Change, Program Up/Down | |
| Live Ch, Live Ch Up/Down, Live Velocity | change a route's output channel or velocity while playing |
| Octave Up/Down | |
| Arp Div Up/Down | |
| Echo On/Off | |
| Stutter Length | 1/8 bar to 4 bars |
| LP Quad | turns Break Off a Quad off and on |
| Looper | any looper action, see Looper |
| Tap Tempo, Panic, Clear P-Locks | |

## Launchpad

Turn Launchpad on for the route a Novation Launchpad (in Programmer mode)
plays into. The grid becomes four 4x4 note blocks from note 36. The pads
light up with whatever notes play back to them, in each channel's color.

| Setting | What it does |
|---|---|
| Octave | 0, -1 or -2 |
| Break Off a Quad | one 4x4 Quadrant plays on its own Channel from its own Start Note, as if it were a separate controller. Defaults: Upper Right, channel 10, note 36. |
| Top 1-8, Side 1-8 | each button runs an action, with an optional light color (a raw Launchpad velocity, 0 for the usual channel color). Reset to Defaults restores the factory set. |

Each button lights to show what its action is doing, in a color from a fixed set, or the button's own
color if you set one:

| Action | Lit while |
|---|---|
| Echo On/Off | Echo is on |
| Stutter Length | that length is stuttering |
| Looper: 1-Button, Play/Arm/Rec, Rec/Arm | armed (amber), recording or overdubbing (red), playing (green), stopped with a loop (dim green) |
| Looper: Play | playing |
| Looper: Stop | stopped with a loop |
| Looper: Pause | paused |
| Looper: Overdub | overdubbing |
| Looper: Stop/Clear/Undo, Un-Clear | a cleared loop can be brought back |
| Looper: Un-Overdub, Re-Overdub | there's a layer to remove, or to bring back |
| Looper: Double, Half | blinks the loop's length in bars when pressed |
| Note Delete, and any button | held |
| Live Ch, Octave Up/Down | that channel or octave is on |
| LP Quad, Note Toggle, CC Toggle | on |

All the buttons are painted when a Launchpad first sends something to the FoxWizard or is plugged in.

## Looper

One loop, shared by the whole FoxWizard. Set Button Action to Looper, or
use Note/CC to Feature and select Looper, or set a Looper Button trigger
(a Note or CC, with Learn) in the Looper row.

| Setting | What it does |
|---|---|
| Length | Free (the first take sets it) or 1-16 bars |
| Quantize | Off, or snap recorded notes to a division |
| Auto Overdub | goes straight into overdub after the first take |
| Record After Routes | on: records what the routes send. Off: records the notes coming in, and plays them back through the routes. |
| Hold Clears | on: hold clears, double press stops. Off: the other way round. |
| Global Looper | also records notes no route matches |
| Record CCs | records CCs too, not just notes |

The loop is saved to flash a few seconds after things go quiet, and comes
back at power on.

### 1-Button

| Press | Does |
|---|---|
| Press | empty: arm, then the first note records. Recording: end the take and play it (Free), or restart the take (bars). Playing: overdub on or off. Stopped: play. |
| Hold (0.5 s) | clear. Pressed again after a clear: the loop comes back and plays (Un-Clear). |
| Double press (within 1 s) | stop |

Hold Clears off swaps hold and double press.

### Looper actions

| Action | Does |
|---|---|
| 1-Button | the gestures above |
| Play/Arm/Rec | a single press of 1-Button |
| Stop/Clear/Undo | stops, the next press clears, the next brings it back |
| Play, Stop, Pause, Overdub, Clear, Rec/Arm | as named |
| Un-Clear | brings back a cleared loop |
| Un-Overdub | removes the newest overdub layer, ending an overdub in progress first |
| Re-Overdub | brings back the overdub layer removed most recently |
| Double | the loop plays twice in a loop twice as long, ready to overdub something longer |
| Half | plays the first half. Half then Double brings the original back. |

With no loop yet, Double and Half change the Length the next take gets, 1 to 16 bars, and Free counts as
1 bar. Armed or recording at a set length, the take changes length right away. The change lasts until
you Save to Device again. Either way, the lights blink the new number of bars.
| Note Delete | hold it and play notes to erase them from the loop, see below |

### Note Delete

Hold Note Delete while the loop plays, and the notes you play don't sound.
Instead, each one erases itself from the loop: whenever the loop reaches
a note you're holding, that note is removed, along with its end. Notes
the loop plays while you're not holding them stay. Arp division notes
recorded in the loop erase the same way, so you can take out a division
change. Letting go of Note Delete stops it. Erased notes are gone for good. With the loop stopped
or empty, nothing happens and your notes play as normal.

With Record After Routes on, a note matches the pitches its routes would
make from it (Scale, Auto Chord, transpose, arp octaves), on any channel.
With it off, it matches the note and channel you played.

By default Note Delete is Launchpad Side 6, the third button up from the
bottom.

Up to 16 overdub layers can be stepped back and forth. A layer removed
with Un-Overdub can still come back after newer overdubs. Removed layers
aren't saved, so only the layers playing survive power off.

## Configuring with Patchling

1. Open the Patchling web app and connect the device over USB-C.
2. Click **Load from Device** to pull its current settings, or start
   from the current in-app state.
3. Edit settings and routes.
4. Click **Save to Device** to write everything and commit it to flash,
   or turn on **Autosave**.

Nothing is sent to the device until it's saved.

## Presets (files)

- **Download Preset**: saves the current in-app state to an XML file.
- **Upload Preset**: loads an XML file into the app. Save to Device
  afterward to push it to the hardware.

## Factory Reset

Hold the button down, then plug in the USB-C cable while still holding
it. Resets Mode, button action, brightness, features and all routes to
factory defaults, and writes that to flash.

## Firmware Update

When the FoxWizard connects, Patchling shows its firmware version in the
badge at the top right.

- Green: firmware is up to date. Click it to confirm.
- Yellow: an update is available. Patchling also asks when it connects.
- Red: not connected. Click it to try connecting again.

Updates keep saved settings and routes.

### Firmware 1.2 and later

1. Click the yellow badge, then **Update Now**.
2. Keep the board plugged in. It restarts by itself and reconnects.

### Firmware 1.0 (once, by hand)

1. Download the `.uf2` firmware file from
   [woz.lol/foxwizard-midi-multitool-firmware](https://woz.lol/foxwizard-midi-multitool-firmware/).
2. Unplug the board from USB.
3. Short the two `FIRMWARE` pads together.
4. While still shorted, plug the board into your computer via USB-C.
5. A drive named `RPI-RP2` appears.
6. Release the short.
7. Drag the `.uf2` file onto the `RPI-RP2` drive.
8. The board reboots automatically once the copy finishes.

The same steps recover a board from any firmware.
