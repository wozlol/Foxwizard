#!/bin/sh
# Builds the three FoxWizard sketches with the Arduino Tools settings they need, as a check that a first flash will work and as
# the reference for those settings. Nothing is flashed or published. Each sketch also has board_check.h, which stops a build with a
# wrong setting from the Arduino IDE.
#
# ./build_sketches.sh            all three
# ./build_sketches.sh main       the main brain (Mini_MIDI_Multitool)
# ./build_sketches.sh host       the USB host module
# ./build_sketches.sh bluetooth  the Bluetooth module (SuperMini, 4 MB flash)
# ./build_sketches.sh deluxe     the same sketch for the Deluxe's ESP32-S3-WROOM-1-N16R8 (16 MB flash, octal PSRAM)
set -e
cd "$(dirname "$0")/.."
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT
WHICH="${1:-all}"

build() {
  echo "== $1"
  arduino-cli compile --fqbn "$2" --build-path "$BUILD/$3" --build-property "compiler.cpp.extra_flags=$5" "$4" 2>&1 | grep -E 'error|Sketch uses|Global variables' || true
}

[ "$WHICH" = all ] || [ "$WHICH" = main ] && build "Main brain" "rp2040:rp2040:waveshare_rp2040_zero:usbstack=tinyusb,flash=2097152_1048576" main Mini_MIDI_Multitool
[ "$WHICH" = all ] || [ "$WHICH" = host ] && build "USB host module" "rp2040:rp2040:waveshare_rp2040_zero:usbstack=tinyusb_host,flash=2097152_1048576" host "Foxwizard Host Edition/Foxwizard_Host_Edition_Sidecar"
[ "$WHICH" = all ] || [ "$WHICH" = bluetooth ] && build "Bluetooth module" "esp32:esp32:esp32s3:FlashSize=4M,PSRAM=disabled,CDCOnBoot=default,USBMode=default,PartitionScheme=default,CPUFreq=240,DebugLevel=none" bluetooth "Foxwizard Host Edition/Foxwizard_Bluetooth_Sidecar"
[ "$WHICH" = all ] || [ "$WHICH" = deluxe ] && build "Bluetooth module on the Deluxe (N16R8)" "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=default,USBMode=default,PartitionScheme=app3M_fat9M_16MB,CPUFreq=240,DebugLevel=none" deluxe "Foxwizard Host Edition/Foxwizard_Bluetooth_Sidecar" "-DFW_S3_MODULE=3"
exit 0
