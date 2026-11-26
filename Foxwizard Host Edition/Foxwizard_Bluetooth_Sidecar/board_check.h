#pragma once

// Checks the Arduino Tools settings for the Bluetooth module, so a first flash with a wrong one stops with the setting to change
// rather than a module that boots wrong. The settings are listed at the top of the sketch. The build stops on a setting it can see.
// Flash size is not one of them, the compiler cannot see it, so boardCheckRun() reads it from the chip when the module starts and the
// status light blinks red if it is wrong.

#include <Arduino.h>
#include <esp_flash.h>

#include "board_config.h"

#ifndef CONFIG_IDF_TARGET_ESP32S3
#error Tools -> Board must be "ESP32S3 Dev Module" (the esp32 board package by Espressif).
#endif

#if !defined(ARDUINO_USB_MODE) || ARDUINO_USB_MODE
#error Tools -> USB Mode must be "USB-OTG (TinyUSB)". The module's USB port is a USB MIDI host or device, which Hardware CDC and JTAG cannot be.
#endif

#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
#error Tools -> USB CDC On Boot must be Disabled. Enabled makes the module a serial port on a computer as well as a MIDI device.
#endif

#if FW_S3_MODULE == FW_S3_DELUXE
#if defined(BOARD_HAS_PSRAM) && defined(CONFIG_SPIRAM_MODE_QUAD)
#error Tools -> PSRAM must be "OPI PSRAM" or Disabled. The Deluxe's N16R8 has octal PSRAM and will not start with QSPI.
#endif
#else
#if defined(BOARD_HAS_PSRAM) && defined(CONFIG_SPIRAM_MODE_OCT)
#error Tools -> PSRAM must be Disabled or "QSPI PSRAM". The SuperMini and the Zero have quad PSRAM and will not start with OPI.
#endif
#endif

#if F_CPU != 240000000L
#error Tools -> CPU Frequency should be 240MHz. The Bluetooth module was written for that.
#endif

constexpr uint8_t BOARD_CHECK_OK = 0;
constexpr uint8_t BOARD_CHECK_FLASH_TOO_BIG = 1; // Flash Size is set larger than the chip has

// Reads what the chip and the flashed image say and prints it. Returns BOARD_CHECK_*.
inline uint8_t boardCheckRun(Print &out, const char *moduleName, uint8_t firmwareVersion) {
  uint32_t physical = 0;
  esp_flash_get_physical_size(nullptr, &physical);
  uint8_t header[4] = {0, 0, 0, 0};
  esp_flash_read(nullptr, header, 0, sizeof(header)); // the bootloader's header, where the Flash Size setting was written
  const uint8_t sizeCode = header[3] >> 4;
  const uint32_t headerBytes = sizeCode <= 5 ? (uint32_t)(1u << 20) << sizeCode : 0;
  uint8_t result = BOARD_CHECK_OK;
  if (headerBytes > physical && physical > 0) result = BOARD_CHECK_FLASH_TOO_BIG;

  out.printf("\nFoxWizard Bluetooth module %u on %s\n", firmwareVersion, moduleName);
  out.printf("  chip %s rev %u, %u MHz\n", ESP.getChipModel(), (unsigned)ESP.getChipRevision(), (unsigned)ESP.getCpuFreqMHz());
  out.printf("  flash: chip %u KB, Flash Size setting %u KB\n", (unsigned)(physical / 1024), (unsigned)(headerBytes / 1024));
  out.printf("  PSRAM %u KB (not used)\n", (unsigned)(ESP.getPsramSize() / 1024));
  out.printf("  heap: %u KB free of %u KB, lowest since start %u KB\n", (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getHeapSize() / 1024), (unsigned)(ESP.getMinFreeHeap() / 1024));
  if (result == BOARD_CHECK_FLASH_TOO_BIG) out.printf("  CHECK FAILED: Tools -> Flash Size is larger than this chip. Set it to %uMB and upload again.\n", (unsigned)EXPECTED_FLASH_MB);
  else if (headerBytes < (uint32_t)EXPECTED_FLASH_MB << 20) out.printf("  Flash Size is set below the %u MB this module has, set %uMB for the whole chip. It still runs.\n", (unsigned)EXPECTED_FLASH_MB, (unsigned)EXPECTED_FLASH_MB);
  else out.printf("  board check: settings look right\n");
  return result;
}
