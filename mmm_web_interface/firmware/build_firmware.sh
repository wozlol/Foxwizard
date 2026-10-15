#!/bin/sh
# Builds FoxWizard firmware and publishes it for Patchling's firmware check: a .bin (sent over
# MIDI), a .uf2 (for the FIRMWARE pads), and firmware.json (version, size, MD5).
#
# ./build_firmware.sh                  live firmware, ../../Mini_MIDI_Multitool, into firmware/.
#                                      Upload the .uf2 to woz.lol/foxwizard-midi-multitool-firmware.
# ./build_firmware.sh --host-edition   test build of the Host Edition main brain into firmware/test/.
#                                      Only Patchling served locally (serve.sh) or opened with ?test
#                                      offers it.
# ./build_firmware.sh --test MINOR     the live firmware relabeled MAJOR.MINOR, for testing the update
#                                      itself. The source is not changed. Rerun with no options after.
#
# Patchling and the firmware always share one version number. Live and Host Edition builds refuse
# to publish unless the Patchling footer (index.html) and cache name (sw.js) match.
set -e
cd "$(dirname "$0")"
FQBN=rp2040:rp2040:waveshare_rp2040_zero:usbstack=tinyusb,flash=2097152_262144
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

SKETCH=../../Mini_MIDI_Multitool
SKETCH_NAME=Mini_MIDI_Multitool
OUT=.
PREFIX=foxwizard-firmware
TEST_MINOR=""

case "$1" in
  --host-edition)
    SKETCH="../../Foxwizard Host Edition/Mini_MIDI_Multitool_Host_Edition"
    SKETCH_NAME=Mini_MIDI_Multitool_Host_Edition
    OUT=test
    PREFIX=foxwizard-host-edition
    ;;
  --test)
    TEST_MINOR="$2"
    case "$TEST_MINOR" in ''|*[!0-9]*) echo "Usage: $0 --test MINOR" >&2; exit 1 ;; esac
    # Build a copy with the test version, leaving the source alone.
    mkdir "$BUILD/$SKETCH_NAME"
    sed "s/^constexpr uint8_t FW_VERSION_MINOR = [0-9]*;/constexpr uint8_t FW_VERSION_MINOR = $TEST_MINOR;/" "$SKETCH/$SKETCH_NAME.ino" > "$BUILD/$SKETCH_NAME/$SKETCH_NAME.ino"
    SKETCH="$BUILD/$SKETCH_NAME"
    ;;
  "") ;;
  *) echo "Usage: $0 [--host-edition | --test MINOR]" >&2; exit 1 ;;
esac

INO="$SKETCH/$SKETCH_NAME.ino"
MAJOR=$(sed -n 's/^constexpr uint8_t FW_VERSION_MAJOR = \([0-9]*\);.*/\1/p' "$INO")
MINOR=$(sed -n 's/^constexpr uint8_t FW_VERSION_MINOR = \([0-9]*\);.*/\1/p' "$INO")
NAME="$PREFIX-$MAJOR.$MINOR"

if [ -z "$TEST_MINOR" ] && { ! grep -q "FOXWIZARD PATCHLING v$MAJOR.$MINOR " ../index.html || ! grep -q "foxwizard-patchling-$MAJOR.$MINOR'" ../sw.js; }; then
  echo "Version mismatch: firmware is $MAJOR.$MINOR. Set the Patchling footer in index.html and CACHE_NAME in sw.js to $MAJOR.$MINOR." >&2
  exit 1
fi

arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD/out" "$SKETCH"

mkdir -p "$OUT"
rm -f "$OUT/$PREFIX"-*.bin "$OUT/$PREFIX"-*.uf2
cp "$BUILD/out/$SKETCH_NAME.ino.bin" "$OUT/$NAME.bin"
cp "$BUILD/out/$SKETCH_NAME.ino.uf2" "$OUT/$NAME.uf2"

SIZE=$(wc -c < "$OUT/$NAME.bin" | tr -d ' ')
MD5=$(md5 -q "$OUT/$NAME.bin" 2>/dev/null || md5sum "$OUT/$NAME.bin" | cut -d' ' -f1)

cat > "$OUT/firmware.json" <<JSON
{
  "major": $MAJOR,
  "minor": $MINOR,
  "bin": "$NAME.bin",
  "size": $SIZE,
  "md5": "$MD5"
}
JSON

[ -n "$TEST_MINOR" ] && echo "TEST BUILD. Run $0 with no options to publish the real version again."
echo "Published firmware $MAJOR.$MINOR into $OUT/: $NAME.bin ($SIZE bytes, MD5 $MD5) and $NAME.uf2"
