#!/usr/bin/env bash
# flash.sh — build one sketch and upload it. No checks, no ceremony.
#
#   scripts/flash.sh jali_truchet.cpp     # a sketch file...
#   scripts/flash.sh jali_truchet         # ...with or without the extension
#   scripts/flash.sh jali_esp32           # or name the env outright
#
# The only thing it does beyond `pio run -t upload` is resolve a sketch name to
# its *_esp32 env, by finding which build_src_filter names that file. That is
# not a safety check — it is the whole reason you can type the sketch name.
# Everything else (port probing, building the SDL target first, reading back
# the log) is deliberately absent.
set -e
cd "$(dirname "$0")/.."

t=${1:?usage: scripts/flash.sh <sketch.cpp|env>}

case $t in
  *esp32)
    env=$t
    ;;
  *)
    env=$(awk -v want="$(basename "$t" .cpp).cpp" '
      /^\[env:/                { e = substr($0, 6, length($0) - 6) }
      /build_src_filter/       { if (index($0, "+<" want ">") && e ~ /esp32$/) { print e; exit } }
    ' platformio.ini)
    ;;
esac

exec pio run -e "${env:?no esp32 env builds $t}" -t upload
