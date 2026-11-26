# FoxWizard task list

Order matters. Finish the item you are on before starting the next. Anything new goes at the end of its section. Version at last edit: firmware 2.2 (test build), host brain 9, Bluetooth module 1, protocol 8.

## Now (needs the hardware)

- [x] Stack proof (1.123 DIAG build): the dive used 6,192 bytes of stack, the unit carried on, four arps, Velocity and a session healthy after. Left: the soak on the release candidate, `./clock_check soak`, phase 1 without the TRS loopback, phase 2 with it.

- [ ] Presets with no unit (Patchling 2.7): unplugged, set Number of Preset Slots Total and use the slot row, add routes to slots 1 and 3, reload the page and they are still there. Then plug in the unit (on 2.7): a box lists slots 1 and 3, Send to Unit replaces those slots on the unit and leaves the others, Load from Unit shows the unit and leaves the browser's presets. On 2.6 the box offers to send only the open preset. Unplug the cable with a preset on screen: it carries on as the same slot. Seen in Chrome unplugged only, the connected box is read from code.
- [ ] Preset names (main 2.8, protocol 12, Patchling 2.8): update the unit to 2.8. Type a Preset Name next to the slot buttons, press Enter, reload the page and reconnect: the name is still there and a slot button's hover text shows it. Name two more slots and the bundle in Presets. Download All: the zip is named after the bundle, the files carry the names and `bundle.txt` is inside. Upload All the same zip after a Factory Reset: every name comes back. Plug a second browser in: it shows the same names. Unknown until this runs: the clock during a name write (`./clock_check out 40 --expect 120` while renaming).
- [ ] Roll back (Patchling 2.8, the live site publishes 2.1 as `previous`): the unit on 2.8 shows a green badge. Click it. The Firmware window offers Go Back to v2.1. Go Back replaces the main firmware, the unit restarts, the badge turns yellow with Update, and Update Now installs 2.8 again. Not run on hardware. Watch for a unit that does not reconnect and for settings that 2.1 will not read.
- [ ] Presets button, All Presets (main 2.7, protocol 11, Patchling 2.7): update the unit to 2.7, set Preset Slots to 4 and save something different in slots 1, 2 and 4, leave 3 empty. Presets, Download All: a zip comes down with 3 files named 01, 02 and 04, the unit never flashes a slot number or goes quiet, and the open slot is the one on screen. Presets, Upload All with that zip while the unit is on slot 2: the box lists 1, 2 and 4, slot 3 is not touched, the unit stays on slot 2, and switching to 1 and 4 afterward gives what the zip held. Unknown until this runs: the real time per slot, and that the unit's clock does not slip during an Upload All (`./clock_check out 40 --expect 120` while it runs).
- [ ] Ports with no unit (Patchling after 2.6): unplugged, a new route shows TRS/DIN, USB Device, USB Host 1 to 7, BLE Host and BLE Device, all on, and no TRS 2 to 6. Plugged into the Hella, the ports are the Hella's own, as before. Plugged into an OG with no Host side or Bluetooth side, no USB Host or BLE chips show, even for a route made unplugged with them on. Unplug the cable, the full set comes back.
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
- [x] Burn test on 2.1 (the 1.124 candidate did 30 minutes of phase a first): phase a 10 minutes, 26,072 notes, no restart, no stuck note, stack 1,248, heap 28 KB. Phase b with the TRS loopback in and Adaptor mode forced: 10 minutes with 1 preset slot (38 saves) and 6 minutes with 2 slots (22 saves with the preset file, 23 preset switches), no restart or fault, stack 1,392 (Green, of 8,192). Then four arps, Velocity, loop quantize at 40 and 100 ms notes, swing and a session all pass. `./clock_check soak a|b MINUTES`.
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

## Editions plan (Hella and Bluetooth first, Deluxe later)

Written and compiling, not yet run on hardware (2.8 test build, protocol 12, host brain 11, Bluetooth module 13). Details in `Foxwizard Host Edition/README.md`.

