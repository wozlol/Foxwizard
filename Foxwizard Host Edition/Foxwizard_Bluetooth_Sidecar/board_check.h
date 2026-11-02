#pragma once

// Checks the Arduino Tools settings for the Bluetooth module, so a first flash with a wrong one stops with the setting to change
// rather than a module that boots wrong. The build stops on a setting it can see. Flash size is not one of them, the compiler
// cannot see it, so boardCheckRun() reads it from the chip when the module starts and the status light blinks red if it is wrong.
//
// Tools settings for the ESP32-S3 SuperMini and Waveshare ESP32-S3-Zero (both 4 MB flash, 2 MB quad PSRAM):
//   Board                  ESP32S3 Dev Module
//   USB CDC On Boot        Enabled
//   USB Mode               Hardware CDC and JTAG
//   Flash Size             4MB (32Mb)
//   PSRAM                  Disabled (QSPI PSRAM also works, OPI PSRAM does not)
//   Partition Scheme       Default 4MB with spiffs
//   CPU Frequency          240MHz (WiFi)
//   Core Debug Level       None
// Libraries: NimBLE-Arduino 2.x.

#include <Arduino.h>
#include <esp_flash.h>

#ifndef CONFIG_IDF_TARGET_ESP32S3
#error Tools -> Board must be "ESP32S3 Dev Module" (the esp32 board package by Espressif).
#endif

#if !defined(ARDUINO_USB_CDC_ON_BOOT) || !ARDUINO_USB_CDC_ON_BOOT
#error Tools -> USB CDC On Boot must be Enabled. Disabled puts Serial on GPIO43 and GPIO44, the wires to the main brain.
#endif

#if !defined(ARDUINO_USB_MODE) || !ARDUINO_USB_MODE
#error Tools -> USB Mode must be "Hardware CDC and JTAG".
#endif

#if defined(BOARD_HAS_PSRAM) && defined(CONFIG_SPIRAM_MODE_OCT)
#error Tools -> PSRAM must be Disabled or "QSPI PSRAM". The SuperMini and the Zero have quad PSRAM and will not start with OPI.
#endif

#if F_CPU != 240000000L
#warning Tools -> CPU Frequency should be 240MHz. The Bluetooth module was written for that.
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
  if (result == BOARD_CHECK_FLASH_TOO_BIG) out.printf("  CHECK FAILED: Tools -> Flash Size is larger than this chip. Set it to 4MB (32Mb) and upload again.\n");
  else out.printf("  board check: settings look right\n");
  return result;
}
