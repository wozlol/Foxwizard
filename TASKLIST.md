# FoxWizard task list

Order matters. Finish the item you are on before starting the next. Anything new goes at the end of its section.

## Now

- [ ] **Test the UMP link on hardware** (main 1.99 with host brain 8). Kit: `~/Desktop/FoxWizard-link-test/`, steps in `TEST_PROCEDURE.md` there. Rollback files are in the kit.
- [ ] **Run the MIDI clock check** (`./clock_check all`, in the same kit) right after the link test passes. Source: `tools/clock_check.swift`.
- [ ] Confirm the 1.93 clock fix against Ableton (does the master clock still fall behind?).

## Next: the flash layout change (a prerequisite for presets and for MIDI 2.0)

Why it is first and why it is urgent:
- The 256 KB filesystem holds the saved loop (up to 73 KB), the plock file, and the staging area for firmware updates over MIDI. A firmware image is staged there whole.
- The firmware image is 222 KB now. An update needs the image plus 16 KB free, so there is about 20 KB of growth left before an update over MIDI can no longer be staged. Every release uses some of it.
- The layout change itself must be staged by the old layout, so it has to ship while the firmware is still under about 235 KB.
- MIDI 2.0 (the upstream TinyUSB with the MIDI 2.0 classes) and 16 preset files will both need more room.

Tasks:
- [ ] Switch the main brain to `Flash Size = 2MB (Sketch: 1MB, FS: 1MB)` (`flash=2097152_1048576`). The saved settings live in the EEPROM area, which sits at the same address in every layout, so they survive.
- [ ] First boot on the new layout: the filesystem is formatted once (the saved loop and plocks are lost once, nothing else). Say so in the release notes.
- [ ] Update `build_firmware.sh`, the Host Edition README (Arduino settings table), and the comment at the top of the sketch.
- [ ] Test an update over MIDI from the old layout to the new one, and one new layout to new layout. Keep this release free of other changes.
- [ ] The sidecar is a separate board with its own flash and is not part of this.

## Next: Preset mode (plan only so far)

