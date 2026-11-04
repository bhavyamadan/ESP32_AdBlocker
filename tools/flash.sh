#!/bin/sh
# Flash an ESP32_AdBlocker build made by tools/build.sh.
#
#   tools/flash.sh PORT [options]
#
# Options:
#   --encrypt   encrypt on the chip while writing; required when the module
#               has the flash-encryption eFuse enabled (SPI_BOOT_CRYPT_CNT set)
#   --usb       reset into download mode over USB-JTAG (packed board, no
#               BOOT/RST button access)
#   --manual    board is already in download mode, do not reset
#   --check     print chip and flash size, write nothing
#   --verify    read the flash back and compare with the image
#   --dir DIR   image directory (default: build/c3)
#
# ESP32-C3 notes:
#   * the serial port is the native USB (/dev/cu.usbmodem* on macOS, not tty)
#   * esptool v5 commands are hyphenated (write-flash, flash-id, ...)
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)

PORT=
MODE=auto
ENCRYPT=0
BUILD_DIR="$ROOT/build/c3"

while [ $# -gt 0 ]; do
  case "$1" in
    --encrypt) ENCRYPT=1 ;;
    --manual)  MODE=manual ;;
    --usb)     MODE=usb ;;
    --check)   MODE=check ;;
    --verify)  MODE=verify ;;
    --dir)     shift; BUILD_DIR=$1 ;;
    -*)        echo "unknown option '$1'" >&2; exit 2 ;;
    *)         if [ -z "$PORT" ]; then PORT=$1; else echo "unexpected arg '$1'" >&2; exit 2; fi ;;
  esac
  shift
done

find_esptool() {
  if [ -n "${ESPTOOL:-}" ] && [ -x "${ESPTOOL:-}" ]; then echo "$ESPTOOL"; return; fi
  if command -v esptool >/dev/null 2>&1; then echo esptool; return; fi
  if command -v esptool.py >/dev/null 2>&1; then echo esptool.py; return; fi
  for d in "$HOME/Library/Arduino15/packages/esp32/tools/esptool_py"/* \
           "$HOME/.arduino15/packages/esp32/tools/esptool_py"/*; do
    [ -x "$d/esptool" ] && { echo "$d/esptool"; return; }
  done
  echo "esptool not found - install with: python3 -m pip install esptool" >&2
  exit 1
}
ESPTOOL_BIN=$(find_esptool)

BAUD=${BAUD:-115200}

if [ -z "$PORT" ]; then
  echo "Usage: $0 PORT [--encrypt] [--usb|--manual|--check|--verify] [--dir DIR]"
  echo
  echo "USB CDC ports present:"
  PORTS=$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/ttyUSB* 2>/dev/null || true)
  if [ -n "$PORTS" ]; then echo "$PORTS"; else echo "  (none - is the board plugged in?)"; fi
  exit 2
fi

case "$PORT" in
  /dev/tty.usbmodem*)
    echo "Use the /dev/cu.* port, not /dev/tty.* - tty blocks on carrier detect." >&2
    echo "Try: /dev/cu.${PORT#/dev/tty.}" >&2
    exit 2
    ;;
esac

case "$MODE" in
  manual) BEFORE=no-reset ;;
  usb)    BEFORE=usb-reset ;;
  *)      BEFORE=default-reset ;;
esac

case "$MODE" in
  check)
    exec "$ESPTOOL_BIN" --chip esp32c3 -p "$PORT" -b "$BAUD" \
      --before "$BEFORE" --after no-reset flash-id
    ;;
  verify)
    exec "$ESPTOOL_BIN" --chip esp32c3 -p "$PORT" -b "$BAUD" \
      --before "$BEFORE" --after hard-reset \
      verify-flash 0x0 "$BUILD_DIR/merged.bin"
    ;;
  *)
    if [ "$ENCRYPT" = 1 ]; then
      # The stub flasher encrypts each block with the chip's eFuse key while
      # writing, so the plaintext build can go straight onto a
      # flash-encryption-enabled module (DIS_DOWNLOAD_MANUAL_ENCRYPT=0).
      exec "$ESPTOOL_BIN" --chip esp32c3 -p "$PORT" -b "$BAUD" \
        --before "$BEFORE" --after hard-reset \
        write-flash --encrypt \
          0x0     "$BUILD_DIR/bootloader.bin" \
          0x8000  "$BUILD_DIR/partitions.bin" \
          0xe000  "$BUILD_DIR/boot_app0.bin" \
          0x10000 "$BUILD_DIR/app.bin"
    else
      exec "$ESPTOOL_BIN" --chip esp32c3 -p "$PORT" -b "$BAUD" \
        --before "$BEFORE" --after hard-reset \
        write-flash 0x0 "$BUILD_DIR/merged.bin"
    fi
    ;;
esac
