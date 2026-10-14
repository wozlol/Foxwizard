# Vendored Adafruit TinyUSB

Private copy of Adafruit TinyUSB Library 3.7.7 (`src/` only) for the sidecar sketch, so it compiles the same on any machine whatever TinyUSB is installed, and core updates can't change it.

## Trimmed

- `portable/`: everything except `raspberrypi/rp2040`.
- `arduino/ports/`: everything except `rp2040`.

## Changed from stock

| File | Change | Why |
|---|---|---|
| Every file | Includes rewritten to file-relative paths | A sketch's `src/` isn't on the include path, and three angle includes in `class/bth` made Arduino pull in an installed TinyUSB |
| `arduino/Adafruit_USBH_Host.h` | `<SPI.h>` only with `CFG_TUH_MAX3421` | The core's `SPI.cpp` includes `<Adafruit_TinyUSB.h>`, which dragged in an installed TinyUSB |
| `arduino/ports/rp2040/tusb_config_rp2040.h` | `CFG_TUH_MIDI` 4 (stock 1), `CFG_TUH_ENUMERATION_BUFSIZE` 512 (stock 256), `CFG_TUH_TASK_EVENTS_PER_RUN` 0, MSC/HID/CDC host drivers off | Four MIDI devices on the hub, devices with big descriptors enumerate, hub traffic doesn't lag |
| `class/midi/midi_host.c/.h` | Adds `tuh_midi_rx_rearm()` | Carried over from the ARPnMIDI copy, unused by the sidecar |
| `portable/raspberrypi/rp2040/hcd_rp2040.c` | Data sequence error and `hcd_edpt_clear_stall()` call `foxwizard_usb_fault()`. Stock: `panic()` | The sketch restarts at once with a named reason instead of halting until the watchdog fires |
| `portable/raspberrypi/rp2040/rp2040_usb.c` | Buffer armed twice calls `foxwizard_usb_fault()`. Stock: `panic()` | Same |
| `foxwizard_usb_fault.h` | New | The fault hook, defined in `Foxwizard_Host_Edition_Sidecar.ino` |

Recovering in place (rolling back the data sequence, clearing stalls to DATA0) was tried and caused more faults, mostly buffers armed twice, so every fault restarts instead.

## Updating

Copy a newer library's `src/` over this folder, trim it the same way, rerun the include rewrite, and reapply every change above. Then compile and check the build output's "Used library" list has no Adafruit TinyUSB in it.
