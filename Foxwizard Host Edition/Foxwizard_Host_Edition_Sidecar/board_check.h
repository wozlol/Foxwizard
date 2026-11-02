#pragma once

// Checks the Arduino Tools settings for the USB host module (the RP2040 sidecar), so a first flash with a wrong one stops with the
// setting to change.
//
// Tools settings for the Waveshare RP2040-Zero or the Host Edition board:
//   Board          Waveshare RP2040 Zero
//   Flash Size     2MB (no FS), or any 2MB option, the filesystem is not used
//   USB Stack      Adafruit TinyUSB Host (native)
//   CPU Speed      200 MHz (the default)
// The TinyUSB it uses is vendored in src/, so no TinyUSB library is needed.

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
#warning Tools -> CPU Speed should be the default, 200 MHz. The link and the DIN ports were measured there.
#endif
