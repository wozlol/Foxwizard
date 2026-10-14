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

Untested on hardware. Both sketches compile.

## Folders

| Folder | Firmware |
|---|---|
| `Mini_MIDI_Multitool_Host_Edition` | Main brain. Fork of `../Mini_MIDI_Multitool` for testing. Includes the same firmware update over SysEx. Version 1.4. Once tested on hardware, it replaces `../Mini_MIDI_Multitool` as the only FoxWizard firmware, released as 1.4, for units with and without a host brain. Because 1.4 is newer than the released 1.3, Patchling never offers to replace it with the regular firmware. |
| `Foxwizard_Host_Edition_Sidecar` | Host brain, on the RP2040-Zero or the Host Edition sidecar board. |

## Arduino settings

| Sketch | Board | Tools -> USB Stack |
|---|---|---|
| Main brain | Waveshare RP2040-Zero, Flash Size 2MB (Sketch: 1792KB, FS: 256KB) | Adafruit TinyUSB |
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

## Testing on an RP2040-Zero

1. Flash the host sketch (see Flashing the host brain), then unplug the computer.
2. Check continuity between the 5V pin and USB-C VBUS.
3. Power the Zero from 5 V on the 5V pin. Never connect the computer while that supply is on.
4. Plug a USB-C male to USB-A female OTG adapter into the Zero, then a USB-A hub or MIDI device into that.
5. Use a 4-port hub. Devices on hub ports above 4 are ignored. With no hub, a device plugged straight in counts as port 1.

The Zero's onboard RGB LED (GP16) is a diagnostic light. It flashes red, green, blue once at power-up, so you can confirm the colors look right, then:

| Light | Meaning |
|---|---|
| Red | No USB device |
| Yellow | A USB device that isn't USB MIDI, or a MIDI device on a hub port above 4 |
| Green | USB MIDI device ready |
| White flash | MIDI received from a USB device |
| Slow blinking | Nothing from the main brain for over 1 second: check the link wires, the ground wire, and that the main brain runs the Host Edition firmware |

USB-A devices and hubs have no CC, so the Zero's own 5.1 kΩ CC resistors don't matter. A USB-C device on a USB-C to USB-C cable will not power up from the Zero.

## Link protocol

UART, 1,000,000 baud, 8N1. One frame carries one USB-MIDI packet plus a port number.

| Byte | Contents |
|---|---|
| 0 | bit 7 = 1, bits 6-4 = USB cable 0-7, bits 3-0 = CIN |
| 1 | bit 7 = 0, bits 6-3 = port, bit 2 = top bit of MIDI byte 3, bit 1 = MIDI byte 2, bit 0 = MIDI byte 1 |
| 2, 3, 4 | MIDI bytes 1, 2, 3, low 7 bits |

Only byte 0 has bit 7 set, so after any glitch the receiver resyncs on the next frame.

| Port | Host to main | Main to host |
|---|---|---|
| 0 | | Every USB device and every DIN OUT (the main brain doesn't use it) |
| 1-4 | From hub port 1-4 | Hub port 1-4 only |
| 5 | From host DIN IN | DIN OUT 1 only |
| 6-9 | | DIN OUT 2-5 only |
| 10-14 | From C3 BLE MIDI devices (planned) | C3 BLE MIDI devices (planned) |
| 15 | Control | Control |

Port 15 carries control frames, with the frame type in the CIN field:

| Type | Direction | Contents |
|---|---|---|
| 1, port map | Host to main | Byte 1 = connected port mask (bit 0 = hub port 1), byte 2 = host firmware version, byte 3 = drop count (max 127). Sent on any plug or unplug and every 500 ms. |
| 2, flow | Both ways | Byte 1 = 1 pause, 0 resume. Sent on change and every 100 ms. |

`foxwizard_link.h` holds the frame code and queue rules. Both sketch folders carry an identical copy.

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

- Realtime messages (clock, start, stop, continue) and control frames skip every ordinary queue on both brains, and still flow while a brain is paused. A clock tick waits at most one frame, 50 µs.
- On DIN OUTs, realtime bytes go out between the bytes of other messages, as MIDI allows.
- USB adds up to 1 ms of wobble, the same as any USB MIDI device on a computer.
- The main brain parses at most 256 link bytes per loop and only writes to the link while the PIO FIFO has room. It never waits on the host brain.

## Ports and routing

| Port | Input | Output | Where |
|---|---|---|---|
| TRS/DIN | yes | yes | Main brain |
| USB Device | yes | yes | Main brain, to a computer |
| USB Host 1-4 | yes | yes | Sidecar, one per hub port |
| TRS 2 | yes | yes | Sidecar DIN IN and DIN OUT 1 |
| TRS 3-6 | | yes | Sidecar DIN OUT 2-5 |

Sidecar ports exist only while a sidecar is present. Without routes:

- **USB Adaptor Mode:** every input goes to every other port, never back to the port it came in on. A device on USB Host 1 reaches USB Host 2-4 but never hears itself.
- **Router Mode:** every input goes to every output, its own port included.

Routes take over from the mode for whatever they match. Each route picks any combination of input ports and output ports. In Patchling the USB Host and TRS 2-6 switches appear once a sidecar is detected, the badge adds "· Host", and a green dot marks USB Host ports with a device plugged in. Patchling never says "sidecar" to customers, who buy a FoxWizard or a FoxWizard Host Edition.

- SysEx replies to FoxWizard commands from the sidecar go back out the port they came in on.
- USB devices with no MIDI OUT (input-only controllers) are never sent anything.

## Recovery

- **Both brains:** a hardware watchdog reboots the brain if its loop stops running, 4 s on the main brain and 2 s on the sidecar. That covers a hang anywhere, a USB hot-plug hang included.
- **Sidecar:** if queued output to a USB device hasn't moved for 3 s, the USB host has wedged, and the sidecar reboots. Every device re-enumerates in about a second.
- **USB driver faults:** stock TinyUSB's RP2040 host driver halts the chip with `panic()` on a USB data sequence error, a buffer armed twice, or a request to clear a device's stalled endpoint. The sidecar's vendored copy (`Foxwizard_Host_Edition_Sidecar/src/Adafruit_TinyUSB/VENDORED.md`) restarts at once instead, with the fault as the reason, rather than halting until the watchdog fires. Recovering in place without a restart was tried and caused more faults, so it was taken back out.
- **Restart reports:** after a recovery restart the sidecar's diagnostic light flashes magenta twice instead of red, green, blue. Patchling shows the reason in a red message that stays until clicked. Clicking the badge shows the last reason again.
- **Buffer armed twice:** the only fault seen so far, and only right after plugging USB devices in or out, never while playing. Likely a race in TinyUSB's RP2040 hot-plug cleanup. It recovers in about a second, so Patchling shows it as a brief yellow "reset its USB ports after a device change" instead.
- **Stuck notes:** when the sidecar restarts or disappears, the main brain sends Note Offs for every note still held from USB Host and TRS 2, through the same routes as a real Note Off. When the sidecar restarts or comes back, the main brain also sends All Notes Off (CC 123) on all channels to the sidecar's ports only, for notes those outputs missed while it was gone. The main brain's own ports are left alone.
- **Main brain:** handles at most 64 USB Device packets and 64 TRS/DIN bytes per loop, so a flood such as a DAW feedback loop can't starve the link, the lights, or the button.

## Limits

- USB full speed (12 Mbps). Devices that only work at high speed (480 Mbps) won't work.
- MIDI 2.0 devices run in their MIDI 1.0 mode. Native MIDI 2.0 host support would be new firmware, not new hardware.
- USB cables (virtual ports) 0-7 pass through the link.
