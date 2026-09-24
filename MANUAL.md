# Foxwizard Mini MIDI Multitool — User Manual

## Overview

USB-C to TRS MIDI adapter and router. Two operating modes, up to 256
programmable routes, configured from a web app called Foxwizard Patchling.

## Connections

- USB-C: connects to a computer or USB MIDI host.
- TRS MIDI IN: 3.5mm TRS-A MIDI input.
- TRS MIDI OUT: 3.5mm TRS-A MIDI output.
- Button: single multi-function button.

## Status LEDs

- IN LED: flashes on TRS MIDI input activity.
- USB LED: flashes on USB MIDI activity.
- OUT LED: flashes on TRS MIDI output activity.
- Brightness: 0-10 steps, set in Patchling under Global Settings.

## Modes

Set in Patchling under Global Settings.

- **USB Adaptor Mode** (default): USB in to TRS out. TRS in to USB out.
  Simple two-port adapter.
- **Router Mode**: TRS in to TRS out and USB out. USB in to USB out and
  TRS out. Full merge, unless a route intercepts a data type.

A route with no match falls through to the current Mode's default
behavior above.

## Button

One action, set in Patchling under Global Settings: Panic, Pitch Up,
Pitch Down, Mod, CC Momentary, CC Toggle, Octave Up, Octave Down, Note
Momentary, or Note Toggle. Momentary/Toggle behavior and channel are also
set there.

## Routes

Each route filters and transforms MIDI by:

| Field | Options |
|---|---|
| Input Device | TRS, USB, or Both |
| Input Channels | any of 1-16 |
| Data Types | Note, CC, Program Change, Pitch Bend, Poly Aftertouch, Channel Pressure, SysEx, Common, Realtime, Clock |
| Note/CC Range | start-end |
| Output Device | None, TRS, USB, or Both |
| Output Channels | any of 1-16 |
| Transpose | -64 to +63 semitones |
| Velocity Scale | 10-200% |
| CC Map | shift CC range to a new start CC |
| Aftertouch/Pressure to CC | remap to a chosen CC number |
| Mono Retrig | poly to mono, last-note priority |
| Round Robin | cycle or randomize output channel per note |

Routes are evaluated top to bottom. Later routes override earlier ones
targeting the same output. Drag to reorder in Patchling.

Set Output Device to None to block a matched message instead of passing
it through.

## Configuring with Patchling

1. Open the Patchling web app and connect the device over USB-C.
2. Click **Load from Device** to pull its current settings, or start
   from the current in-app state.
3. Edit Global Settings, add/edit/reorder/delete routes.
4. Click **Save to Device** to write everything and commit it to flash.

Nothing is sent to the device until you click Save to Device.

## Presets (files)

- **Download Preset**: saves the current in-app state to an XML file.
- **Upload Preset**: loads an XML file into the app. Click Save to
  Device afterward to push it to the hardware.

## Factory Reset

Hold the button down, then plug in the USB-C cable while still holding
it. Resets Mode, button action, brightness, and all routes to factory
defaults, and writes that to flash.

## Firmware Update

1. Unplug the board from USB.
2. Short the two `FIRMWARE` pads together.
3. While still shorted, plug the board into your computer via USB-C.
4. A drive named `RPI-RP2` appears.
5. Release the short.
6. Drag `Mini_MIDI_Multitool.ino.uf2` onto the `RPI-RP2` drive.
7. The board reboots automatically once the copy finishes.
