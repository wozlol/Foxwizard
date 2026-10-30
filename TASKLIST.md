# FoxWizard task list

Order matters. Finish the item you are on before starting the next. Anything new goes at the end of its section. Version at last edit: firmware 2.0 (test build), host brain 8, protocol 6.

## Now (needs the hardware)

- [x] Stack proof (1.123 DIAG build): the dive used 6,192 bytes of stack, the unit carried on, four arps, Velocity and a session healthy after. Left: the soak on the release candidate, `./clock_check soak`, phase 1 without the TRS loopback, phase 2 with it.

- [ ] Round Robin Velocity on the unit (1.119, protocol 7): 3 channels, soft, middle and hard notes land on ranks 1, 2 and 3, Route Velocity % 200 and 50 move them, a held key pressed again moves, no stuck notes. `./clock_check version` shows protocol 7.
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

- Main stack: Green. The stack is 8,192 bytes, not 2,048 (arduino-pico gives a single core sketch the whole scratch block), proven on 1.123: a dive made the unit use 6,192 bytes and carry on. Deepest ordinary use is 1,328 (Save to Device with 2 preset slots, was 1,616 before 1.121). Levels: Green up to 4,096, Yellow up to 6,144, Red beyond. Redo the numbers if `setup1` or `loop1` is ever added. One unexplained read of 1,744 (304 free, Red) on 1.119 after a flash, with Patchling in use, not reproduced. 1.120 moves 640 bytes of scratch (arpStep 384, stutterUpdate 256) off the stack, the biggest frame in the firmware went from 504 to 216 bytes. Heap cost 640 bytes. The deepest path is Save to Device with 2 or more preset slots, 1,616 on 1.120: the settings image, then the preset file through LittleFS (its commit chain is 5 frames of 130 to 220 bytes), then the flash, all inside the sysex handler inside the USB reader. 1.121 runs the save from the top of the main loop. The real stack is 2 KB at the top of a 4 KB block, the lower 2 KB is unused, so an overflow would not reach a variable at once, but the readout stops at 2,048 and does not see it. `SET_PRESET_SLOTS` and Factory Reset still write inside the handler. `-fstack-usage` lists frame sizes: `arduino-cli compile --build-property compiler.cpp.extra_flags=-fstack-usage`, read the `.su` files. Levels by bytes free: Green 700 or more, Yellow 500 to 700, Red under 500. One earlier read of 1,488 did not repeat. **Keep an eye on it.** Re-measure after anything that adds nesting or locals in the note path, and before the TinyUSB swap.
- Heap: lowest 29 KB of 44 KB. Keep at least 20 KB for MIDI 2.0.
- Saved settings image: 4,024 of 4,096 bytes at the worst case, 72 bytes of padding. Swing used 2 spare bytes (arp 15 and 44, looper 11), no growth. A per-route field does not fit.
- RAM at build: 83% used. Program flash 18%. Filesystem 1 MB, nearly empty.

## Version numbers

- The firmware version goes out as two single bytes in the identity reply, so neither may pass 127. 1.128 sent 0x80, a Note Off status, which cut the reply short: Patchling showed "old firmware", `clock_check version` said "No answer", and the unit looked dead. Fixed by moving to 2.0, a compile time check in the .ino and a check in `build_firmware.sh` stop it happening again. 2.0 is a placeholder name on a test build, nothing is published, pick the release number before publishing. `./clock_check ident` shows the raw bytes of the reply.

## Found by the soak test

- [ ] A two way flood on the USB device port, about 3,000 messages a second sustained, still restarts the unit (watchdog, stage 84, in the TinyUSB read), see `DIAGNOSTICS.md`. 1.125 batches controller data for USB (2 ms), which took 1,000 to 2,000 a second from always restarting to 12 of 13 runs holding. A longer wait, paced reads and a 256 byte buffer did not help. Retest after the TinyUSB swap for MIDI 2.0.

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
