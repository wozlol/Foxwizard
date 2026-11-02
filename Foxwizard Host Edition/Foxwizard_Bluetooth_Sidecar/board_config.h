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
#define FW_BUTTON_ONE_MODE 1 // the default. Hold 3 s: add devices in both roles at once, no choosing host or device. Short press: stop adding
#define FW_BUTTON_CME 2      // CME's way, if it is ever needed: hold 3 s forces peripheral (a phone or computer connects), double press learns hosts, short press stops
#ifndef FW_BUTTON_STYLE
#define FW_BUTTON_STYLE FW_BUTTON_ONE_MODE
#endif
// Both styles: hold 10 s and let go to forget every Bluetooth device and bond.
constexpr uint8_t PIN_BOOT = 0;

// ---- Pins to the main brain -------------------------------------------------------------------------------------------------
// The module's UART0 pads, which is also where the chip's own bootloader listens, so the main brain can flash it over the same two wires.
constexpr uint8_t PIN_LINK_RX = 44; // from main brain GP15
constexpr uint8_t PIN_LINK_TX = 43; // to main brain GP14

// ---- The module's USB port --------------------------------------------------------------------------------------------------
// The S3's USB port is a host port for USB MIDI devices, on a hub too as far as the chip's 8 host channels go. It shares the chip's USB
// serial and JTAG function, so as a host the module is no longer a serial port on a computer. A saved setting picks which it is
// (a command from the main brain, then the module restarts), and the host starts only if the last start of it got through 5
// seconds, otherwise the port falls back to serial so the module can always be reached and flashed. 0 here turns the host off.
// A device needs 5 V on the port's VBUS, which the SuperMini and the Zero do not supply to a connected device, so use a powered hub.
#ifndef FW_USB_HOST
#define FW_USB_HOST 1
#endif

// ---- Limits -----------------------------------------------------------------------------------------------------------------
// Devices at once on the module, Bluetooth and USB together, each with a link port. The main brain keeps track of 4.
constexpr uint8_t MODULE_HANDLES = 4;
// BLE Hosts at once. NimBLE-Arduino allows 3 Bluetooth connections in all by default, one of them the BLE Device, so 2 hosts. To
// allow more, raise CONFIG_BT_NIMBLE_MAX_CONNECTIONS in the library's nimconfig.h (or a build_opt.h in this folder) to hosts + 1,
// then set this, and test it: more links means less airtime for each.
constexpr uint8_t BLE_HOSTS_MAX = 2; // of those, Bluetooth ones

// ---- Bench mode -------------------------------------------------------------------------------------------------------------
// 1 runs the module on its own for a first test, no main brain, with the USB port left as serial: it may connect up to BLE_HOSTS_MAX Bluetooth hosts when adding, and it
// prints what it hears on the USB serial port. 0 for the real thing.
#ifndef FW_BENCH
#define FW_BENCH 0
#endif
