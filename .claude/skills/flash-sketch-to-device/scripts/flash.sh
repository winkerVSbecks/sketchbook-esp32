#!/usr/bin/env bash
#
# Flash a sketch from this repo to the Waveshare ESP32-S3-LCD-1.47B.
#
#   flash.sh                          list environments and exit
#   flash.sh arc_tiles_shake.cpp      resolve to that sketch's esp32 env, build + upload
#   flash.sh shake_esp32              use an environment name directly
#   flash.sh <target> --build         build only, don't touch the board
#
# Resolving the environment is the point of this script: `build_src_filter`
# means a wrong -e still builds and uploads happily, just with the wrong sketch.
# Everything here is derived from `pio project config`, never hardcoded.

set -euo pipefail

cd "$(git rev-parse --show-toplevel 2>/dev/null || echo "${0%/*}/../../../..")"
[ -f platformio.ini ] || { echo "error: no platformio.ini in $PWD" >&2; exit 1; }

TARGET="${1:-}"
BUILD_ONLY=0
[ "${2:-}" = "--build" ] && BUILD_ONLY=1

# env<TAB>platform<TAB>source-file, one line per environment
MAP=$(pio project config | awk '
  /^env:/                { env = substr($0, 5); next }
  env == ""              { next }
  /^ *platform  *=/      { plat[env] = $3 }
  /^ *build_src_filter/  { for (i = 1; i <= NF; i++)
                             if ($i ~ /^\+</) { f = $i; gsub(/^\+<|>$/, "", f); src[env] = f } }
  END { for (e in src) printf "%s\t%s\t%s\n", e, plat[e], src[e] }
' | sort)

if [ -z "$TARGET" ]; then
  printf 'environments:\n\n%-16s %-14s %s\n' ENV PLATFORM SOURCE
  echo "$MAP" | awk -F'\t' '{ printf "%-16s %-14s %s\n", $1, $2, $3 }'
  echo
  echo "usage: ${0##*/} <sketch.cpp|env> [--build]"
  exit 0
fi

# Exact environment name wins; otherwise resolve by source file among esp32 envs.
if echo "$MAP" | cut -f1 | grep -qx "$TARGET"; then
  ENV="$TARGET"
else
  # Match on the source basename, with or without a .cpp/.ino/.c extension.
  MATCHES=$(echo "$MAP" | awk -F'\t' -v t="${TARGET##*/}" '
    $2 == "espressif32" {
      s = $3;  sub(/\.(cpp|ino|c)$/, "", s)
      tt = t;  sub(/\.(cpp|ino|c)$/, "", tt)
      if (s == tt) print $1
    }')
  COUNT=$(echo "$MATCHES" | grep -c . || true)
  if [ "$COUNT" -eq 0 ]; then
    echo "error: no espressif32 environment builds '$TARGET'" >&2
    echo "known:" >&2; echo "$MAP" | awk -F'\t' '{ printf "  %-16s %-14s %s\n", $1, $2, $3 }' >&2
    exit 1
  elif [ "$COUNT" -gt 1 ]; then
    echo "error: '$TARGET' is built by more than one environment; name one explicitly:" >&2
    echo "$MATCHES" | sed 's/^/  /' >&2
    exit 1
  fi
  ENV="$MATCHES"
fi

SRC=$(echo "$MAP" | awk -F'\t' -v e="$ENV" '$1 == e { print $3 }')
echo "env: $ENV   source: src/$SRC"

if [ "$BUILD_ONLY" -eq 1 ]; then
  exec pio run -e "$ENV"
fi

PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
if [ -z "$PORT" ]; then
  echo "error: no /dev/cu.usbmodem* port found." >&2
  echo "       Hold BOOT, tap RESET, release BOOT to force download mode, then retry." >&2
  exit 1
fi
echo "port: $PORT"

pio run -e "$ENV" -t upload

cat <<EOF

Uploaded. A clean upload is not evidence the sketch works — check the board:

  pio device monitor -b 115200      (tap RESET to catch the boot lines)

For a sensor sketch, confirm the sensor was found before debugging behaviour:
  "QMI8658 at 0x6B"   sensor present; any problem is threshold or logic
  "QMI8658 not found" wiring/pins; tuning thresholds will not help
EOF
