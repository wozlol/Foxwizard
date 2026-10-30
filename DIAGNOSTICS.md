# FoxWizard diagnostics

How to see what the main brain is doing, and how much room it has left. Needs a Mac with the unit on USB.

## The check tool

`tools/clock_check.swift`, built with `swiftc -O tools/clock_check.swift -o clock_check`. `./clock_check list` shows the MIDI ports, `--port NAME` picks one.

| Command | What it does |
|---|---|
| `version` | Firmware and protocol, the sidecar, the memory readout, the last restart |
| `all` | The whole clock check: the clock it sends, following one sent to it, taking over and re-locking. Needs Send Clock on |
| `out [seconds]`, `in [bpm] [seconds]` | The two halves of it |
| `session [seconds]` | A play session: chords, drum rolls, aftertouch floods, CC and pitch bend sweeps, Patchling-style loads. With no routes on the unit it loads a test setup into memory only, never saved, and restores it. Reports notes that never ended, memory, loop time, and whether the unit restarted. `--no-setup`, `--no-live`, `--skip chord,roll,at,sweep,sysex`, `--only-routes 1,2`, `--channels 1,10` |
| `livetest` | Each Live Ch action once, checking the unit survives |
| `atflood` | Poly aftertouch at rising rates through a route |
| `dinflood [--rates a,b]` | Aftertouch to the DIN port, to see the queue and the thinning |
| `flood`, `probe` | A steady CC stream, and single notes through each test route |
| `selftest` | The clock checks against a simulated unit, to prove they pass a good clock and catch a bad one |

## What the numbers should look like

| Number | Now | Keep |
|---|---|---|
| Free heap, lowest since power on (of about 44 KB at start) | 29 KB | at least 20 KB, the MIDI 2.0 floor |
| Main stack, deepest (of 2,048 bytes) | 1,200 | at least 500 bytes free, so under 1,500 |
| Saved settings image, worst case (60 routes) | 4,024 of 4,096 | the build fails if it does not fit |
| Longest main loop pass | about 2 ms | well under 5 ms, the watchdog is 4 s |
| Program flash | 18% of 1 MB | firmware staging needs the image plus 16 KB free in the filesystem |

The stack is the one to watch: it is 2 KB and the note path nests deep (input, route, mapped action, live channel replay, route, output, USB). Run `./clock_check session 100` after a change that adds calls or locals and read the stack line.

## The crash tracker (1.110)

Each stage of the main loop and the note path writes its number to a watchdog scratch register, which survives a restart. `GET_STATUS` reports the last one after a watchdog restart, and `clock_check` prints it: "last watchdog restart was in stage N". The stage stays until another writes over it, so it names the last place the loop went through, and a stall is after it.

| Stage | Where |
|---|---|
| 2 | `processIncomingEvent` |
| 3, 4 | `dispatchNoteEvent`, a key played again for a live channel or octave change |
| 5 | `runAction` |
| 6 to 10 | `setLiveChannel`: start, after the release, before the arp, after the change, end |
| 20 to 51 | `routeReplayHeld`, the held key being replayed |
| 60, 61, 62 | `sendRouteNote`, `flushPending`, `sendToOutputs` |
| 63, 65, 66 | `pumpUsbOut`: start, before and after the TinyUSB write |
| 83 | the end of `usbSend` |
| 64 | `serviceArps` |
| 70 to 82 | the main loop between its services, 81 and 82 after the USB output and the link flow |
| 84, 85, 86 | `pumpUsbIn`: before and after the TinyUSB read, and after its packets are handled |

A **hard fault** is caught too (1.112): the unit saves the address it stopped at and restarts at once. `clock_check` prints "HARD FAULT at address 0x...". Look the address up with `arm-none-eabi-addr2line -f -e <build>/Mini_MIDI_Multitool.ino.elf 0x...` (the toolchain is in the Arduino15 packages folder).

`STATUS_DATA` bytes 28 to 40 carry this, see `SYSEX_PROTOCOL.md`. A restart the unit asked for, a firmware update or a power cycle, leaves no stage.

## The USB stall (fixed in 1.111 and 1.113)

Reading and writing USB one 4-byte packet at a time started a USB transfer for every packet. Under a flood of aftertouch passing through a route, with traffic both ways, the unit stalled in TinyUSB for 4 seconds until the watchdog restarted it, dropping every route held only in memory. `pumpUsbOut` now writes up to 16 packets per call and `pumpUsbIn` reads up to 16, with one transfer start each. Before: the unit restarted at 100 aftertouch messages a second. After: it survived 1,600 a second, 100 seconds of a mixed session with nothing stuck. Any new USB code, MIDI 2.0 included, must batch.

## Aftertouch thinning (1.114)

The DIN port carries about 3 KB a second. Past half full its queue thins poly aftertouch and channel pressure, every 2nd update, then every 4th, 8th and 16th, counted per note and channel, and a pressure of 0 always goes. Before: a flood of 1,600 a second filled the DIN queue to 83% and held the whole input back. `dinflood` shows it, and 1.115 reports how many updates went out and how many were thinned.
