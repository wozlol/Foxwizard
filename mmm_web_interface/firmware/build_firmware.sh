#!/bin/sh
# Builds FoxWizard firmware and publishes it for Patchling's firmware check: a .bin (sent over
# MIDI), a .uf2 (for the FIRMWARE pads), and firmware.json (version, size, MD5). Also builds the
# two modules' .bin files (the USB host module and the Bluetooth module), which Patchling sends
# through the FoxWizard to update them, and lists them in firmware.json under "host" and
# "bluetooth". Their versions are HOST_FW_VERSION and BT_FW_VERSION in their sketches. For flashing a module by
# hand it also publishes foxwizard-host-N.uf2 (drag onto the RPI-RP2 drive) and foxwizard-bluetooth-N-full.bin
# (the whole flash, written at address 0). Upload those to the same page as the main .uf2.
#
# ./build_firmware.sh                  live firmware, ../../Mini_MIDI_Multitool, into firmware/.
#                                      Upload the .uf2 to woz.lol/foxwizard-midi-multitool-firmware.
# ./build_firmware.sh --test-channel   the same firmware as a test build into firmware/test/. Only
#                                      Patchling served locally (serve.sh) or opened with ?test offers it.
# ./build_firmware.sh --test MINOR     the live firmware relabeled MAJOR.MINOR, for testing the update
#                                      itself. The source is not changed. Rerun with no options after.
#                                      The modules are not rebuilt, their entries stay as they were.
#
# Patchling and the firmware always share one version number. Live and test channel builds refuse
# to publish unless the Patchling footer (index.html) and cache name (sw.js) match.
set -e
cd "$(dirname "$0")"
FQBN=rp2040:rp2040:waveshare_rp2040_zero:usbstack=tinyusb,flash=2097152_1048576
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

SKETCH=../../Mini_MIDI_Multitool
SKETCH_NAME=Mini_MIDI_Multitool
HOST_SKETCH="../../Foxwizard Host Edition/Foxwizard_Host_Edition_Sidecar"
HOST_FQBN=rp2040:rp2040:waveshare_rp2040_zero:usbstack=tinyusb_host,flash=2097152_1048576
BT_SKETCH="../../Foxwizard Host Edition/Foxwizard_Bluetooth_Sidecar"
BT_FQBN=esp32:esp32:esp32s3:FlashSize=4M,PSRAM=disabled,CDCOnBoot=default,USBMode=default,PartitionScheme=default,CPUFreq=240,DebugLevel=none
OUT=.
PREFIX=foxwizard-firmware
TEST_MINOR=""

case "$1" in
  --test-channel)
    OUT=test
    ;;
  --test)
    TEST_MINOR="$2"
    case "$TEST_MINOR" in ''|*[!0-9]*) echo "Usage: $0 --test MINOR" >&2; exit 1 ;; esac
    # Build a copy with the test version, leaving the source alone.
    cp -R "$SKETCH" "$BUILD/$SKETCH_NAME"
    sed "s/^constexpr uint8_t FW_VERSION_MINOR = [0-9]*;/constexpr uint8_t FW_VERSION_MINOR = $TEST_MINOR;/" "$SKETCH/$SKETCH_NAME.ino" > "$BUILD/$SKETCH_NAME/$SKETCH_NAME.ino"
    SKETCH="$BUILD/$SKETCH_NAME"
    ;;
  "") ;;
  *) echo "Usage: $0 [--test-channel | --test MINOR]" >&2; exit 1 ;;
esac

INO="$SKETCH/$SKETCH_NAME.ino"
MAJOR=$(sed -n 's/^constexpr uint8_t FW_VERSION_MAJOR = \([0-9]*\);.*/\1/p' "$INO")
MINOR=$(sed -n 's/^constexpr uint8_t FW_VERSION_MINOR = \([0-9]*\);.*/\1/p' "$INO")
if [ "$MAJOR" -gt 127 ] || [ "$MINOR" -gt 127 ]; then
  echo "Version $MAJOR.$MINOR does not fit: each number goes out as a 7-bit SysEx data byte, 127 at most. Move the major number up and the minor back to 0." >&2
  exit 1
fi
NAME="$PREFIX-$MAJOR.$MINOR"

if [ -z "$TEST_MINOR" ] && { ! grep -q "FOXWIZARD PATCHLING v$MAJOR.$MINOR " ../index.html || ! grep -q "foxwizard-patchling-$MAJOR.$MINOR'" ../sw.js; }; then
  echo "Version mismatch: firmware is $MAJOR.$MINOR. Set the Patchling footer in index.html and CACHE_NAME in sw.js to $MAJOR.$MINOR." >&2
  exit 1
fi

