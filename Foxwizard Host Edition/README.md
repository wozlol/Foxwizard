# FoxWizard Host Edition and Bluetooth modules

Build and protocol notes for the modules that add ports to the FoxWizard. How to use an edition is in `../MANUAL.md`.

The Host Edition is two RP2040s joined by a 2-wire serial link. The Bluetooth module (Bluetooth and Hella editions) is an ESP32-S3 on a second link, see Bluetooth module below.

- **Main brain:** the FoxWizard. Runs all routing, its native USB MIDI device port, and its DIN/TRS IN and OUT.
- **Host brain:** USB host for a 4-port hub, plus 1 DIN/TRS MIDI IN and 5 DIN/TRS MIDI OUTs. No routing. It tags everything with the port it came from and sends it to the main brain.

The only custom FoxWizard board today is the original (OG). Every other edition is the OG plus one or both of two sidecar modules: an RP2040-Zero (the USB host module, Host and Hella editions) and an ESP32-S3 SuperMini (the Bluetooth module, Bluetooth and Hella editions). The Deluxe is one new PCB with both modules' circuits on it.

## Without a host brain

The main brain firmware runs the same with or without a host brain attached, so one firmware covers both.

- GP28 has its internal pull-up on, so an unconnected link reads as idle.
- The host brain counts as present only for 1.5 s after its last port map frame, which it sends every 500 ms.
- With no host brain present, the main brain ignores link MIDI and sends none. Flow control frames still go out GP29, to nothing.
- A host brain plugged in or restarting is picked up within 500 ms.

Tested on hardware: it runs with the host brain disconnected and picks it up when reconnected.

## Folders

| Folder | Firmware |
|---|---|
| `../Mini_MIDI_Multitool` | Main brain, the one FoxWizard firmware, for units with and without a host brain. |
| `Foxwizard_Host_Edition_Sidecar` | Host brain, the RP2040 sidecar module (a Waveshare RP2040-Zero), and the same chip on the Deluxe. |
| `Foxwizard_Bluetooth_Sidecar` | Bluetooth module, on an ESP32-S3 SuperMini or Waveshare Zero. |

Each sketch folder has `board_check.h`, which stops the build with the Tools setting to change if one is wrong, and `foxwizard_link.h` and `ump.h`, identical copies kept in all three folders.

## Test builds through Patchling

`mmm_web_interface/firmware/build_firmware.sh --test-channel` builds the main brain into `mmm_web_interface/firmware/test/`. Patchling served from this computer (`serve.sh`) or opened with `?test` in its address shows a "Test builds" tag and offers those builds through its update button. The live site only ever offers the live firmware in `firmware/`. Patchling and the firmware share each version number.

The sidecar can't be updated through Patchling. Flash it from the Arduino IDE.

## Releasing

1. `mmm_web_interface/firmware/build_firmware.sh` (no options) publishes the live firmware into `mmm_web_interface/firmware/`.
2. Merge to `main` and push.
3. Copy `mmm_web_interface/` to the Patchling site (woz.lol/fox).
4. Upload the `.uf2` to woz.lol/foxwizard-midi-multitool-firmware.

## Arduino settings

| Sketch | Board and Tools settings | Library |
|---|---|---|
| Main brain | Waveshare RP2040-Zero, Flash Size 2MB (Sketch: 1MB, FS: 1MB), USB Stack Adafruit TinyUSB | Adafruit TinyUSB Library |
| Host brain | Waveshare RP2040-Zero, USB Stack Adafruit TinyUSB Host (native) | none, TinyUSB is vendored |
| Bluetooth module | ESP32S3 Dev Module, USB CDC On Boot Enabled, USB Mode Hardware CDC and JTAG, Flash Size 4MB (32Mb), PSRAM Disabled or QSPI PSRAM, Partition Scheme Default 4MB with spiffs, CPU Frequency 240MHz | NimBLE-Arduino 2.x |
| Bluetooth module on the Deluxe (N16R8) | the same, but `FW_S3_MODULE` set to `FW_S3_DELUXE` in `board_config.h`, Flash Size 16MB (128Mb), PSRAM OPI PSRAM, Partition Scheme 16M Flash (3MB APP/9.9MB FATFS) | NimBLE-Arduino 2.x |

CPU Speed stays at the RP2040 board's default, 200 MHz. A wrong setting the compiler can see stops the build with the setting to change, from `board_check.h` in each sketch folder. Flash Size on the ESP32-S3 is not one of them, so the module reads it from the chip when it starts, prints the result on its USB serial port (115200 baud) and shows a slow red light if it is wrong. `tools/build_sketches.sh` builds all three sketches with the right settings.

**Tinkerers:** the host brain must use **Tools -> USB Stack -> Adafruit TinyUSB Host (native)**. With any other USB Stack setting it won't compile, or won't act as a USB host.

The host brain uses its own vendored copy of TinyUSB in `Foxwizard_Host_Edition_Sidecar/src/`, with the RP2040 host fixes it needs, so whatever TinyUSB you have installed doesn't matter. See `VENDORED.md` there. The main brain uses the installed Adafruit TinyUSB Library.

## Flashing the host brain

How depends on which host sidecar you have.

### RP2040-Zero sidecar

Over the Zero's own USB-C port, like any Arduino upload. Unplug the OTG adapter and hub first, and hold BOOT while plugging in if the upload doesn't start.

### Deluxe host brain

On the Deluxe the RP2040's USB is wired to the hub on the board, so it can't be flashed over USB. Use SWD instead, which also flashes a blank chip.

- **Pads:** SWCLK, SWDIO, and GND. 3V3 and RUN are optional, for voltage sensing and reset.
- **Probe:** a Raspberry Pi Debug Probe, or any Pico or RP2040-Zero running the `debugprobe` firmware.
- **Arduino IDE:** Tools -> Upload Method -> Picoprobe/Debug Probe (CMSIS-DAP), then upload as normal.