- [ ] Heap and stack on the main brain with the Bluetooth support in. It adds 4.4 KB of RAM, so the lowest heap is the number to read first (the stack too after a first Bluetooth connection, the slots save has a 368 byte frame, a Green reading is under 4,096): `./clock_check version` after `session 100`. Green above 30 KB, Yellow 20 to 30, Red under 20. Was 28 KB lowest on 2.1, expect about 23 (Yellow) and say so.
- [ ] Without the Bluetooth module: the unit behaves as 2.1. `./clock_check all --with-arp --send-clock`, `session 100`, a preset save, then `soak a 5`. Nothing should differ.
- [ ] Bluetooth module alone, bench mode (`-DFW_BENCH=1`): flash it over its USB (BOOT held while plugging in), open the serial monitor at 115200. The board check line says the settings look right. BLE Device shows up in the Mac's Audio MIDI Setup, Bluetooth MIDI. Tap BOOT, turn on a Bluetooth MIDI keyboard in pairing mode, it connects and its notes print.
- [ ] Hella: wire by the chart in `~/Desktop/FoxWizard-hella-test/TEST_PROCEDURE.md` (FoxWizard GP14 to the S3's RX, GP15 to TX, and the host link now GP27 to the host's GP29 and the host's GP28 to GP26), flash the main brain 2.6, the host brain 11 and the Bluetooth module 13. `./clock_check slots` shows the module, a keyboard played over Bluetooth comes out the routes, and a Bluetooth keyboard and a USB keyboard play at once.
- [ ] Slots: with BLE in slot 8 and 5, plug USB devices until the chained-hub slots are wanted (a device behind two hubs): it waits, never a stuck note. Unplug the Bluetooth keyboard mid-note, no stuck note. Power cycle the unit, the keyboard reconnects to the same slot by itself.
- [ ] Full: with every slot taken, Patchling's Add Bluetooth Device is greyed with the reason, a fifth device is sent away.
- [ ] Names: a USB device's product name on its port chip in Patchling, and the Bluetooth keyboard's name.
- [ ] The module's USB host (never run): a USB MIDI keyboard on a powered hub into the module's USB-C. It takes a slot (slot 8 first), its name shows, it plays through a route, unplugged mid-note leaves no stuck note. A second device through the hub, both steady (the module takes 2, `USB_HOSTS_MAX`, the chip has 8 USB channels). A third is ignored until it is plugged in again with one of the two gone. A device with no slot free waits and starts when one frees up. Bench mode keeps a serial monitor, the real build has none.
- [ ] The module's USB device port (never run): Patchling's USB Port set to Device, the module restarts and a Mac lists a MIDI port named FoxWizard Bluetooth and no serial port. Notes from the Mac reach a route from BLE Device, notes sent to BLE Device reach the Mac, a phone on Bluetooth hears the same port. Unplug mid-note, no stuck note. Set back to Host. The port never falls back to Device by itself, a host that does not start keeps trying.
- [ ] Bluetooth Radio Off in Patchling (needs protocol 10): the module restarts, nothing advertises (a phone no longer sees FoxWizard), the Add Bluetooth Device button is greyed and a BOOT tap does nothing (a 3 s hold turns the radio back on), a USB device on the module still plays. On again, the devices reconnect. `clock_check bleaction radio off|on` does the same.
- [ ] Flashing by hand (`Foxwizard Host Edition/README.md`, Updating a module by hand): the Bluetooth module with BOOT held and `foxwizard-bluetooth-3-full.bin` from the page `esptool-js` at 0x0 and from `esptool.py`, then RESET. The same file on a blank module. The host module's `foxwizard-host-10.uf2`. Check that the module starts after the upload with no button, and that the steps in the manual match what the page shows.
- [ ] The button: a tap (let go within 1 s) adds devices (a phone connects to BLE Device and a Bluetooth keyboard connects, the light blinks blue, nothing already connected drops), a tap while adding stops. Hold with Bluetooth on: 3 red flashes from 1 s, a white fade-out from 2.5 s, Patchling's Bluetooth Radio then reads Off within a few seconds. Hold with it off: 3 green flashes from the press, a white fade-in from 1.5 s from 2.5 s, the radio reads On. Let go any time during the white: the fade still finishes and it still switches. Let go before the white: nothing changes. Keep holding: 1.5 s after the white (4 s if it was on, 3 s if it was off) a red blink with every 4th brighter, at 10 s a red fade-out, every Bluetooth device is forgotten without letting go and the radio ends On whichever it was (start it Off and On both) (the slots in Patchling go empty, USB devices stay), let go before 10 s and nothing is forgotten. With `FW_BUTTON_STYLE` set to `FW_BUTTON_CME`: hold 3 s forces peripheral, double press adds.
- [ ] Factory Reset in Patchling (main 2.6): with the Bluetooth-side USB Port set to Device, the radio Off and a BLE Host remembered, a Factory Reset leaves the Bluetooth side restarting, then Patchling shows the USB Port as Host, the radio On and every slot empty. The 10 s button hold on its own forgets the Bluetooth devices and leaves the USB Port as it was.
- [ ] Factory reset clears the slots file. Presets, loop and Save to Device still work (the slots file shares the filesystem).
- [ ] Relayed module updates (built in 2.3, the logic tested on a computer with a link that drops and damages frames, Patchling's chain tried with a fake unit, never run on hardware). Flash the host brain once with `Flash Size 2MB (Sketch: 1MB, FS: 1MB)` and the Bluetooth module with the default partition scheme, then try in this order. `./clock_check update ../mmm_web_interface/firmware/test/foxwizard-host-10.bin --target host` (the Hella's host module restarts, its USB devices come back, `clock_check version` shows the host brain's version). The same for `foxwizard-bluetooth-3.bin --target bluetooth` (the first real time a 660 KB image goes through, note how long it takes). Then Patchling from a unit with an older module (it needs a newer number than the flashed one, `build_firmware.sh --test-channel` after raising `HOST_FW_VERSION` or `BT_FW_VERSION`). Pull the USB cable mid-transfer: the module keeps working, the update is dropped after 15 s. Read the main brain's stack and heap after (`session 100`).
- [ ] Not built yet: a probe build and SWD flash mode for the host brain, flashing the Deluxe's Bluetooth module from the main brain over EN and IO0, typed names, the protocol floor in Patchling, the Deluxe PCB (ES8388, WS2812 lights, TRS 2-6 on board), AMY Synth, the Audio Looper from Loopanini with layer undo, sample transfer over SysEx and Patchling's AMY Synth row. How many Bluetooth links hold steady (NimBLE allows 3 by default, 2 hosts and BLE Device).
- [ ] Open for the Deluxe schematic: the proposed main brain pins for the host brain's SWD (GPIO6, 7, 8), and which pin the Deluxe's lights use (Chart 3 in `Foxwizard Host Edition/README.md`).
- [ ] Open: which S3 module's LED pin to trust for the Zero (`FW_S3_ZERO`, GPIO21 from Waveshare's pinout), and whether the main brain's three activity lights on the Deluxe become the one WS2812.

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
