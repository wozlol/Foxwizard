# FoxWizard Mini MIDI Multitool, User Manual

## Overview

USB-C to TRS MIDI adapter and router. Onboard memory for 16 switchable presets, with up to 60 programmable routes each, live performance features (Arp, Scale, Auto Chord, Echo, Stutter, Looper, Launchpad, Per-Note Parameter Lock), all handled live over MIDI via a web app called Patchling.

NOTE: This manual covers multiple versions and configurations of FoxWizard. The only difference is the number of ins and outs. All features (that don't depend on more ins and outs) work on all versions. The Host Edition series adds support for up to seven USB Host ports (using hubs), 5 more TRS outs, one additional TRS in, or future BLE.

[`Foxwizard Host Edition/README.md`](Foxwizard%20Host%20Edition/README.md).

## V2 Highlights

- **16 onboard presets.** Each holds its own routes (up to 60), arps, Echo, Stutter, Looper settings, mappings, Launchpad buttons, Scale and clock settings. Switch from Patchling, the front button, or a Note, CC or Launchpad button. A switch silences everything, lets go of held keys, and flashes the slot number without holding anything up. See Presets.
- **Round Robin Priority and Velocity.** Priority gives each new note the first free output channel by rank, and the oldest note gives up its channel when all are down. Velocity sends each note to a channel by how hard it is played, the softest to rank 1. Set the rank by the order you select the channels.
- **Swing.** On each arp (54 to 75%, every second 16th or 8th), and on the Looper's Record Quantize grid. The MIDI clock stays straight.
- **Live Ch changes that keep your notes.** Held notes end on the old channel and start again on the new one. One extra stop between channel 16 and 1 brings back the route's own channels and its arp. The Launchpad buttons light by how far you have moved.
- **Auto clock.** Follows an external clock, takes over with its own when that stops, and locks back on. Loop Lock keeps a loop in the beat. Restart on Clock Start is a switch.
- **A looper that keeps your performance.** Layer Undo and Redo, Double and Half, Note Delete, held notes carried across layers and the loop top.
- **Aftertouch that cannot jam the DIN port.** Poly aftertouch and channel pressure thin out gracefully when a flood is more than DIN can carry, and USB stays steady under 1,600 aftertouch messages a second.
- **MIDI 2.0 ready.** The link between the main board and the Host Edition board carries MIDI 2.0 packets. USB MIDI 2.0 is next. See `MIDI_2_PLAN.md`.
- **Patchling keeps up with the unit.** It follows presets switched on the unit, asks before dropping unsaved changes, and has Autosave.
- **Built to be measured.** `DIAGNOSTICS.md` and the `clock_check` tool test the clock, memory, stuck notes and restarts on a real unit.

## Connections

- USB-C: connects to a computer or USB MIDI host.
- TRS MIDI IN: 3.5mm TRS-A OR TRS-B OR Classic DIN MIDI input.
- TRS MIDI OUT: 3.5mm TRS-A OR TRS-B OR Classic DIN MIDI output.
- Button: single multi-function button.

## Host Edition ports

The Host Edition adds a host brain to the FoxWizard, and with it more ports. All of them route through the same routes.

| Port | In | Out | Notes |
|---|---|---|---|
| USB Device (USB-C) | 1 | 1 | to a computer |
| TRS/DIN | 1 | 1 | the FoxWizard's own |
| USB Host 1-7 | 7 | 7 | one device each. The board's hub has four jacks, a 7-port hub or two chained 4-port hubs gives all seven |
| TRS 2 | 1 | 1 | needs the TRS board. Patchling shows TRS 2-6 only while a Host Edition with the TRS board is connected |
| TRS 3-6 | | 4 | needs the TRS board |

That is **13 MIDI connections** on the board, 7 inputs and 11 outputs to route between, and a hub takes it to 16 connections, 10 inputs and 14 outputs. Without the TRS board it is 7 connections on the board, 6 inputs and 6 outputs. Up to 60 routes.

Up to seven USB devices work at once, whichever hub jacks they're plugged into. An eighth waits and starts working when a port frees up. Patchling shows only the USB Host ports there's room for. With no hub, that's one, for a device plugged straight in. With a hub it's the hub's jacks, up to seven, and the next port shows up once a device is plugged into the one before it. A hub made of more than one chip, or a hub plugged into a hub, works too. Seven devices on bus power is a lot for one USB supply, so power the hub if they don't all come up.

## Status LEDs

- IN LED: flashes on TRS MIDI input activity.
- USB LED: flashes on USB MIDI activity.
- OUT LED: flashes on TRS MIDI output activity.
- Brightness: 0-10 steps, set with the Lights knob in Patchling.

With Button Action set to Looper, the lights show the looper instead: OUT recording, IN armed, USB playing. After Double or Half, all three flash together once per bar of the new loop length.

## Modes

Set in Patchling with the Main Mode switch.

- **Adaptor** (default): USB in to TRS out. TRS in to USB out.
- **Router**: TRS in to TRS out and USB out. USB in to USB out and TRS out. Full merge, unless a route intercepts a data type.

A message no route matches falls through to the current mode.

## Button

One action, set in Patchling with Button Action:

| Action | Settings |
|---|---|
| Panic (default) | |
| Preset | each press switches to the next preset, see Presets |
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
| Round Robin | each new note to the next selected output channel, a random one (Rand), the first free one (Priority), or one chosen by its velocity (Velocity) |
| Round Robin: Priority | each new note takes the first free selected output channel, lowest number first. If that one is already sounding, the next one up, and so on, and with every channel down the oldest note gives up its channel. To rank the channels differently, clear them and select them again in the order you want: each shows its rank in its corner. A route you leave in the default order costs nothing in a preset, and a handful of routes can each have their own order |
| Round Robin: Velocity | each new note goes to a selected output channel by its velocity. The velocity range is cut into equal parts, one per selected channel, and the softest part goes to the channel ranked 1 (ranked as in Priority). 2 channels split at 64, 4 channels by quarters. Route Velocity % is applied first, so above 100% pushes notes into the higher channels and below 100% into the lower ones. It does not rotate, notes of the same part share a channel. |
| Looper | whether this route's notes are recorded (shown once the looper is set up) |

Routes are evaluated top to bottom. Later routes override earlier ones targeting the same output. Drag to reorder in Patchling.

Below these, each route has a switch for each feature. Switches that are off stay on top, and features switched on open up below them.

## Global Clock

Shown while something uses the clock (Arp, Echo, Stutter, Looper, ramps, Tap Tempo).

| Setting | Options |
|---|---|
| BPM | 20-300, while the FoxWizard is master |
| Clock | FoxWizard is master, Auto (the default), or Follow a port. Following a port ignores clock from every other port. |
| Send Clock to every port | master, and Auto while it uses its own clock |
| Time Signature | 1-16 beats over 2, 4, 8 or 16 |
| Tap Tempo Trigger | a Note or CC, with Learn |

A loop recorded at Free length sets the tempo from its length.

### Clock: Auto

In Auto, the FoxWizard uses a clock from outside when one comes in. When no clock comes in, it uses its own clock. This is the same Auto that other devices have. Auto is the default. A preset saved with an earlier setting keeps that setting.

| The FoxWizard is | When |
|---|---|
| following | a clock is coming in. It follows the first port that sends a clock. It ignores the clock bytes from other ports while that clock is coming in. |
| master | no clock is coming in. It runs its own clock. It starts at the BPM setting. If it was following before, it keeps the tempo of that clock. |

The FoxWizard decides that a clock is gone when it receives no clock byte for half a second. At a very slow tempo it waits a little longer. When a clock comes in again, the FoxWizard follows it again. It needs no setting change.

The FoxWizard adds these things to standard Auto:

- **The beat count does not stop.** The FoxWizard counts the beat all the time. When it changes between following and master, the count goes on from where it was. The arps and the looper keep their time.
- **Start restarts the count.** A Start message from the clock it follows sets the count to 0, so the beats line up with that device.
- **Stop does not stop the FoxWizard.** When the clock it follows sends Stop, the FoxWizard becomes master and goes on. The clock bytes that arrive after Stop are not counted. Start or Continue from that device makes the FoxWizard follow it again.
- **Send Clock works when it is master.** If Send Clock to every port is on, the FoxWizard sends its own clock whenever it is master in Auto.
- **A Free loop sets the tempo only when it is master.** Tap Tempo works only when it is master too.

### Arps on a followed clock

This section is about following a clock, in Follow a port and in Auto while the FoxWizard is following.

With Clock set to Follow a port, the FoxWizard counts that port's MIDI Clock bytes. Each byte is 4 pulses, 96 pulses to a quarter note. A division is a whole number of pulses (1/16 is 24, six clock bytes) and its grid lines are the multiples of that number, counted from 0.

- **The count** restarts at 0 on a Start from the followed port and when Clock is set to Follow. It carries on through Continue and holds at Stop. Song Position Pointer doesn't move it. An arp steps only while the clock is arriving.
- **A new arp phrase** doesn't restart the count. After a 6 ms window to catch the whole chord, its first step plays at the next grid line, at once if the count is exactly on one, otherwise up to one division later. Every step after that is on a grid line, on the first clock byte at or after it.
- **Beats:** the grid lines match the source's own beats when the source sent Start as it began playing. If the FoxWizard began following mid-song, the arp keeps the source's tempo, but its grid lines sit wherever the count began.

As master, a phrase that starts with no other arp running restarts the count, so its first step plays at once. This is also true in Auto while the FoxWizard is master.

## Divisions

Arp, Echo and Stutter each have a division table: a default division, plus up to 7 more squares. Each square is a division and a learned Note or CC trigger. Pressing a square's trigger switches to its division.

Divisions run 1/1 to 1/64T, with dotted (D) and triplet (T) values. Stutter also has 2 bars and 4 bars.

## Scale and Auto Chord

The Scale row sets one root and scale for the whole FoxWizard, and shows them on a piano and a guitar neck. Scales: Major, Minor, Harmonic Major, Harmonic Minor, Melodic Major, Melodic Minor, Major Blues, Minor Blues, Major Pentatonic, Minor Pentatonic, Dorian, Mixolydian.

- **Scale** (per route): keeps the route's notes in the scale. Layout: Next Note Up, White Keys, Drum Pad 5ths, Drum Pad 4ths, Drum Pad 3rds.
- **Auto Chord** (per route): each note plays a chord. Pick its notes as scale positions with Scale on, or as intervals without it.

## Arp

Up to 4 routes can arp at once, all on the Global Clock.

| Setting | Options |
|---|---|
| Mode | Up, Down, Up-Down 1, Up-Down 2, Drum Roll/Chord Pulse, Random, As Played, Converge, Diverge, Thumb, Pinky, Up 4-Step, Rhythm, Ostinato, Octave Walk, Fifth, Bass+Chord, Chord+Run |
| Octaves | 1-4 |
| Gate % | 5-100 |
| Swing | Off, or 54 to 75%. Delays the off-beat steps and nothing else, so the arp has a shuffle and the MIDI clock stays straight. 50% is straight, 66% is a triplet feel, 75% is the most. Each arp has its own |
| Swing Base | shown when Swing is on: every second 16th (default) or every second 8th is delayed |
| Velocity | Played, Fixed, or Aftertouch |
| Bypass ch | also sends the played notes, unarped, on this channel |
| Bass ch | also plays the lowest held note an octave down on this channel |
| Hold | a CC that holds the arp's notes, with Learn |
| Division | division table. The default square's trigger turns the arp off and on. |

### Swing

Swing delays the second note of each pair and leaves the first where it is, so every pair keeps its length and nothing drifts. The amount is the share of the pair the first note takes: 50% is straight, 66% is a triplet feel, 75% is the most. The MIDI clock out and a clock the FoxWizard follows are never touched. It follows tempo changes and Start, Stop and a following clock, since it is worked out in clock pulses. Echo and Stutter are not swung, they capture things as they are. Played notes passing through are not swung.

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

Pressing a division square repeats that length of what was just played, over and over. Live notes still play over the top. If a stutter is longer than what's been played so far, it waits until there's enough.

| Setting | What it does |
|---|---|
| Global Stutter | stutters everything the FoxWizard sends, not just this route |
| Stutter Latching | off (default): stutters while a square is held. On: a press starts it, the same press again stops it. |
| Stutter After Route | on (default): stutters what the route sends. Off: stutters the notes coming in, so the repeats go through the route too. |
| Runout Bars | Off (default), or stop by itself after 2, 4, 8 or 16 bars |
| Division | division table, up to 4 bars |

Pressing another Echo or Stutter square while one is on switches to it.

## Per-Note Parameter Lock (P-Lock)

Hold a key and move a CC: that CC value is locked to the key, and sent again each time the key plays. Up to 256 locks, kept through power off.

| Setting | What it does |
|---|---|
| Glide | off: the locked values jump. On: they slide from the last value sent. |
| Delay | None sends the locked values before the note. A division waits that long first, or with Glide on, slides over that long. |
| Clear Locks | forgets every lock |

## Note to Feature and CC to Feature

Up to 16 Notes and 16 CCs, each set to an action, with Learn. The same action list drives the Launchpad buttons.

| Action | |
|---|---|
| Note, Note Toggle | |
| CC Momentary, CC Toggle, CC Value, CC Level | |
| Pitch Ramp, Mod Ramp, CC Ramp | sweep over a division |
| Transport | Play, Stop, Continue, Pause, Record, Record Exit, Rewind, Fast Forward, Return to Start (MMC, and MIDI Start/Stop/Continue) |
| Program Change, Program Up/Down | |
| Live Ch, Live Ch Up/Down, Live Velocity | change a route's output channel or velocity while playing. Live Ch Up/Down steps through channels 1 to 16 and one more stop between 16 and 1: the route's own channels, with everything the live channel switched off back, like its arp. Keys held when the channel changes are released on the old channel and start again on the new one |
| Octave Up/Down | |
| Arp Div Up/Down | |
| Echo On/Off | |
| Stutter Length | 1/8 bar to 4 bars |
| LP Quad | turns Break Off a Quad off and on |
| Looper | any looper action, see Looper |
| Tap Tempo, Panic, Clear P-Locks | |
| Preset | Next, Previous, or Slot 1 to 16, see Presets |

## Launchpad

Turn Launchpad on for the route a Novation Launchpad (in Programmer mode) plays into. The grid becomes four 4x4 note blocks from note 36. The pads light up with whatever notes play back to them, in each channel's color.

| Setting | What it does |
|---|---|
| Octave | 0, -1 or -2 |
| Break Off a Quad | one 4x4 block of pads plays as if it were a separate controller. Quadrant picks the block, Quadrant Channel is the channel it plays on, and Quadrant Start Note is its first note. Defaults: Upper Right, channel 10, note 36. |
| Top 1-8, Side 1-8 | each button runs an action, with an optional light color (a raw Launchpad velocity, 0 for the usual channel color). Reset to Defaults restores the factory set: Top 1 Live Ch Down, Top 2 Live Ch Up, Top 3-8 Stutter Length from 4 bars down to 1/8 bar, Side 1 Layer Undo, Side 2 Layer Redo, Side 3 Double, Side 4 Half, Side 5 LP Quad, Side 6 Note Delete, Side 7 Play/Arm/Rec, Side 8 Stop/Clear/Undo. |

Each button lights to show what its action is doing, in a color from a fixed set, or the button's own color if you set one:

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
| Looper: Layer Undo, Layer Redo | there's a layer to remove, or to bring back |
| Looper: Double, Half | blinks the loop's length in bars when pressed. Then the one it's moved toward is lit, green after one step from where the loop started, yellow after two, red after three or more, and neither at the start. A loop starts from its own take. Before there's a loop they start from the Length setting. Clearing the loop goes back to the start. |
| Note Delete, and any button | held |
| Live Ch, Octave Up/Down | that channel or octave is on |
| Live Ch Up/Down | lit the way the channel has moved from the route's own: green after one step, yellow after two, orange after three, red after four or more. Up counts channels 1 to 8. From channel 9 the Down button counts the way back, 16 is one step, and neither is lit at the route's own channels |
| LP Quad, Note Toggle, CC Toggle | on |

All the buttons are painted when a Launchpad first sends something to the FoxWizard or is plugged in.

## Looper

One loop, shared by the whole FoxWizard. Set Button Action to Looper, or use Note/CC to Feature and select Looper, or set a Looper Button trigger (a Note or CC, with Learn) in the Looper row.

| Setting | What it does |
|---|---|
| Length | Free (the first take sets it) or 1-16 bars |
| Quantize | Off, or snap recorded notes to a division. Each note's start moves to the grid and the note keeps the length it was played, so staccato notes and drum hits stay short and none is lost |
| Quantize Swing | Off, or 54 to 75%. Swings the Quantize grid: every second line is later, so notes snap to a swung grid as they are recorded and play back exactly as recorded. Grey while Quantize is Off. A loop recorded without Quantize stays exactly as played |
| Record After Routes | on: records what the routes send. Off: records the notes coming in, and plays them back through the routes. |
| Record Unrouted Notes | also records notes that no route matches. It only works when Record After Routes is off. When Record After Routes is on, the loop already records everything the FoxWizard sends, so this switch is grey. |
| Auto Overdub | goes straight into overdub after the first take |
| Hold Clears | on: hold clears, double press stops. Off: the other way round. |
| Record CCs | records CCs too, not just notes |
| Restart on Clock Start | when the FoxWizard follows a clock and that clock sends a Start message, the loop plays again from its top. Off: the loop keeps playing and stays in time with the clock. See Loop Lock. |

### Loop Lock

Loop Lock keeps the loop in time with a clock from outside. It works when Clock is set to Follow a port, and when Clock is set to Auto while a clock is coming in. It has no setting of its own.

- **The loop lasts a whole number of beats.** The FoxWizard makes the loop a little faster or slower, so that it fits the tempo of the clock. The loop stays at the same place in the beat, so it does not drift.
- **A loop recorded at another tempo** plays at the tempo of the clock. Its notes keep their places inside the loop. Set Length to a number of bars for the best result. A Free loop is held to the nearest whole beat.
- **In Auto, when the clock goes away,** the loop keeps its last speed and stays in time with the FoxWizard's own clock. When the clock comes back, it follows the clock again.
- **A Start from the clock** sets the beat count to 0. A Start message is what a device sends when you press play on it. The loop keeps playing and keeps its place, unless Restart on Clock Start is on. Then the loop plays again from its top, so the loop and the clock start together.
- **Loop Lock stops** when you clear or stop the loop, or when Clock is set to master.

While no clock is coming in, the FoxWizard and the other device each keep their own time. They can move apart by a few milliseconds each minute. Only a clock between them stops that.

The loop is saved to flash a few seconds after things go quiet, and comes back at power on.

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
| Layer Undo | removes the newest overdub layer, ending an overdub in progress first |
| Layer Redo | brings back the overdub layer removed most recently |
| Double | the loop plays twice in a loop twice as long, ready to overdub something longer |
| Half | plays the first half. Half then Double brings the original back. |

With no loop yet, Double and Half change the Length the next take gets, 1 to 16 bars, and Free counts as 1 bar. Armed or recording at a set length, the take changes length right away. The change lasts until you Save to Device again. Either way, the lights blink the new number of bars.
| Note Delete | hold it and play notes to erase them from the loop, see below |

### Held notes and the arp

While a loop is recording, armed or playing, the arps keep one beat grid. A new phrase doesn't start a new grid the way it does with no loop, so an arp on playback lines up with the way you played it. A loop that starts from its top starts the grid there, and a take plants it at the first arp phrase.

A note held by you and by the loop stays held until both let go. If two layers hold the same note and overlap, it stays held until the last of them lets go. The loop letting go of a hat you're still holding doesn't stop your roll, and you letting go of one the loop still holds doesn't either. Each hold of a note in the loop counts, whichever layer or pass it came from. The loop plays the Note On when the first one starts and the Note Off when the last one ends, and drops a Note Off nothing is holding, as ARPnMIDI's output count does. A Note On a device repeats while the note is held isn't recorded twice.

An arp set to Aftertouch velocity keeps the pressure that sets its velocities with the loop, when the loop records inputs and the route's Looper switch is on, so playback has the velocities you played. The values are thinned the way ARPnMIDI thins recorded CCs.

A note you hold during an overdub is recorded as held everywhere you held it. That includes a note you were already holding when the overdub started, one held across the top of the loop, and one held for longer than the whole loop, which is held all the way round. A held note goes into the loop when you let go of it, so it plays back from the next time round, except one held for the whole loop or longer, which the loop takes over from you the moment you let go, so it doesn't drop out until it comes round again. A note held through the top of the loop, or from one layer into the next, plays straight through with no gap where it changes over.

### Note Delete

Hold Note Delete while the loop plays, and the notes you play don't sound. Instead, each one erases itself from the loop: whenever the loop reaches a note you're holding, that note is removed, along with its end. Notes the loop plays while you're not holding them stay. Arp division notes recorded in the loop erase the same way, so you can take out a division change. Letting go of Note Delete stops it. Erased notes are gone for good. With the loop stopped or empty, nothing happens and your notes play as normal.

With Record After Routes on, a note matches the pitches its routes would make from it (Scale, Auto Chord, transpose, arp octaves), on any channel. With it off, it matches the note and channel you played.

By default Note Delete is Launchpad Side 6, the third button up from the bottom.

Up to 16 overdub layers can be stepped back and forth. A layer removed with Layer Undo can still come back after newer overdubs. Removed layers aren't saved, so only the layers playing survive power off.

## Aftertouch thinning

The DIN output moves about 3 KB a second. When a flood of poly aftertouch or channel pressure fills its queue past a quarter, only every 2nd update goes out, then every 4th, 8th or 16th as it fills, for each note on each channel. Every note keeps getting some, with bigger gaps, instead of the whole input waiting behind them. A pressure of 0 always goes. Below a quarter full nothing is thinned.

## Presets

The unit keeps up to 16 presets and runs one at a time. A preset is everything Patchling edits: routes, arps, Echo, Stutter, Looper settings, mappings, Launchpad buttons, Scale, clock and button settings. Set **Preset Slots** (1 to 16) next to Button Action.

With 2 or more slots, Patchling shows a button per preset above Button Action. Clicking one switches the unit to that preset and loads it into Patchling. Slots with nothing saved yet start from the factory defaults and are dashed.

On the unit, a preset switch is a Button Action of **Preset** (each press goes to the next slot and wraps) or a Note, CC or Launchpad button set to the **Preset** action (Next, Previous, or one slot by number). With several slots, any Preset mapping sets the front button to Preset too, so a preset without the mapping can always be left.

A switch silences everything first, like Panic, and stops the loop (the loop itself stays). Then the preset is live and the lights flash the slot number, slower than the loop's bar count. Playing is not held up while they flash.

Save to Device and Autosave save the preset in use. Changes you have not saved are dropped when you switch. Patchling asks first. Download Preset and Upload Preset work on the preset in use.

The slot count, the slot in use, the lights brightness and the front button being Preset belong to the unit, not to a preset. The loop is the unit's, not a preset's. A Factory Reset deletes every preset.

## Configuring with Patchling

1. Open the Patchling web app and connect the device over USB-C.
2. Click **Load from Device** to pull its current settings, or start from the current in-app state.
3. Edit settings and routes.
4. Click **Save to Device** to write everything and commit it to flash, or turn on **Autosave**.

Nothing is sent to the device until it's saved.

## Preset Files

- **Download Preset**: saves the current in-app state to an XML file.
- **Upload Preset**: loads an XML file into the app. Save to Device afterward to push it to the hardware.

## Factory Reset

Patchling's small Factory Reset button, at the bottom of the page while connected, asks once and then resets Mode, button action, brightness, features, all routes and all presets to factory defaults, and writes that to flash. Holding the button while plugging in does nothing.

## Firmware Update

When the FoxWizard connects, Patchling shows its firmware version in the badge at the top right.

- Green: firmware is up to date. Click it to confirm.
- Yellow: an update is available. Patchling also asks when it connects.
- Red: not connected. Click it to try connecting again.

Updates keep saved settings and routes.

### Firmware 1.2 and later

1. Click the yellow badge, then **Update Now**.
2. Keep the board plugged in. It restarts by itself and reconnects.

### Firmware 1.0 (once, by hand)

1. Download the `.uf2` firmware file from [woz.lol/foxwizard-midi-multitool-firmware](https://woz.lol/foxwizard-midi-multitool-firmware/).
2. Unplug the board from USB.
3. Short the two `FIRMWARE` pads together.
4. While still shorted, plug the board into your computer via USB-C.
5. A drive named `RPI-RP2` appears.
6. Release the short.
7. Drag the `.uf2` file onto the `RPI-RP2` drive.
8. The board reboots automatically once the copy finishes.

The same steps recover a board from any firmware.