## Pins

### Main brain

Same as the FoxWizard, plus:

| Pin | Use |
|---|---|
| GP29 | Link TX to host brain GP29 (PIO serial) |
| GP28 | Link RX from host brain GP28 (PIO serial) |
| GP14 | Link TX to the Bluetooth module's GPIO44 (PIO serial) |
| GP15 | Link RX from the Bluetooth module's GPIO43 (PIO serial) |

GP26 and GP27 are kept for the Bluetooth module's EN and IO0, so the main brain can flash it (planned). On the Deluxe the main brain also has its own pins for the host brain's SWD, see Chart 2.

### Host brain

| Pin | Use |
|---|---|
| GP28 | Link TX to main brain GP28 (UART0) |
| GP29 | Link RX from main brain GP29 (UART0) |
| GP4 | DIN/TRS MIDI OUT 1 (UART1 TX) |
| GP5 | DIN/TRS MIDI IN (UART1 RX) |
| GP6, GP7, GP8, GP9 | DIN/TRS MIDI OUT 2-5 (PIO serial TX) |
| GP10, GP11, GP12, GP13 | Hub port 1-4 status lights, active HIGH |
| GP14 | TRS detection, internal pull-up. Floating: the sidecar module alone, no TRS 2-6. Grounded: the Deluxe, with TRS 2-6 |
| Native USB | USB host to the hub upstream port |

Both brains share ground.

Status lights stay on while a USB MIDI device is mounted on that port and blink off briefly on traffic.

## Deluxe host circuit (planned)

The Host and Hella editions use an RP2040-Zero as the host brain and need none of this. On the Deluxe the host brain is a bare RP2040 on the board with:


- **Hub:** Microchip USB2514B, 4-port USB 2.0 hub, soldered on the board, so hub port numbers always match the same physical jacks.
- **Upstream:** RP2040 native USB D+ and D- go straight to the hub upstream port with 27 Ω series resistors at the RP2040. No connector and no CC on this link.
- **Hub support parts:** 24 MHz crystal, RBIAS resistor, and strap pins per the USB2514B datasheet reference schematic.
- **Downstream USB-C ports:** one 56 kΩ Rp resistor from each CC pin to 5 V, separate resistors on CC1 and CC2. This advertises Default USB power. Never fit 5.1 kΩ here, that is Rd, the device-side value.
- **Downstream USB-A ports:** no CC.
- **Power:** all ports share one 5 V bus, each through its own polyfuse or current-limit switch.
- **Programming:** SWD pads, no BOOT button needed. See Flashing the host brain.

## TRS 2-6 detection

