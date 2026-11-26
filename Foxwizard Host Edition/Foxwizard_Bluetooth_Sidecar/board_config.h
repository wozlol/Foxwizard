#pragma once

// The switches of the Bluetooth module, and its pins. Change these here, not in the sketch.

#include <stdint.h>

// ---- Which module this is -------------------------------------------------------------------------------------------------
// The status light is the module's own WS2812. The SuperMini has it on GPIO48, the Waveshare Zero on GPIO21. The Deluxe board has an
// ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM) and its WS2812 on GPIO48 too, which stays 3.3 V on the N16R8 (only the R16V
// parts run it at 1.8 V). Its PSRAM is for the audio looper, the sampler and AMY, the Bluetooth and USB code here does not use it.
#define FW_S3_SUPERMINI 1
#define FW_S3_ZERO 2
#define FW_S3_DELUXE 3
#ifndef FW_S3_MODULE
#define FW_S3_MODULE FW_S3_SUPERMINI
#endif

#if FW_S3_MODULE == FW_S3_SUPERMINI
constexpr uint8_t PIN_STATUS_RGB = 48;
constexpr const char *MODULE_NAME = "ESP32-S3 SuperMini";
constexpr uint32_t EXPECTED_FLASH_MB = 4;
#elif FW_S3_MODULE == FW_S3_ZERO
constexpr uint8_t PIN_STATUS_RGB = 21;
constexpr const char *MODULE_NAME = "Waveshare ESP32-S3-Zero";
constexpr uint32_t EXPECTED_FLASH_MB = 4;
#elif FW_S3_MODULE == FW_S3_DELUXE
constexpr uint8_t PIN_STATUS_RGB = 48;
constexpr const char *MODULE_NAME = "FoxWizard Deluxe, ESP32-S3-WROOM-1-N16R8";
constexpr uint32_t EXPECTED_FLASH_MB = 16;
#else
#error FW_S3_MODULE must be FW_S3_SUPERMINI, FW_S3_ZERO or FW_S3_DELUXE
#endif

// ---- The button -------------------------------------------------------------------------------------------------------------
// The module's BOOT button. A hold at power-on is the chip's own bootloader, so everything here happens while it runs.
#define FW_BUTTON_ONE_MODE 1 // the default. Tap: add devices in both roles at once, nothing connected is dropped, tap again stops. Hold 3 s: radio on or off. Hold 10 s: forget
#define FW_BUTTON_CME 2      // CME's way, if it is ever needed: hold 3 s forces peripheral (a phone or computer connects), double press learns hosts, short press stops
#ifndef FW_BUTTON_STYLE
#define FW_BUTTON_STYLE FW_BUTTON_ONE_MODE
#endif
// Both styles forget every Bluetooth device and bond at 10 s held (the default style at once, the CME style when the button is let go).
constexpr uint8_t PIN_BOOT = 0;

// ---- Pins to the main brain -------------------------------------------------------------------------------------------------
// The module's UART0 pads, which is also where the chip's own bootloader listens, so the main brain can flash it over the same two wires.
constexpr uint8_t PIN_LINK_RX = 44; // from main brain GP15
constexpr uint8_t PIN_LINK_TX = 43; // to main brain GP14

// ---- The module's USB port --------------------------------------------------------------------------------------------------
// The S3's USB port is either a USB MIDI host or a USB MIDI device, never a serial port. As a host it plays USB MIDI devices, on a hub
// too as far as the chip's 8 host channels go. As a device the module is a USB MIDI port on a computer (named USB_DEVICE_NAME), a
// second way into BLE Device's link port 9 beside Bluetooth. A saved setting picks which it is (a command from the main brain, then
// the module restarts), and the host starts only if the last start of it got through 5 seconds, otherwise the port falls back to
// device so the module keeps working. A host needs 5 V on the port's VBUS, which the SuperMini and the Zero do not supply to a
// connected device, so use a powered hub. 0 here leaves the host out and the port is always a device.
#ifndef FW_USB_HOST
#define FW_USB_HOST 1
#endif
constexpr const char *USB_DEVICE_NAME = "FoxWizard Bluetooth"; // what a computer lists the module as
constexpr const char *USB_MAKER_NAME = "woz.lol";

// ---- Limits -----------------------------------------------------------------------------------------------------------------
// Devices at once on the module, Bluetooth and USB together, each with a link port. The main brain keeps track of 4.
constexpr uint8_t MODULE_HANDLES = 4;
// BLE Hosts at once. NimBLE-Arduino allows 3 Bluetooth connections in all by default, one of them the BLE Device, so 2 hosts. To
// allow more, raise CONFIG_BT_NIMBLE_MAX_CONNECTIONS in the library's nimconfig.h (or a build_opt.h in this folder) to hosts + 1,
// then set this, and test it: more links means less airtime for each.
constexpr uint8_t BLE_HOSTS_MAX = 2; // of those, Bluetooth ones
// USB devices at once on the module's own USB port. The chip has 8 USB host channels and each endpoint uses one, so a hub and 2 MIDI devices
// use about all of them. A third device is ignored until it is plugged in again once one of the two is gone.
constexpr uint8_t USB_HOSTS_MAX = 2; // of those, USB ones, so 2 + 2 fills MODULE_HANDLES

// ---- Bench mode -------------------------------------------------------------------------------------------------------------
// 1 runs the module on its own for a first test, no main brain, with the USB port a serial port: it may connect up to BLE_HOSTS_MAX Bluetooth hosts when adding, and it
// prints what it hears on that port. 0 for the real thing, where the USB port is never a serial port.
#ifndef FW_BENCH
#define FW_BENCH 0
#endif
