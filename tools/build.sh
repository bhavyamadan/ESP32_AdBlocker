#!/bin/sh
# Build ESP32_AdBlocker with arduino-cli.
#
#   tools/build.sh c3      ESP32-C3, No OTA 2MB/2MB partition scheme (default)
#   tools/build.sh s3      ESP32-S3, 8MB with PSRAM
#   tools/build.sh esp32   Original ESP32, Minimal SPIFFS
#
# Output lands in build/<target>/ with canonical names (app.bin,
# bootloader.bin, partitions.bin, boot_app0.bin, merged.bin).
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TARGET=${1:-c3}

case "$TARGET" in
  c3)     FQBN="esp32:esp32:esp32c3:PartitionScheme=no_ota,CDCOnBoot=cdc" ;;
  c3uart) FQBN="esp32:esp32:esp32c3:PartitionScheme=no_ota" ;;
  s3)     FQBN="esp32:esp32:esp32s3:PartitionScheme=default_8MB,PSRAM=opi" ;;
  esp32)  FQBN="esp32:esp32:esp32:PartitionScheme=min_spiffs" ;;
  *) echo "unknown target '$TARGET' (c3, c3uart, s3, esp32)" >&2; exit 2 ;;
esac

if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "arduino-cli not found - install from https://arduino.github.io/arduino-cli/" >&2
  echo "then: arduino-cli core install esp32:esp32" >&2
  exit 1
fi

BUILD="$ROOT/build/$TARGET"
mkdir -p "$BUILD"

# arduino-cli requires the sketch folder to be named after the .ino, so build
# from a clean temporary copy rather than the (differently named) checkout
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/ESP32_AdBlocker"
for f in "$ROOT"/*.ino "$ROOT"/*.cpp "$ROOT"/*.h; do cp "$f" "$TMP/ESP32_AdBlocker/"; done
cp -R "$ROOT/data" "$TMP/ESP32_AdBlocker/"

arduino-cli compile --fqbn "$FQBN" --build-path "$TMP/out" "$TMP/ESP32_AdBlocker"

cp "$TMP/out/ESP32_AdBlocker.ino.bin"            "$BUILD/app.bin"
cp "$TMP/out/ESP32_AdBlocker.ino.bootloader.bin" "$BUILD/bootloader.bin"
cp "$TMP/out/ESP32_AdBlocker.ino.partitions.bin" "$BUILD/partitions.bin"
cp "$TMP/out/boot_app0.bin"                      "$BUILD/boot_app0.bin"
cp "$TMP/out/ESP32_AdBlocker.ino.merged.bin"     "$BUILD/merged.bin"

echo
echo "Built $FQBN -> $BUILD"
ls -l "$BUILD"
