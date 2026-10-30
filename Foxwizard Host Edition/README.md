# FoxWizard Host Edition

Two RP2040s joined by a 2-wire serial link.

- **Main brain:** the FoxWizard. Runs all routing, its native USB MIDI device port, and its DIN/TRS IN and OUT.
- **Host brain:** USB host for a 4-port hub, plus 1 DIN/TRS MIDI IN and 5 DIN/TRS MIDI OUTs. No routing. It tags everything with the port it came from and sends it to the main brain.

The host brain runs on a Waveshare RP2040-Zero module or on the Host Edition board.

## Without a host brain

The main brain firmware runs the same with or without a host brain attached, so one firmware covers both.

- GP27 has its internal pull-up on, so an unconnected link reads as idle.
- The host brain counts as present only for 1.5 s after its last port map frame, which it sends every 500 ms.
- With no host brain present, the main brain ignores link MIDI and sends none. Flow control frames still go out GP26, to nothing.
- A host brain plugged in or restarting is picked up within 500 ms.

Tested on hardware: it runs with the host brain disconnected and picks it up when reconnected.

## Folders

| Folder | Firmware |
|---|---|
| `../Mini_MIDI_Multitool` | Main brain, the one FoxWizard firmware, for units with and without a host brain. |
| `Foxwizard_Host_Edition_Sidecar` | Host brain, on the RP2040-Zero or the Host Edition sidecar board. |

## Test builds through Patchling

`mmm_web_interface/firmware/build_firmware.sh --test-channel` builds the main brain into `mmm_web_interface/firmware/test/`. Patchling served from this computer (`serve.sh`) or opened with `?test` in its address shows a "Test builds" tag and offers those builds through its update button. The live site only ever offers the live firmware in `firmware/`. Patchling and the firmware share each version number.

The sidecar can't be updated through Patchling. Flash it from the Arduino IDE.

## Releasing

1. `mmm_web_interface/firmware/build_firmware.sh` (no options) publishes the live firmware into `mmm_web_interface/firmware/`.
2. Merge to `main` and push.
3. Copy `mmm_web_interface/` to the Patchling site (woz.lol/fox).
4. Upload the `.uf2` to woz.lol/foxwizard-midi-multitool-firmware.

## Arduino settings

| Sketch | Board | Tools -> USB Stack |
|---|---|---|
| Main brain | Waveshare RP2040-Zero, Flash Size 2MB (Sketch: 1MB, FS: 1MB) | Adafruit TinyUSB |
| Host brain | Waveshare RP2040-Zero | Adafruit TinyUSB Host (native) |

**Tinkerers:** the host brain must use **Tools -> USB Stack -> Adafruit TinyUSB Host (native)**. With any other USB Stack setting it won't compile, or won't act as a USB host.

The host brain uses its own vendored copy of TinyUSB in `Foxwizard_Host_Edition_Sidecar/src/`, with the RP2040 host fixes it needs, so whatever TinyUSB you have installed doesn't matter. See `VENDORED.md` there. The main brain uses the installed Adafruit TinyUSB Library.

## Flashing the host brain

How depends on which host sidecar you have.

### RP2040-Zero sidecar

Over the Zero's own USB-C port, like any Arduino upload. Unplug the OTG adapter and hub first, and hold BOOT while plugging in if the upload doesn't start.

### FoxWizard Host Edition sidecar board

The RP2040's USB is wired to the hub on the board, so it can't be flashed over USB. Use SWD instead, which also flashes a blank chip.

- **Pads:** SWCLK, SWDIO, and GND. 3V3 and RUN are optional, for voltage sensing and reset.
- **Probe:** a Raspberry Pi Debug Probe, or any Pico or RP2040-Zero running the `debugprobe` firmware.
- **Arduino IDE:** Tools -> Upload Method -> Picoprobe/Debug Probe (CMSIS-DAP), then upload as normal.

## Pins

### Main brain

Same as the FoxWizard, plus:

| Pin | Use |
|---|---|
| GP26 | Link TX to host brain GP29 (PIO serial) |
| GP27 | Link RX from host brain GP28 (PIO serial) |

### Host brain

