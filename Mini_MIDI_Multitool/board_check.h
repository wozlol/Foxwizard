#pragma once

// Checks the Arduino Tools settings for the main brain, so a first flash with a wrong one stops with the setting to change instead of
// a unit that boots with presets that will not save. The settings are listed at the top of the sketch. The build stops on any setting
// it can see.

#ifndef ARDUINO_ARCH_RP2040
#error Tools -> Board must be "Waveshare RP2040 Zero" (the Raspberry Pi Pico/RP2040 board package by Earle Philhower).
#endif

#ifndef USE_TINYUSB
#error Tools -> USB Stack must be "Adafruit TinyUSB".
#endif

#ifdef USE_TINYUSB_HOST
#error Tools -> USB Stack must be "Adafruit TinyUSB", not "Adafruit TinyUSB Host (native)". That one is for the USB host module.
#endif

#if PICO_FLASH_SIZE_BYTES != 2097152
#error Tools -> Flash Size must be a 2MB option. This chip has 2 MB of flash.
#endif

#if (FS_END - FS_START) != 1048576
#error Tools -> Flash Size must be "2MB (Sketch: 1MB, FS: 1MB)". Presets, the loop and the Bluetooth slots live in that 1 MB filesystem.
#endif

#if F_CPU != 200000000L
#error Tools -> CPU Speed should be the default, 200 MHz. The clock and the link were measured there.
#endif