# FW_EXTRA_FLAGS adds compiler flags, e.g. FW_EXTRA_FLAGS=-DFW_DIAG for the stack dive command. Not for a release build.
if [ -n "$FW_EXTRA_FLAGS" ]; then
  arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD/out" --build-property "compiler.cpp.extra_flags=$FW_EXTRA_FLAGS" "$SKETCH"
else
  arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD/out" "$SKETCH"
fi

mkdir -p "$OUT"
rm -f "$OUT/$PREFIX"-[0-9]*.bin "$OUT/$PREFIX"-*.uf2
cp "$BUILD/out/$SKETCH_NAME.ino.bin" "$OUT/$NAME.bin"
cp "$BUILD/out/$SKETCH_NAME.ino.uf2" "$OUT/$NAME.uf2"

md5_of() { md5 -q "$1" 2>/dev/null || md5sum "$1" | cut -d' ' -f1; }
SIZE=$(wc -c < "$OUT/$NAME.bin" | tr -d ' ')
MD5=$(md5_of "$OUT/$NAME.bin")

# Builds one module and sets MODULE_JSON to its firmware.json entry. $1 name, $2 fqbn, $3 sketch folder, $4 sketch file, $5 version constant
build_module() {
  MOD_NAME="$1"; MOD_VERSION=$(sed -n "s/^constexpr uint8_t $5 = \([0-9]*\);.*/\1/p" "$3/$4.ino")
  [ -n "$MOD_VERSION" ] || { echo "No $5 in $3/$4.ino" >&2; exit 1; }
  [ "$MOD_VERSION" -le 127 ] || { echo "$5 $MOD_VERSION does not fit a 7-bit SysEx data byte." >&2; exit 1; }
  arduino-cli compile --fqbn "$2" --build-path "$BUILD/$MOD_NAME" "$3" > "$BUILD/$MOD_NAME.log" 2>&1 || { cat "$BUILD/$MOD_NAME.log" >&2; exit 1; }
  MOD_BIN="foxwizard-$MOD_NAME-$MOD_VERSION.bin"
  rm -f "$OUT/foxwizard-$MOD_NAME"-*.bin
  cp "$BUILD/$MOD_NAME/$4.ino.bin" "$OUT/$MOD_BIN"
  if [ "$MOD_NAME" = host ]; then
    rm -f "$OUT"/foxwizard-host-*.uf2
    cp "$BUILD/$MOD_NAME/$4.ino.uf2" "$OUT/foxwizard-host-$MOD_VERSION.uf2"
  else
    cp "$BUILD/$MOD_NAME/$4.ino.merged.bin" "$OUT/foxwizard-$MOD_NAME-$MOD_VERSION-full.bin"
  fi
  MODULE_JSON="{\"version\": $MOD_VERSION, \"bin\": \"$MOD_BIN\", \"size\": $(wc -c < "$OUT/$MOD_BIN" | tr -d ' '), \"md5\": \"$(md5_of "$OUT/$MOD_BIN")\"}"
}

HOST_JSON=""; BT_JSON=""
if [ -z "$TEST_MINOR" ]; then
  build_module host "$HOST_FQBN" "$HOST_SKETCH" Foxwizard_Host_Edition_Sidecar HOST_FW_VERSION; HOST_JSON="$MODULE_JSON"
  build_module bluetooth "$BT_FQBN" "$BT_SKETCH" Foxwizard_Bluetooth_Sidecar BT_FW_VERSION; BT_JSON="$MODULE_JSON"
fi

# --test leaves the module entries as the last full run wrote them.
python3 - "$OUT/firmware.json" "$MAJOR" "$MINOR" "$NAME.bin" "$SIZE" "$MD5" "$HOST_JSON" "$BT_JSON" <<'PY'
import json, sys
path, major, minor, binname, size, md5, host, bt = sys.argv[1:]
try:
    old = json.load(open(path))
except Exception:
    old = {}
out = {"major": int(major), "minor": int(minor), "bin": binname, "size": int(size), "md5": md5}
for key, value in (("host", host), ("bluetooth", bt)):
    if value: out[key] = json.loads(value)
    elif key in old: out[key] = old[key]
json.dump(out, open(path, "w"), indent=2)
open(path, "a").write("\n")
PY

[ -n "$TEST_MINOR" ] && echo "TEST BUILD. Run $0 with no options to publish the real version again."
echo "Published firmware $MAJOR.$MINOR into $OUT/: $NAME.bin ($SIZE bytes, MD5 $MD5) and $NAME.uf2"
[ -n "$HOST_JSON" ] && echo "USB host module: $HOST_JSON"
[ -n "$BT_JSON" ] && echo "Bluetooth module: $BT_JSON"
[ -n "$HOST_JSON" ] && echo "Files for flashing a module by hand: $(cd "$OUT" && ls foxwizard-host-*.uf2 foxwizard-bluetooth-*-full.bin | tr '\n' ' ')"
