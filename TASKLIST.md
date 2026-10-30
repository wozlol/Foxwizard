# FoxWizard task list

Order matters. Finish the item you are on before starting the next. Anything new goes at the end of its section. Version at last edit: firmware 1.118, host brain 8, protocol 6.

## Now (needs the hardware)

- [ ] Round Robin Priority on the unit: the Priority switch shows (protocol 6), the numbers follow the click order, Save then Load keeps them, two routes keep their own orders. Steps in `~/Desktop/FoxWizard-preset-test/TEST_PROCEDURE.md`, 3c.
- [ ] Presets, the rest of the kit steps: power cycle comes back in the same slot, factory reset leaves 1 slot, the clock during a switch (`./clock_check out 40 --expect 120` while pressing the front button).
- [ ] Confirm the 1.93 clock fix against Ableton (does the master clock still fall behind?).

## Done and tested on the unit

- [x] UMP link between the brains (1.99, host brain 8) and its clock check.
- [x] The 1 MB filesystem layout (1.100), and an update over MIDI across it.
- [x] Presets (1.102 to 1.105): slots, front button and mapped action, Patchling following the unit, silencing every route's destinations and releasing held keys on a switch.
- [x] Live Ch: held notes released and played again on a channel change, a stop for the route's own channels, Launchpad lights by distance (1.106, 1.107).
- [x] The USB stall under an aftertouch flood (1.111, 1.113), see `DIAGNOSTICS.md`.
- [x] Aftertouch thinning on the DIN port (1.115 thresholds, tested on 1.116): at 1,600, 3,200 and 6,400 a second the DIN queue peaked at 25%, 51% and 62%, input was never held back, 3,169, 12,598 and 15,328 updates were thinned.
- [x] Looper Record Quantize (1.118): the start goes to the grid and the note keeps its played length. `./clock_check loopswing` at 40, 100 and 150 ms notes, 6 of 6 each, no Note On lost with Record After Routes on. `tests/looper_quantize_test.cpp` passes.
- [x] Swing (1.116): arp Swing 54 to 75% with a 1/16 or 1/8 base, and Looper Quantize Swing. `./clock_check swingtest` measured the first step of a pair at 54.0, 62.0, 66.0, 74.9 and 75.0% with the pair length unchanged. `./clock_check all --with-arp --send-clock` passes 10 of 10 with a 66% arp playing. `./clock_check loopswing` records a loop on the unit and plays it back, 6 of 6 with 150 ms notes (see Parked for short notes).

## Resource watch

Numbers from `./clock_check version` and `session`, the thresholds are in `DIAGNOSTICS.md`.

- Main stack: Yellow, just. 1,376 of 2,048 bytes deepest on 1.118 after a power cycle and the loop tests, 672 free. It was 1,328 (720 free, Green) on 1.116, the looper's note length rule added 48. Levels by bytes free: Green 700 or more, Yellow 500 to 700, Red under 500. One earlier read of 1,488 did not repeat. **Keep an eye on it.** Re-measure after anything that adds nesting or locals in the note path, and before the TinyUSB swap.
- Heap: lowest 29 KB of 44 KB. Keep at least 20 KB for MIDI 2.0.
- Saved settings image: 4,024 of 4,096 bytes at the worst case, 72 bytes of padding. Swing used 2 spare bytes (arp 15 and 44, looper 11), no growth. A per-route field does not fit.
- RAM at build: 83% used. Program flash 18%. Filesystem 1 MB, nearly empty.

## Waiting for your answer

- [ ] Clock rounding (carry the leftover microsecond between ticks): about 16 ppm at 120 BPM. Cheap, can wait.
- [ ] USB 2.0: high-speed USB (not possible on the RP2040) or MIDI 2.0 over USB (in the plan)?

## MIDI 2.0 (status and plan in `MIDI_2_PLAN.md`)

- [x] `ump.h` and its tests. The link carries UMP.
- [ ] Main brain onto the vendored TinyUSB with the MIDI 2.0 device class. A switch, off by default. Measure stack and heap before and after. Batch every USB call.
- [ ] The sidecar's MIDI 2.0 host class (needs a MIDI 2.0 device to test).
- [ ] Higher-resolution internal events, MIDI-CI, groups in routes, 16-bit velocity in the looper.

## To test on hardware (from earlier)

- [ ] Held notes in loops (1.90 and later): a chord into the next layer across Double, a note carried through the loop top, the JamJum into a loop.
- [ ] Aftertouch velocity in loops (1.88 and later).
- [ ] Auto and Loop Lock with the Zoom (1.92 and later): follow, lead when its clock stops, lock back on, Restart on Clock Start. Which Zoom, and does it send Start and Stop?
- [ ] JamJum lag and stuck notes with Bluetooth off. The aftertouch flood crash fixed in 1.113 may have been behind some of it. If it still happens, copy the host report before power cycling.

## Parked


- Akai MPK mini mk4 on hub 2 (slow, then stuck notes). Isolate it without the hub first.
- A loop used only through Launchpad buttons may not count as using the clock, so Auto and Loop Lock would not apply.
- Arp Bypass and Bass with loop playback (untested, the fuzz cannot model the arp).
- Record After Routes on: the loop's Note Off can cut a note you hold live.
- Route 2's inputs in your preset skip USB Host 5.
- ESL pass over the whole manual.
- Thinning for the sidecar's DIN ports and the USB host devices (only the main brain's DIN port is thinned).
- Presets later: slot names, copy and clear a slot, a Launchpad light for the active slot.
- 1.118 is built live and `host-edition` is pushed. Left for you: merge to `main` if wanted, copy `mmm_web_interface/` to the Patchling site (woz.lol/fox), upload the `.uf2` to woz.lol/foxwizard-midi-multitool-firmware. ARPnMIDI has the same clock stall-guard bug in its `clock_engine.cpp`, and may have the same USB per-packet pattern.
