# Vendored Updater

The firmware update from the main brain (host firmware 10) needs the Updater, which stages the image in a LittleFS file for the OTA bootloader. Those libraries are copied here because arduino-pico's `LittleFS.cpp` includes `<Adafruit_TinyUSB.h>`, which makes Arduino compile an installed TinyUSB next to the vendored one in `../Adafruit_TinyUSB` and the link fails with every symbol defined twice.

- `Updater.*`, `Updater_Signing.h`, `PicoOTA.*`, `MD5Builder.*`, `LittleFS.*`: arduino-pico 5.6.1 `libraries/`, LGPL 2.1.
- `littlefs/`: the littlefs C library from arduino-pico 5.6.1 `libraries/LittleFS/lib/littlefs`, BSD (`LICENSE.md`).

## Changed from stock

| File | Change | Why |
|---|---|---|
| `Updater.cpp`, `Updater.h`, `PicoOTA.cpp`, `PicoOTA.h`, `MD5Builder.cpp` | `#include <X.h>` of the vendored headers made `#include "X.h"` | An angle include makes Arduino pick the core's copy of the library as well |
| `LittleFS.h` | `../lib/littlefs/lfs.h` is `littlefs/lfs.h` | The C library sits next to it |
| `LittleFS.cpp` | The `#ifdef USE_TINYUSB` block that includes `<Adafruit_TinyUSB.h>` is `#if 0` | The clash above |
| `littlefs/lfs.c` | The defines and pragma from the library's `src/lfs.c` stub are at the top | The stub is not copied |

Moving to another arduino-pico release: copy the same files again and redo the table, the filesystem format must stay the same as the bootloader's.
