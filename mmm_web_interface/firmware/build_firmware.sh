#!/bin/sh
# Builds ../../Mini_MIDI_Multitool and publishes it for Patchling's firmware check:
# foxwizard-firmware-X.Y.bin (sent over MIDI), foxwizard-firmware-X.Y.uf2 (for the FIRMWARE pads,
# upload it to woz.lol/foxwizard-midi-multitool-firmware), and firmware.json (version, size, MD5).
# Patchling and the firmware always share one version number. This refuses to publish unless the
# Patchling footer (index.html) and cache name (sw.js) match the firmware version.
#
# ./build_firmware.sh --test MINOR publishes the same firmware labeled MAJOR.MINOR, for testing the
# update. The source is not changed and the version check is skipped. Run ./build_firmware.sh
# afterwards to publish the real version again.
set -e
cd "$(dirname "$0")"
SKETCH=../../Mini_MIDI_Multitool
INO="$SKETCH/Mini_MIDI_Multitool.ino"
FQBN=rp2040:rp2040:waveshare_rp2040_zero:usbstack=tinyusb,flash=2097152_262144
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

TEST_MINOR=""
if [ "$1" = "--test" ]; then
  TEST_MINOR="$2"
  case "$TEST_MINOR" in ''|*[!0-9]*) echo "Usage: $0 --test MINOR" >&2; exit 1 ;; esac
  # Build a copy with the test version, leaving the source alone.
  mkdir "$BUILD/Mini_MIDI_Multitool"
  sed "s/^constexpr uint8_t FW_VERSION_MINOR = [0-9]*;/constexpr uint8_t FW_VERSION_MINOR = $TEST_MINOR;/" "$INO" > "$BUILD/Mini_MIDI_Multitool/Mini_MIDI_Multitool.ino"
  SKETCH="$BUILD/Mini_MIDI_Multitool"
  INO="$SKETCH/Mini_MIDI_Multitool.ino"
fi

arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD/out" "$SKETCH"

MAJOR=$(sed -n 's/^constexpr uint8_t FW_VERSION_MAJOR = \([0-9]*\);.*/\1/p' "$INO")
MINOR=$(sed -n 's/^constexpr uint8_t FW_VERSION_MINOR = \([0-9]*\);.*/\1/p' "$INO")
NAME="foxwizard-firmware-$MAJOR.$MINOR"

if [ -z "$TEST_MINOR" ] && { ! grep -q "FOXWIZARD PATCHLING v$MAJOR.$MINOR " ../index.html || ! grep -q "foxwizard-patchling-$MAJOR.$MINOR'" ../sw.js; }; then
  echo "Version mismatch: firmware is $MAJOR.$MINOR. Set the Patchling footer in index.html and CACHE_NAME in sw.js to $MAJOR.$MINOR." >&2
  exit 1
fi

rm -f foxwizard-firmware-*.bin foxwizard-firmware-*.uf2
cp "$BUILD/out/Mini_MIDI_Multitool.ino.bin" "$NAME.bin"
cp "$BUILD/out/Mini_MIDI_Multitool.ino.uf2" "$NAME.uf2"

SIZE=$(wc -c < "$NAME.bin" | tr -d ' ')
MD5=$(md5 -q "$NAME.bin" 2>/dev/null || md5sum "$NAME.bin" | cut -d' ' -f1)

cat > firmware.json <<JSON
{
  "major": $MAJOR,
  "minor": $MINOR,
  "bin": "$NAME.bin",
  "size": $SIZE,
  "md5": "$MD5"
}
JSON

[ -n "$TEST_MINOR" ] && echo "TEST BUILD. Run $0 with no options to publish the real version again."
echo "Published firmware $MAJOR.$MINOR: $NAME.bin ($SIZE bytes, MD5 $MD5) and $NAME.uf2"
