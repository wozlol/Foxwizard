#pragma once

// The switches of the Bluetooth module, and its pins. Change these here, not in the sketch.

#include <stdint.h>

// ---- Which module this is -------------------------------------------------------------------------------------------------
// The status light is the module's own WS2812. The SuperMini has it on GPIO48, the Waveshare Zero on GPIO21.
#define FW_S3_SUPERMINI 1
#define FW_S3_ZERO 2
#ifndef FW_S3_MODULE
#define FW_S3_MODULE FW_S3_SUPERMINI
#endif

#if FW_S3_MODULE == FW_S3_SUPERMINI
constexpr uint8_t PIN_STATUS_RGB = 48;
constexpr const char *MODULE_NAME = "ESP32-S3 SuperMini";
#elif FW_S3_MODULE == FW_S3_ZERO
constexpr uint8_t PIN_STATUS_RGB = 21;
constexpr const char *MODULE_NAME = "Waveshare ESP32-S3-Zero";
#else
#error FW_S3_MODULE must be FW_S3_SUPERMINI or FW_S3_ZERO
#endif

// ---- The button -------------------------------------------------------------------------------------------------------------
// The module's BOOT button. A hold at power-on is the chip's own bootloader, so everything here happens while it runs.
#define FW_BUTTON_CME 1      // hold 3 s: force peripheral (a phone or computer connects). Double press: learn hosts. Short press: stop learning
#define FW_BUTTON_ONE_MODE 2 // hold 3 s: add devices in both roles at once. Short press: stop adding
#ifndef FW_BUTTON_STYLE
#define FW_BUTTON_STYLE FW_BUTTON_CME
#endif
// Both styles: hold 10 s and let go to forget every Bluetooth device and bond.
constexpr uint8_t PIN_BOOT = 0;

// ---- Pins to the main brain -------------------------------------------------------------------------------------------------
// The module's UART0 pads, which is also where the chip's own bootloader listens, so the main brain can flash it over the same two wires.
constexpr uint8_t PIN_LINK_RX = 44; // from main brain GP15
constexpr uint8_t PIN_LINK_TX = 43; // to main brain GP14

// ---- Limits -----------------------------------------------------------------------------------------------------------------
// BLE Hosts at once. NimBLE-Arduino allows 3 Bluetooth connections in all by default, one of them the BLE Device, so 2 hosts. To
// allow more, raise CONFIG_BT_NIMBLE_MAX_CONNECTIONS in the library's nimconfig.h (or a build_opt.h in this folder) to hosts + 1,
// then set this, and test it: more links means less airtime for each.
constexpr uint8_t BLE_HOSTS_MAX = 2;

// ---- Bench mode -------------------------------------------------------------------------------------------------------------
// 1 runs the module on its own for a first test, no main brain: it may connect up to BLE_HOSTS_MAX hosts when learning, and it
// prints what it hears on the USB serial port. 0 for the real thing.
#ifndef FW_BENCH
#define FW_BENCH 0
#endif