Decisions so far:
- A new front button action, **Preset**, cycles through the preset slots. A new mappable action, **Preset**, works from Notes, CCs and Launchpad buttons.
- When any Preset action is mapped, the front button is forced to Preset, so presets can always be changed. The front button settings show that it is set by the mapping.
- New setting in the main button area: **Preset Slots**, 1 to 16. 16 is the limit, with or without a sidecar.
- Changing preset: panic first, then load the slot, at once. The lights then flash the slot number without blocking, at the speed the loop bar count flashed before it was made twice as quick (on 140 ms, off 220 ms).
- Patchling: in the master button area, one button per preset. Choosing one loads that slot into the unit and into the editor. Edits run live on the unit (the unit's active preset matches the editor). Save to Device, Download and Upload work per slot. Autosave is a setting and is off by default. Loading a slot always happens automatically.

Space (checked in the code):
- One preset is one EEPROM image of at most 4096 bytes (the sketch asserts the worst case, 60 routes, fits). 16 presets are at most 64 KB. Typical presets are 1 to 2 KB.
- Presets are storage, not RAM. The active preset lives in the same RAM structures as today. A swap reads a file into them. RAM cost: a staging buffer of up to 4 KB, or none if it is read in pieces.
- Storage needs the layout change above. Presets do not fit with the loop and firmware staging in 256 KB. With a 1 MB filesystem they fit easily.
- Borrowing the sidecar's flash is not needed and not wanted. Most units have no sidecar, and it would add a protocol and a dependency.
- RAM now: 45 KB free of 262 KB (82% used). MIDI 2.0 should fit. Keep the looper event slot at 12 bytes (narrow the tag for 16-bit velocity) so it does not take 24 KB more. Re-measure at each MIDI 2.0 step and keep at least 20 KB free.

Tasks, in order:
1. [ ] **Decide what is per preset and what belongs to the device.** The device keeps: Preset Slots, the active slot, LED brightness, and the front button being Preset. Recommended: the loop stays global and stops on a swap (panic already stops it without clearing it). Empty slots start as a copy of the factory defaults.
2. [ ] **Storage layer.** Slot files `preset01` to `preset16` in the filesystem. The current EEPROM image keeps holding the active preset, so power-on loads as fast as now and old units keep their settings. The first run makes slot 1 from the EEPROM image. A save writes both the EEPROM and the slot file. Test the file format round trip on the Mac with the real structs.
3. [ ] **Swap engine.** A silent panic (release every held note, stop arps, echo, stutter, looper without clearing, reset note tracking, Launchpad ports re-learn), load, apply (clock config, routes, features, plocks), mirror to EEPROM. The existing panic flashes the lights and waits. The swap must not. Reuse the non-blocking count flash with the slower timing.
4. [ ] **Triggers.** The Preset front button action and the Preset Slots setting. The Preset mappable action (next, previous, and slot 1 to 16 directly), including Launchpad buttons and their lights. The forced front button rule.
5. [ ] **Protocol.** Commands for: get and set the slot count, select a slot (does the swap), save the active preset to a slot, read slot info (used or empty). Update `SYSEX_PROTOCOL.md` and bump the protocol version.
6. [ ] **Patchling.** The slot buttons in the master button area, live editing of the active slot, per-slot Save, Download, Upload, Autosave setting, the Preset Slots setting in the main button area, the greyed front button when mapped.
7. [ ] **Hardware tests.** Swap while a loop plays, with arps rolling, with a note held on each device, with the JamJum flooding aftertouch. Nothing sticks, nothing doubles, the swap is instant, the flash does not delay the next note.
8. [ ] **Manual.** A Presets section, with the rules above in plain words.
9. [ ] Later: slot names, copy and clear a slot, a Launchpad light for the active slot.

## Next: MIDI 2.0 (see `MIDI_2_PLAN.md` and the memory note)

- [x] `ump.h` and the tests in `Mini_MIDI_Multitool/tests/`.
- [x] The link carries UMP (main 1.99, host brain 8). Hardware test is in Now.
- [ ] The sidecar's MIDI 2.0 host class for MIDI 2.0 devices (needs a MIDI 2.0 device to test).
- [ ] Main brain onto the vendored TinyUSB with the MIDI 2.0 device class. A switch, off by default. The MIDI 1.0 alt setting stays byte-identical. The front button held at power-up forces MIDI 1.0 USB. After the layout change, and measure RAM first.
- [ ] Higher-resolution internal events, MIDI-CI, groups in routes, 16-bit velocity in the looper.

## Waiting for your answer

- [ ] Clock rounding (carry the leftover microsecond between ticks): cheap, small, makes the tempo true to the set BPM.
- [ ] Stutter and Echo "After Route": grey out instead of hide, like the new Looper switch.
- [ ] USB 2.0: high-speed USB (not possible on the RP2040) or MIDI 2.0 over USB (in the plan)?

## To test on hardware (from earlier)

- [ ] Held notes in loops (1.90 and later): a chord into the next layer across Double, a note carried through the loop top, the JamJum into a loop.
- [ ] Aftertouch velocity in loops (1.88 and later).
- [ ] Auto and Loop Lock with the Zoom (1.92 and later): follow, lead when its clock stops, lock back on, Restart on Clock Start. Which Zoom, and does it send Start and Stop?
- [ ] JamJum lag and stuck notes with Bluetooth off. If it still happens, copy the host report before power cycling.

## Parked

- Akai MPK mini mk4 on hub 2 (slow, then stuck notes). Isolate it without the hub first.
- A loop used only through Launchpad buttons may not count as using the clock, so Auto and Loop Lock would not apply.
- Arp Bypass and Bass with loop playback (untested, the fuzz cannot model the arp).
- Record After Routes on: the loop's Note Off can cut a note you hold live.
- Route 2's inputs in your preset skip USB Host 5.
- ESL pass over the whole manual.
- Commits on `host-edition` are not pushed (36 at last count). The Patchling site copy and uf2 upload are yours for a release. ARPnMIDI has the same clock bug: the stall guard in its `clock_engine.cpp`.