| Pin | Use |
|---|---|
| GP28 | Link TX to main brain GP27 (UART0) |
| GP29 | Link RX from main brain GP26 (UART0) |
| GP4 | DIN/TRS MIDI OUT 1 (UART1 TX) |
| GP5 | DIN/TRS MIDI IN (UART1 RX) |
| GP6, GP7, GP8, GP9 | DIN/TRS MIDI OUT 2-5 (PIO serial TX) |
| GP10, GP11, GP12, GP13 | Hub port 1-4 status lights, active HIGH |
| GP14 | TRS board detection, internal pull-up. Floating: a bare sidecar module, no TRS. Grounded: a board with TRS 2-6 added |
| Native USB | USB host to the hub upstream port |

Both brains share ground.

Status lights stay on while a USB MIDI device is mounted on that port and blink off briefly on traffic.

## Host Edition board

- **Hub:** Microchip USB2514B, 4-port USB 2.0 hub, soldered on the board, so hub port numbers always match the same physical jacks.
- **Upstream:** RP2040 native USB D+ and D- go straight to the hub upstream port with 27 Ω series resistors at the RP2040. No connector and no CC on this link.
- **Hub support parts:** 24 MHz crystal, RBIAS resistor, and strap pins per the USB2514B datasheet reference schematic.
- **Downstream USB-C ports:** one 56 kΩ Rp resistor from each CC pin to 5 V, separate resistors on CC1 and CC2. This advertises Default USB power. Never fit 5.1 kΩ here, that is Rd, the device-side value.
- **Downstream USB-A ports:** no CC.
- **Power:** all ports share one 5 V bus, each through its own polyfuse or current-limit switch.
- **Programming:** SWD pads, no BOOT button needed. See Flashing the host brain.

## TRS 2-6 detection

The sidecar is a bare module with 4 USB host ports and nothing else, or that module with a board
added on top of it that wires up TRS 2-6 (shares the main brain's DIN/TRS pin layout: one shared
DIN IN/TRS OUT 1 plus four more TRS OUTs). GP14 tells the firmware which:

- **Floating** (its internal pull-up, nothing wired to it): a bare module. USB Host only.
- **Grounded:** a TRS board is present. USB Host plus TRS 2-6.

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
| 13-14 | From C3 BLE MIDI devices (planned) | C3 BLE MIDI devices (planned) |
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

Everything fitted: a Host Edition board with the TRS board added on top.

| | Count | Which |
|---|---|---|
| Inputs | 10 | TRS/DIN, USB Device, USB Host 1-7, TRS 2 |
| Outputs | 14 | TRS/DIN, USB Device, USB Host 1-7, TRS 2-6 |
| DIN/TRS outs | 6 | TRS/DIN on the main brain, TRS 2-6 on the sidecar |
| DIN/TRS ins | 2 | TRS/DIN on the main brain, TRS 2 on the sidecar |
| USB Host devices at once | 7 | 4 on the board's own jacks, up to 3 more through a hub plugged into one of them |

- Every port has its own switch in Patchling, as an input and as an output, and a route picks any combination of each. A route's ports are 16 bits, 14 used, 2 kept for BLE MIDI.
- The ports do not trade against each other. Each has its own fixed bit, so fewer USB Host devices does not free a bit for more DIN outs, and 7 USB Host with 6 DIN/TRS outs works at once. 5 sidecar DIN outs is the limit, there is no bit for a sixth.
- Grounding GP14 only shows TRS 2-6 in Patchling and sends nothing else. It takes no USB Host slot, the pins are separate (DIN/TRS GP4-GP9, hub lights GP10-GP13, USB Host is the native USB port).
- Fewer ports are not a gain today. Letting an empty USB Host slot lend its bit to more TRS or BLE ports is the Planned shared slots below, a route format change.

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

## Planned

- **Shared port slots:** a route's ports are 16 switches, 14 in use (USB, TRS/DIN, USB Host 1-7, TRS 2-6). Bits 14-15 and link ports 13-14 are kept for BLE MIDI. Later, a USB Host slot with no device in it, and the TRS and BLE ports, are meant to share the spare slots first come, first served, so a very full setup uses everything the format has. That needs ports numbered by what's plugged in rather than fixed bits, which is a route format change.

## Limits

- USB full speed (12 Mbps). Devices that only work at high speed (480 Mbps) won't work.
- MIDI 2.0 devices run in their MIDI 1.0 mode. Native MIDI 2.0 host support would be new firmware, not new hardware.
- USB cables (virtual ports) 0-7 pass through the link.
