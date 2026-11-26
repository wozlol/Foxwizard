#pragma once

// Checks the Arduino Tools settings for the USB host module (the RP2040 sidecar), so a first flash with a wrong one stops with the
// setting to change. The settings are listed at the top of the sketch.

#ifndef ARDUINO_ARCH_RP2040
#error Tools -> Board must be "Waveshare RP2040 Zero" (the Raspberry Pi Pico/RP2040 board package by Earle Philhower).
#endif

#ifndef USE_TINYUSB_HOST
#error Tools -> USB Stack must be "Adafruit TinyUSB Host (native)".
#endif

#if PICO_FLASH_SIZE_BYTES != 2097152
#error Tools -> Flash Size must be a 2MB option. This chip has 2 MB of flash.
#endif

#if F_CPU != 200000000L
#error Tools -> CPU Speed should be the default, 200 MHz. The link and the DIN ports were measured there.
#endif