The sidecar module alone (RP2040-Zero) has the USB host and nothing else. The Deluxe also has the extra TRS jacks, TRS 2-6 (they share the main brain's DIN/TRS pin layout: one shared DIN IN/TRS OUT 1 plus four more TRS OUTs). GP14 tells the firmware which:

- **Floating** (its internal pull-up, nothing wired to it): the sidecar module alone. USB Host only.
- **Grounded:** the Deluxe. USB Host plus TRS 2-6.

The sidecar reads GP14 once at power on, eight times over 8 ms after the pull-up settles, all of which must say grounded, and sends it to the main brain as bit 4 of the port map
frame (`CONTROL_PORT_MAP`, alongside the hub's mounted-device bits). The main brain reports it to
Patchling over `GET_STATUS`, which shows the TRS 2-6 switches only when the sidecar has them wired.
A board adding TRS 2-6 grounds GP14; nothing else changes in the firmware.

## Testing on an RP2040-Zero

1. Flash the host sketch (see Flashing the host brain), then unplug the computer.
2. Check continuity between the 5V pin and USB-C VBUS.
3. Power the Zero from 5 V on the 5V pin. Never connect the computer while that supply is on.
4. Plug a USB-C male to USB-A female OTG adapter into the Zero, then a USB-A hub or MIDI device into that.
5. Use a hub, or plug one device straight in. A device on the first hub keeps its jack number, and a device plugged straight in counts as 1. Hubs made of chained chips, or a hub plugged into a hub, number their jacks twice over, so those devices take a free slot, the highest first. The host brain handles up to 4 hubs and 13 devices, with 64 USB endpoint slots between them (a device takes one for itself, and two for each MIDI interface), and has 7 USB Host slots, so up to seven devices work at once whichever jacks they're in. An eighth waits, and takes the next slot to free up. A device's other MIDI interfaces aren't separate ports.

Patchling shows only the USB Host ports there's room for: one with no hub, otherwise the hubs' jacks up to 4. A port shows once a device is plugged into the one before it, and stays until Patchling reconnects.

The Zero's onboard RGB LED (GP16) is a diagnostic light, off by default, the same no-blink ethic as
the main brain's own activity lights, with one deliberate exception for a dead link:

| Light | Meaning |
|---|---|
| Blue, fading | Any MIDI activity through the sidecar (a hub USB port or a DIN/TRS port, either direction). Fades up over 1 s and back down over 1/2 s, follows the main brain's brightness knob. |
| White, fading | The same fade, for a USB device (MIDI or not) just being plugged in. |
| Purple, instant | Two flashes, right after a restart a USB fault caused. See Restart reports below. |
| Purple, blinking fast | Nothing heard from the main brain for over 1 second: check the link wires, the ground wire, and that the main brain runs the Host Edition firmware. |

USB-A devices and hubs have no CC, so the Zero's own 5.1 kΩ CC resistors don't matter. A USB-C device on a USB-C to USB-C cable will not power up from the Zero.

## Link protocol

UART, 1,000,000 baud, 8N1. The link carries UMP, the MIDI 2.0 packet. One frame carries one 32-bit UMP word plus a port number. A message is 1 to 4 words, and the Message Type in the top 4 bits of its first word gives its length, so a message type added to the MIDI 2.0 spec later passes through the host brain unchanged.

| Byte | Contents |
|---|---|
| 0 | bit 7 = 1, bits 6-3 = port, bit 2 = first word of a message, bits 1-0 = 0 |
| 1 | bit 7 = 0, bits 3-0 = bits 31-28 of the word |
| 2, 3, 4, 5 | bit 7 = 0, the word's next 7 bits each: 27-21, 20-14, 13-7, 6-0 |

Only byte 0 has bit 7 set, so after any glitch the receiver resyncs on the next frame. A real time message (clock, start, stop, continue) can come between the words of another message, because it skips the queues. The receiver takes it out whole and carries on with the message in progress. A first-word frame that arrives mid-message drops the unfinished message. At 1,000,000 baud a frame takes 60 µs.

Each brain turns MIDI at its own edge. The host brain converts the USB MIDI 1.0 packets of its devices and DIN IN into UMP, and UMP back into USB MIDI 1.0 packets for devices and DIN OUTs, translating a MIDI 2.0 message down for those. The main brain does the same for the link, so everything inside it stays MIDI 1.0 until it is changed on purpose. `ump.h` has the conversion, with MIDI 1.0 messages carried exactly and MIDI 2.0 messages translated by the spec's scaling.

| Port | Host to main | Main to host |
|---|---|---|
| 0 | | Every USB device and every DIN OUT (the main brain doesn't use it) |
| 1-4 | From USB Host slot 1-4 | USB Host slot 1-4 only |
| 5 | From host DIN IN | DIN OUT 1 only |
| 6-9 | | DIN OUT 2-5 only |
| 10-12 | From USB Host slot 5-7 (host firmware 6) | USB Host slot 5-7 only |
| 13-14 | | Reserved. The Bluetooth module has its own link |
| 15 | Control | Control |

Port 15 carries control frames. The word's four bytes are the frame type (0-15) then three data bytes, all 8 bits:

| Type | Direction | Contents |
|---|---|---|
| 1, port map | Host to main | Byte 1 = mounted slots (bits 0-3 = USB Host slot 1-4, bit 4 = TRS 2-6 present, 1.44, bits 5-7 = USB Host slot 5-7, host firmware 6), byte 2 = host firmware version, byte 3 = drop count (max 127). Sent on any plug or unplug and every 500 ms. |
| 2, flow | Both ways | Byte 1 = 1 pause, 0 resume. Sent on change and every 100 ms. |
| 3, host restart | Host to main | Byte 1 = last restart reason, byte 2 = hub port (for a wedge), byte 3 = restart count. Sent with the port map. |
| 6, host hub | Host to main | Byte 1 = downstream jacks of the hubs attached (0 no hub), byte 2 = hubs attached, byte 3 = devices waiting for a free slot. Sent with the port map. Host firmware 4. |
| 5, host report | Host to main | Byte 1 = index, bytes 2-3 = report bytes 2 x index and 2 x index + 1. The 240-byte host report, sent six times with the heartbeat after a recovery restart. Host firmware 3. |
| 4, brightness | Main to host | Byte 1 = the LED brightness knob, 0-10. Sent whenever it changes, and once a sidecar (re)appears. |
| 7, slot reserve | Both ways | Main to host: byte 1 = the USB Host slots held for a Bluetooth device, slot n is bit n-1, sent every 500 ms and on a change. Host to main: the ones it is keeping free (held and not in use by a USB device). Host firmware 9. |
| 8, device name | Host to main | Byte 1 = chunk << 4 or slot (1-7), bytes 2-3 = two characters of the name, 0 ends it. The whole name, 12 chunks, when the product string has been read and every 5 s after. Host firmware 9. |

`foxwizard_link.h` holds the frame code and queue rules. Both sketch folders carry an identical copy.

`ump.h` holds the MIDI 2.0 packet code: UMP message lengths, the conversion to and from USB MIDI 1.0 packets, and the MIDI 1.0 and MIDI 2.0 Protocol translation. Both sketch folders carry an identical copy, and `Mini_MIDI_Multitool/tests/ump_test.cpp` checks it on a computer.

## Buffering, flow control, and drops

Dropping is the last resort. Each step below is tried first.

1. **Buffer.** Every path has a queue.

   | Queue | Ordinary | Realtime |
   |---|---|---|
   | Main brain receive buffer | 8 KB | shared |
   | Main brain link out | 1024 frames | 64 frames |
   | Host brain UART receive buffer | 4 KB | shared |
   | Host brain frames from the main brain, waiting for outputs | 1024 frames | sent straight to outputs |
   | Host brain link out | 2048 frames | 64 frames |
   | Each hub port | 256 packets | 32 packets |
   | Each DIN OUT | 256 messages | 32 bytes |

2. **Hold back the source.** The host brain only reads a USB device while its link queue has room for a full USB packet (16 messages). Otherwise the data stays in the device, which waits.
3. **Wait for slow outputs.** A frame from the main brain waits until every output it goes to has room. When that waiting queue is full, the host brain stops reading the link and new bytes wait in its 4 KB UART buffer. Nothing is dropped because a DIN OUT is slow.
4. **Pause the sender.** Each brain tells the other to pause when its incoming backlog gets deep, and to resume once it drains. A paused brain keeps its ordinary frames queued.
   - Main brain: pauses the host above 2048 waiting bytes in its 8 KB receive buffer, resumes below 1024.
   - Host brain: pauses the main brain above 512 waiting frames, counting bytes still in its UART buffer, resumes below 256.
   - Flow state is resent every 100 ms. A pause not repeated for 500 ms counts as resume, so a crashed brain never stalls the other.
5. **Protect stuck-note messages.** Every queue keeps its last slots for Note Off, Note On velocity 0, sustain release (CC 64 below 64), All Sound Off (CC 120), and All Notes Off (CC 123). Same rule as ARPnMIDI's secondary link.
6. **Drop, and count it.** Only after all of the above. Ordinary messages go before protected ones.

Where a drop can still happen:

- **Host DIN IN while the main brain holds a pause for a long time.** DIN can't be held back.
- **Main brain link out while the host brain holds a pause for a long time.** Main brain output to the link is paced by its own DIN OUT, so this only happens if the host brain is stuck.
- **A USB device that accepts nothing for 1 second.** It is skipped so it can't hold up the other outputs. It rejoins as soon as it accepts data again.
- **A realtime queue that fills.** Realtime queues have no reserve. Clock traffic is far too light to fill them in normal use.
- **The main brain loop blocked for over 60 ms while the host sends at full link speed.** The 6 KB above the pause point fills. Real MIDI traffic is far below full link speed.

The host brain reports its total drop count in its port map frame. The main brain stores it as `hostDropCount`. Every queue also keeps a high-water mark for testing.

## Timing

- Realtime messages (clock, start, stop, continue) and control frames skip every ordinary queue on both brains, and still flow while a brain is paused. A clock tick waits at most one frame, 60 µs.
- On DIN OUTs, realtime bytes go out between the bytes of other messages, as MIDI allows.
- USB adds up to 1 ms of wobble, the same as any USB MIDI device on a computer.
- The main brain parses at most 256 link bytes per loop and only writes to the link while the PIO FIFO has room. It never waits on the host brain.

## Ports and routing

| Port | Input | Output | Where |
|---|---|---|---|
| TRS/DIN | yes | yes | Main brain |
| USB Device | yes | yes | Main brain, to a computer |
| USB Host 1-7 | yes | yes | Sidecar, one per slot (7 from host firmware 6, 4 before) |
| TRS 2 | yes | yes | Sidecar DIN IN and DIN OUT 1 |
| TRS 3-6 | | yes | Sidecar DIN OUT 2-5 |

### Port maximums

Everything fitted: the Deluxe.

| | Count | Which |
|---|---|---|
| Inputs | 10 | TRS/DIN, USB Device, USB Host 1-7, TRS 2 |
| Outputs | 14 | TRS/DIN, USB Device, USB Host 1-7, TRS 2-6 |
| DIN/TRS outs | 6 | TRS/DIN on the main brain, TRS 2-6 on the sidecar |
| DIN/TRS ins | 2 | TRS/DIN on the main brain, TRS 2 on the sidecar |
| USB Host devices at once | 7 | 4 on the board's own jacks, up to 3 more through a hub plugged into one of them |

- Every port has its own switch in Patchling, as an input and as an output, and a route picks any combination of each. A route's ports are 16 bits. Bits 14 and 15 are the Bluetooth module's, see Shared slots.
- The ports do not trade against each other. Each has its own fixed bit, so fewer USB Host devices does not free a bit for more DIN outs, and 7 USB Host with 6 DIN/TRS outs works at once. 5 sidecar DIN outs is the limit, there is no bit for a sixth.
- Grounding GP14 only shows TRS 2-6 in Patchling and sends nothing else. It takes no USB Host slot, the pins are separate (DIN/TRS GP4-GP9, hub lights GP10-GP13, USB Host is the native USB port).
- Fewer ports are not a gain, except that a Bluetooth device can take an empty USB Host slot 5-7, see Shared slots. That does not change a saved route.

Sidecar ports exist only while a sidecar is present. Without routes:

- **USB Adaptor Mode:** every input goes to every other port, never back to the port it came in on. A device on USB Host 1 reaches USB Host 2-4 but never hears itself.
- **Router Mode:** every input goes to every output, its own port included.

Routes take over from the mode for whatever they match. Each route picks any combination of input ports and output ports. In Patchling the USB Host and TRS 2-6 switches appear once a sidecar is detected, the badge adds "· Host", and a green dot marks USB Host ports with a device plugged in. Patchling never says "sidecar" to customers, who buy a FoxWizard or a FoxWizard Host Edition.

- SysEx replies to FoxWizard commands from the sidecar go back out the port they came in on.
- USB devices with no MIDI OUT (input-only controllers) are never sent anything.

## Launchpad

For old octave-per-row Launchpads (Mini mk1 and mk2). A route with the Launchpad switch on remaps the grid to a drum pad layout and learns its top and side buttons. Give the route only the Launchpad's port as input, on channel 1. Another controller on channel 1 then needs its own route on its own port.

- **Learning the port:** the first grid or button press on a Launchpad route makes that port a Launchpad port, owned by that route. A save or route change forgets it, and the next press learns it again.
- **Lights, not notes:** no note reaches a Launchpad port as it is. Every note headed there, from any route and channel (arp, Bypass, Bass, drums, echoes, stutter, looper playback), lights each pad that plays it on the owning route, through the grid, Scale, Auto Chord and transpose. A note no pad plays lights nothing. A route's output switch for that port doesn't matter.
- **-1 Oct:** a switch in the Launchpad section that moves everything the Launchpad plays down an octave, whatever the Scale and Layout.
- **Colors by channel:** yellow, orange, green, red, then round again. Channel 10 is always red.

| Channel | Color | Velocity |
|---|---|---|
| 1, 5, 9, 13 | Yellow | 62 |
| 2, 6, 14 | Orange | 47 |
| 3, 7, 11, 15 | Green | 60 |
| 4, 8, 10, 12, 16 | Red | 15 |

- **Presses:** each press echoes back at once in its channel's color.
- **Round buttons:** the top row sends CC 104-111 and the side column sends notes. A button with an action in the Launchpad section runs it. A top button left Off passes its CC on to the route's other features, so a Stutter, Echo or arp square or CC to Feature can use it. Side buttons work as division square triggers either way.
- **Two Launchpads:** each needs its own Launchpad route, listening only on its own port.

## Output queues

- **TRS/DIN:** 2 KB, so the loop never waits on the 31,250-baud UART. Timed features depend on that.
- **USB Device:** 512 packets. TinyUSB's own USB MIDI buffer holds only 16 packets and drops the rest, which cut long SysEx replies (a 53-byte route is 23 packets) and bursts like a note cloned to 16 channels. Firmware 1.10.
- Each has a realtime lane that goes first.
- While either is nearly full (under 512 bytes or 128 packets free), the main brain stops reading its inputs: the computer and the host brain wait, and TRS/DIN input waits in a 256-byte receive buffer. Nothing is dropped, SysEx dumps included.

## Recovery

- **Both brains:** a hardware watchdog reboots the brain if its loop stops running, 4 s on the main brain and 2 s on the sidecar. That covers a hang anywhere, a USB hot-plug hang included.
- **Sidecar:** if queued output to a USB device hasn't moved for 3 s, the USB host has wedged, and the sidecar reboots. Every device re-enumerates in about a second.
- **USB driver faults:** stock TinyUSB's RP2040 host driver halts the chip with `panic()` on a USB data sequence error, a buffer armed twice, or a request to clear a device's stalled endpoint. The sidecar's vendored copy (`Foxwizard_Host_Edition_Sidecar/src/Adafruit_TinyUSB/VENDORED.md`) restarts at once instead, with the fault as the reason, rather than halting until the watchdog fires. Recovering in place without a restart was tried and caused more faults, so it was taken back out.
- **Restart reports:** after a recovery restart the sidecar's diagnostic light flashes purple twice. Patchling shows the reason in a red message that stays until clicked. Clicking the badge opens the full host report, with a Copy button.
- **Host report (host firmware 3):** a black box in RAM the startup code doesn't clear, so it survives any restart but a power cycle. It holds each port's device VID:PID, packets taken and sent, the last packet taken, the last 16 events, and which part of the loop was running. A recovery restart also notes each port's waiting packets and the one at the front.
- **Buffer armed twice:** the only fault seen so far, and only right after plugging USB devices in or out, never while playing. Likely a race in TinyUSB's RP2040 hot-plug cleanup. It recovers in about a second, so Patchling shows it as a brief yellow "reset its USB ports after a device change" instead.
- **Stuck notes:** when the sidecar restarts or disappears, the main brain sends Note Offs for every note still held from USB Host and TRS 2, through the same routes as a real Note Off. When the sidecar restarts or comes back, the main brain also sends All Notes Off (CC 123) on all channels to the sidecar's ports only, for notes those outputs missed while it was gone. The main brain's own ports are left alone.
- **Main brain:** handles at most 64 USB Device packets and 64 TRS/DIN bytes per loop, so a flood such as a DAW feedback loop can't starve the link, the lights, or the button.

## Bluetooth module

An ESP32-S3 SuperMini or Waveshare ESP32-S3-Zero on a second link to the main brain. It does no routing, like the host brain. Firmware: `Foxwizard_Bluetooth_Sidecar`.

- **BLE Device:** the FoxWizard as a Bluetooth MIDI device to a phone or computer. Always advertising, link port 9, route port bit 14.
- **Handles:** a device on the module, Bluetooth or USB, has a handle 1-4 (`MODULE_HANDLES` in `board_config.h`), link port = handle. The main brain tracks 4. Of those, `BLE_HOSTS_MAX` is 2 Bluetooth hosts, because NimBLE-Arduino allows 3 Bluetooth connections in all by default and BLE Device is one.
- **Link:** the host link's frames and UMP, 1,000,000 baud, on UART1 over the module's GPIO44 (RX) and GPIO43 (TX), the pads the chip's own bootloader listens on. Main brain GP14 (TX) and GP15 (RX).
- **Light:** the module's own WS2812, `FW_S3_MODULE` in `board_config.h` picks the SuperMini (GPIO48) or the Zero (GPIO21).
- **Timing:** Bluetooth adds up to one connection interval, 7.5 to 15 ms, to a message.
- **Bench mode:** `FW_BENCH` set to 1 runs the module alone for a first test, no main brain. The USB port stays serial, it connects up to `BLE_HOSTS_MAX` Bluetooth hosts while adding, and prints what it hears.

### USB port

The module's own USB port hosts USB MIDI devices, best effort. `usb_midi_host.h` is a client of ESP-IDF's USB Host Library, which the Arduino core ships with hub support on, multi-level too, so a hub works as far as the chip's 8 host channels go. Each endpoint of each device takes a channel and a hub takes some, so plan on 2 or 3 MIDI devices through a hub, one device straight in always works.

- **Which devices:** a MIDI Streaming interface (class 1, subclass 3) with bulk endpoints. A hub, or anything else, is closed again. Names come from the device's product string, the id is its vendor and product id and a hash of its serial number, sent as the handle's address (address frame part 3 says it is USB).
- **Slots:** a USB device takes a handle and a slot like a Bluetooth one. With no room it waits, plugged in and unused, and starts when the main brain says there is a slot. It is never taken out of a slot for another. A refused device is left alone until it is unplugged.
- **Power:** the S3 module gives a connected device no 5 V, the SuperMini and the Zero do not switch VBUS. Use a powered hub or an adapter that powers it. The Deluxe board supplies it.
- **Host or device:** the port shares the chip's USB serial and JTAG function, so as a host the module is not a serial port on a computer. The setting is saved in the module's flash (`usbrole`), host by default, changed by a link command (Patchling's USB Port in the Bluetooth row, `clock_check bleaction usb host|device`), and the module restarts to apply it. The Device role is the module's USB serial port, the way it is updated and watched, and a place for a second USB device port, a USB MIDI device, in a later build (it needs the USB-OTG (TinyUSB) Tools setting, so it is not this build).
- **Fall back:** each time the host starts the module counts it in `usbtry`, and clears the count after 5 seconds of running. If the count is still set at the next start and the last reset was a crash or a watchdog, or three starts in a row never ran 5 seconds, the role goes to Device, the light blinks orange for 30 seconds and Patchling says so. A power cut alone does not move it. The module can always be reached and flashed.
- **Build:** `FW_USB_HOST` 0 in `board_config.h` leaves the host out. Bench mode never starts it, so the serial monitor works.
- **Not run on hardware yet.** Written from the IDF 5.5 headers in the core and compiled, nothing more.

### Button

The module's BOOT button, GPIO0, pressed while it runs (holding it at power-on is the chip's bootloader). `FW_BUTTON_STYLE` in `board_config.h`:

| Gesture | `FW_BUTTON_ONE_MODE` (default) | `FW_BUTTON_CME` |
|---|---|---|
| Power on | remembered devices reconnect on their own | same |
| Hold 3 s, let go | add devices for a minute, both roles: BLE Device keeps advertising for a phone or computer and the module scans and connects compatible Bluetooth MIDI devices up to the room there is, remembering each | force peripheral: drops every Bluetooth link and advertises for a minute so a phone or computer can connect, then the remembered devices reconnect |
| Double press | nothing | learn: connect compatible Bluetooth MIDI devices up to the room there is, remember each |
| Short press | stop adding | stop learning |
| Hold 10 s, let go | forget every Bluetooth device and bond, USB names are kept | same |

The one-mode layout needs no choice of host or device, the other equipment decides the role. CME's layout is kept as a switch in case it is ever needed, it is CME's way of managing connections, not something Bluetooth requires. USB devices are never dropped by a gesture.

### Light

Off by default, same no-blink ethic as the other lights. Blue fades up on Bluetooth MIDI (1 s up, 0.5 s down, follows the brightness knob). Green for 350 ms on each new connection. Blue blinking: adding devices. Purple pulsing slowly: force peripheral, CME style only. Purple blinking fast: nothing heard from the main brain for a second. Orange blinking, 30 seconds after start: the USB host fell back. Red blinking slowly: the Tools settings are wrong. White at 3 s held, red at 10 s held.

### Control frames on the Bluetooth link

Port 15, the same frame as the host link. Types 1 (port map: byte 1 = connected handles, bit h-1, byte 2 = module firmware, byte 3 = drops), 2 (flow) and 4 (brightness) are as for the host brain.

| Type | Direction | Contents |
|---|---|---|
| 8, device name | Module to main | Byte 1 = chunk << 4 or handle, bytes 2-3 = two characters, as for the host brain. |
| 9, status | Module to main | Byte 1 = bit 0 BLE Device connected, 1 adding devices, 2 scanning, 3 force peripheral window, 4 the USB port is a host port, 5 the USB host fell back. Byte 2 = devices connected on handles. Byte 3 = board check, 0 fine, 1 Flash Size wrong. Sent with the port map. |
| 10, address | Module to main | Byte 1 = part << 4 or handle, bytes 2-3 = two bytes of the device's address, parts 0-2. A USB device's "address" is vendor id, product id and a serial hash. Part 3: byte 2 = 1 USB device, 2 Bluetooth device. Sent with the name when a device connects and every 5 s. |
| 11, known | Main to module | Byte 1 = part << 4 or slot, bytes 2-3 = two address bytes, parts 0-2. The devices to reconnect to, all zero for none. Sent every 5 s and on a change. |
| 12, command | Main to module | Byte 1 = 1 add devices (value 1 or 0), 2 allow (devices it may have connected, 0 to 8, sent every second), 3 disconnect (value = handle, a USB device is let go until unplugged), 4 forget (value = handle), 5 USB port (0 host, 1 device, saved, the module restarts). |
| 13, event | Module to main | Byte 1 = 1: the button was held 10 s and every Bluetooth device is forgotten. Sent 3 times. |

## Shared slots

USB Host, BLE Host and the USB devices on the module's own USB port share host slots. Slots 1-7 are the USB Host ports. Slot 8 is route bit 15, only ever a device on the Bluetooth module, and slot 9 is BLE Device, bit 14. A module device (Bluetooth or USB) may take slot 8, then USB Host slots 5, 6 and 7 when no USB device is in them. Slots 1-4 are the hub's jacks and never go to a module device, so a hub with 4 devices works as before and a module device takes what a chained hub would use.

- **Saved routes do not change.** A route's port masks are 16 bits and 14 and 15 are 0 in routes saved before. Only what a bit means at run time changes, and that is in `/slots.fw`.
- **`/slots.fw`** is a 291 byte file in the filesystem, not the 4 KB settings image (4,024 of 4,096 bytes used). One record per slot: kind (USB on the host module, Bluetooth, or USB on the Bluetooth module), the device's address or id, its name (24 characters, printable ASCII) and a last-seen count. The count goes up on each connection, there is no clock. It is written 2 s after a change, when nothing has been played for a moment, and removed by Factory Reset.
- **Rules** (`src/ble_slots.h`, `tests/ble_slots_test.cpp`): a device that comes back gets its own slot if it is free. A new device takes a slot nothing has used, else the slot whose device has been gone longest. A connected device's slot is never taken. Forget in Patchling clears a record at once. The 10 second button hold clears every Bluetooth record.
- **Taking a USB Host slot:** the main brain sends the slot in a slot reserve frame, the host brain stops giving it to USB devices and echoes it back, and only then does the Bluetooth device get it. No echo in 1.5 s, or a USB device got there first, and the device is sent away. A Bluetooth device holding slot 5-7 is announced to the host brain every 500 ms, so a host brain that restarted learns it. With no host brain, slots 5-7 need no agreement. A host brain older than firmware 9 cannot hold a slot, so a Bluetooth device only gets slot 8.
- **Full:** the main brain tells the module how many Bluetooth devices it may have connected, connected plus the slots it could still give. At 0 the module connects nothing and learning does nothing, and a device that still connects is sent away. Patchling shows why and says to unplug a USB device in USB Host 5 to 7 first, or to forget a Bluetooth device. No device is ever removed to make room.
- **Names:** USB devices' product strings are read by the host brain once a device has settled and sent as device name frames (without the string it sends `USB vid:pid`), and Bluetooth devices' names come from the module. Patchling shows the name on the port's switch, remembered when the device is gone.
- **GET_SLOTS and BLE_ACTION** (`SYSEX_PROTOCOL.md`) are how Patchling reads and changes this. `tools/clock_check slots` shows it from a terminal.
- **Memory:** the Bluetooth support adds 4.4 KB of RAM to the main brain, heap with the filesystem and Patchling in use is the number to watch (`DIAGNOSTICS.md`).

## Wiring and pin numbers

Two charts. The first is for wiring the boards you have, by what is printed on them. The second is for drawing the Deluxe schematic, by the chips' pin numbers from their datasheets.

### Chart 1: the Hella, wires between the boards by the labels printed on them

The FoxWizard, an RP2040-Zero as the USB host module and an ESP32-S3 SuperMini as the Bluetooth module. One row per wire, each wire goes between the pads in its row. Ground and power are not listed.

| Wire | FoxWizard (main brain) | S3 SuperMini (Bluetooth module) | RP2040-Zero (USB host module) |
|---|---|---|---|
| Bluetooth link, out of the FoxWizard | GP14 | RX | |
| Bluetooth link, into the FoxWizard | GP15 | TX | |
| USB host link, out of the FoxWizard | GP29 | | GP29 |
| USB host link, into the FoxWizard | GP28 | | GP28 |
| S3 reset, optional | GP26 | RESET button's pad (EN) | |
| S3 boot mode, optional | GP27 | BOOT button's pad (GPIO0) | |

Both modules are updated from Patchling through the FoxWizard over the four link wires once the relay is built. The last two rows are only for rescuing a Bluetooth module whose firmware is dead or blank, by the chip's own bootloader. The RP2040-Zero is rescued with its own BOOT button and USB-C, it has no debug pads. The main brain is always reachable over its USB-C and firmware pads.

TX and RX cross, a wire from one board's output goes to the other board's input. If the S3 board prints 43 and 44 or IO43 and IO44 instead of TX and RX, RX is 44 and TX is 43. The SuperMini's pinout pages list pads for many GPIOs but I could not confirm pads for GPIO0 and EN, only the BOOT and RESET buttons, so for the last two rows check your board with a meter and, if there are no pads, solder to the buttons' pads on the terminal that is not ground.

There is no separate Host Edition board. The Host Edition is the OG with an RP2040-Zero, the Hella adds an S3 SuperMini, and the Deluxe is the one new PCB with both modules' circuits.

### Chart 2: the Deluxe PCB, by chip pin numbers

The main brain and the host brain are bare RP2040 chips (QFN-56 pin numbers from the RP2040 datasheet) and the Bluetooth module is an ESP32-S3-WROOM-1 module (pad numbers from its datasheet, Table 3-1). One row per net, each net joins the pins in its row. Power, ground and the USB ports are not listed.

| Net | Main RP2040 | S3 module (ESP32-S3-WROOM-1) | Host RP2040 |
|---|---|---|---|
| Bluetooth link, out of the main brain | GPIO14, pin 17 | RXD0 (GPIO44), pad 36 | |
| Bluetooth link, into the main brain | GPIO15, pin 18 | TXD0 (GPIO43), pad 37 | |
| USB host link, out of the main brain | GPIO29, pin 41 | | GPIO29, pin 41 |
| USB host link, into the main brain | GPIO28, pin 40 | | GPIO28, pin 40 |
| S3 reset, flashing and rescue | GPIO26, pin 38 | EN, pad 3 | |
| S3 boot mode, flashing and rescue | GPIO27, pin 39 | IO0, pad 27 | |
| Host brain SWD clock, the main brain as its own probe | GPIO6, pin 8 (proposed) | | SWCLK, pin 24 |
| Host brain SWD data | GPIO7, pin 9 (proposed) | | SWD, pin 25 |
| Host brain reset | GPIO8, pin 11 (proposed) | | RUN, pin 26 |
| TRS 2-6 are on the board | | | GPIO14, pin 17, tied to ground |

The three SWD nets and the S3's reset, boot and serial nets are the main brain's way to flash and update the host brain and the S3 with no probe, the first flash included. The same nets also go to small backup headers (host brain: SWCLK, SWD, RUN and ground, S3: TXD0, RXD0, EN, IO0 and ground), for a debug probe or a USB-serial adapter if the main brain is ever dead. The main brain's pins sit idle, as inputs, while a probe is on a header. The S3 also has BOOT and RESET buttons on IO0 and EN. SWCLK and SWD are a consecutive pair of main brain pins because the probe code drives SWD from one PIO program, any free consecutive pair will do.

### Chart 3: the other pins on the Deluxe's two RP2040s, by chip pin numbers

Not wires between chips, listed so nothing is lost when the schematic is drawn.

| Chip | Pin | Goes to |
|---|---|---|
| Main RP2040 | GPIO0, pin 2 | DIN/TRS MIDI out |
| Main RP2040 | GPIO1, pin 3 | DIN/TRS MIDI in |
| Main RP2040 | GPIO5, pin 7 | the front button |
| Main RP2040 | GPIO2, 3 and 4, pins 4, 5 and 6 | the OUT, USB and IN lights (the Deluxe's lights are WS2812, which pin is open) |
| Host RP2040 | GPIO4, pin 6 | TRS/DIN out 1 (UART1 TX) |
| Host RP2040 | GPIO5, pin 7 | TRS/DIN in (UART1 RX) |
| Host RP2040 | GPIO6, 7, 8 and 9, pins 8, 9, 11 and 12 | TRS outs 2 to 5 |
| Host RP2040 | GPIO10, 11, 12 and 13, pins 13, 14, 15 and 16 | the four hub port lights |
| Host RP2040 | GPIO16, pin 27 | its WS2812 status light |

### The Deluxe's Bluetooth module

ESP32-S3-WROOM-1-N8R8, the version with the PCB antenna: 8 MB flash and 8 MB PSRAM, 18.0 x 25.5 x 3.1 mm. The PSRAM is what the audio looper, the sampler and AMY need. JLCPCB lists it as C2913201, about $3.40 to $5.00 each depending on the quantity, with tens of thousands in stock when checked (check the live page before ordering). The N16R8 (C2913202, about $3.50 to $5.30) has the same pinout with 16 MB of flash, for more samples. Both are rated to 65 degrees C ambient, and the 8 MB PSRAM uses pads 28 to 30 (IO35 to IO37), which are not available.

## Flashing the modules

- **Bluetooth module, first time and after:** its own USB-C, with the Tools settings above. The first upload may need BOOT held while plugging in.
- **Host brain on the Deluxe:** SWD pads, see above. On the Hella and the Host Edition it is an RP2040-Zero, flashed over its own USB-C with BOOT held.
- **Planned, updating the modules from Patchling, on the Hella as well as the Deluxe:** the main brain relays the same chunked update it already takes (begin, data, end, with a checksum and the whole-file MD5) to a chosen module over its link, and the module installs it itself. Nothing in it needs the Deluxe's extra pins. It needs: the host brain's sketch built with a filesystem to stage the image, as the main brain is (`Flash Size 2MB (Sketch: 1MB, FS: 1MB)`, the sketch is 85 KB), the S3's second app slot (the default 4 MB layout has two), a target byte in the update messages, each module reporting its firmware version so Patchling can show an update badge per module, and the module binaries published next to the main brain's. Until it is built a module is updated by hand: the host brain over its own USB-C and BOOT button (RP2040-Zero, Hella and Host Edition) or SWD pads (the Deluxe), the Bluetooth module over its USB-C.
- **Planned, a probe build on the main brain:** for the first flash of the Deluxe's host brain, Raspberry Pi's `debugprobe` firmware with `PROBE_PIN_OFFSET` 6, so SWCLK is GPIO6 and SWDIO is GPIO7 (the pins in Chart 2), the reset line on GPIO8, and its stock reset, UART and light pins moved off GP1, GP4 and GP5 (DIN in, IN light, button). Load it over USB with the firmware pads shorted, flash the host brain over SWD from a computer with OpenOCD, short the pads again and load the main firmware. The Hella and the Host Edition use an RP2040-Zero as the host brain, which has no debug pads, so none of this applies to them.
- **Planned, an SWD flash mode in the main firmware:** routing paused with a notice, a free PIO state machine borrowed or plain GPIO, the watchdog fed, a restart at the end. Only the SWD engine and the flashing steps, no USB probe side. A failed flash is run again, the host brain cannot be bricked.
- **Planned, the Bluetooth module from the main brain:** Espressif's `esp-serial-flasher` (it has a Pico host port and ESP32-S3 target), over GP14 and GP15 with GP26 on the module's EN and GP27 on IO0. Patchling streams the image over SysEx, nothing is stored on the main brain.

The exposed pads on the main brain are GP14, GP15, GP26, GP27, GP28 and GP29. GP28 and GP29 are the host brain link, the same pad numbers as on the host module, and GP14, GP15, GP26 and GP27 are used by the above. Firmware 2.2 moved the host link here from GP26 and GP27 (it was GP26 to host GP29 and GP27 from host GP28), a unit wired the old way needs its two link wires moved.

## Editions

| Edition | Main brain | USB host module | Bluetooth module |
|---|---|---|---|
| FoxWizard Jr | RP2040, no TRS-B hardware | | |
| FoxWizard | RP2040 | | |
| Host Edition | RP2040 | RP2040-Zero | |
| Bluetooth Edition | RP2040 | | ESP32-S3 SuperMini |
| Hella Edition | RP2040 | RP2040-Zero | ESP32-S3 SuperMini |
| Deluxe | RP2040 | RP2040 on the board with its hub, SWD pads and TRS 2-6 | ESP32-S3-WROOM-1-N16R8 on the board, with an ES8388 codec |

One main brain firmware covers every edition, it finds each module by its link.

## Planned

- **Deluxe:** one PCB with the main brain, the USB host module with its hub and TRS 2-6, the ESP32-S3 and an ES8388 codec on it. WS2812 lights on the same pins the modules' own lights use (GP16 on the RP2040-Zero), no discrete lights. The S3's USB port is on the board too, a USB-C jack with 5 V for the devices, a host port by default and switchable in software to a device port, the same as on the Bluetooth module. The device role becomes a USB MIDI device (a second port to a computer) with a TinyUSB build, a service port for updating the module meanwhile.
- **AMY Synth and Audio Looper (Deluxe):** AMY runs on the S3 through the codec and takes MIDI as one more port. The audio looper comes from the Loopanini project with layer undo, not its four tracks. Very small audio files (drum hits and simple sampled instruments) go from Patchling to the S3's storage as SysEx through the main brain and the module's link, in checksummed chunks like the firmware update. Patchling's AMY Synth row only shows when the unit reports an S3 with the codec.
- **Typed names:** a name typed in Patchling for any port, kept in `/slots.fw`.
- **Protocol floor:** below it, Patchling shows the update badge and does not edit until the unit is updated. The firmware keeps no old protocols.
- **The module's USB host,** its test on hardware: how many MIDI devices stay steady through a hub, power, and that the fall back works.

## Limits

- USB full speed (12 Mbps). Devices that only work at high speed (480 Mbps) won't work.
- MIDI 2.0 devices run in their MIDI 1.0 mode. Native MIDI 2.0 host support would be new firmware, not new hardware.
- USB cables (virtual ports) 0-7 pass through the link.
