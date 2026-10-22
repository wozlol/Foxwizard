# Vendored Adafruit TinyUSB

Private TinyUSB for the sidecar sketch, so it compiles the same on any machine whatever TinyUSB is installed, and core updates can't change it.

- TinyUSB stack (`class`, `common`, `device`, `host`, `osal`, `typec`, `portable/raspberrypi/rp2040`, `tusb.*`): hathach/tinyusb master, commit 4c195b7 (2026-10-14). Includes the RP2040 host fixes #4166 (a failed EPX transfer's buffer is released, not left armed) and #4177 (interrupt endpoint transfers abort).
- Arduino glue (`arduino/`, `Adafruit_TinyUSB.h`, `tusb_config.h`): Adafruit TinyUSB Library 3.7.7, with `arduino/ports/` trimmed to `rp2040`. Adafruit hasn't released past 3.7.7.

Upstream's RP2040 driver files alone, over 3.7.7's stack, broke a Launchpad's notes, so the stack and driver always move together.

## Changed from stock

| File | Change | Why |
|---|---|---|
| Every file | Includes rewritten to file-relative paths | A sketch's `src/` isn't on the include path, and three angle includes in `class/bth` made Arduino pull in an installed TinyUSB |
| `arduino/Adafruit_USBH_Host.h` | `<SPI.h>` only with `CFG_TUH_MAX3421` | The core's `SPI.cpp` includes `<Adafruit_TinyUSB.h>`, which dragged in an installed TinyUSB |
| `arduino/ports/rp2040/tusb_config_rp2040.h` | `CFG_TUH_HUB` 4 (stock 1), `CFG_TUH_DEVICE_MAX` 13, `CFG_TUH_MIDI` 13 (stock 1), `CFG_TUH_ENUMERATION_BUFSIZE` 512 (stock 256), `CFG_TUH_TASK_EVENTS_PER_RUN` 0, MSC/HID/CDC host drivers off | Hubs chained inside a hub or plugged into one enumerate, more devices than the four slots wait their turn, devices with big descriptors enumerate, hub traffic doesn't lag |
| `portable/raspberrypi/rp2040/hcd_rp2040.c` | Data sequence error and `hcd_edpt_clear_stall()` call `foxwizard_usb_fault()`. Stock: `panic()` | The sketch restarts at once with a named reason instead of halting until the watchdog fires |
| `portable/raspberrypi/rp2040/rp2040_usb.c` | Buffer armed twice calls `foxwizard_usb_fault()`. Stock: `panic()` | Same |
| `foxwizard_usb_fault.h` | New | The fault hook, defined in `Foxwizard_Host_Edition_Sidecar.ino`, and the hub port count accessor |
| `host/hub.c` | `foxwizard_hub_port_count()` added | The sketch counts how many jacks the attached hubs have |

Recovering in place (rolling back the data sequence, clearing stalls to DATA0) was tried and caused more faults, mostly buffers armed twice, so every fault restarts instead.

## Updating

Copy a newer tinyusb `src/` (the folders listed at the top) over this folder, keep the Adafruit glue, rerun the include rewrite, and reapply every change above. Check every `CFG_TUH_*` in `tusb_config_rp2040.h` is still used by the stack. Then compile and check the build output's "Used library" list has no Adafruit TinyUSB in it.
