#!/usr/bin/env bash
# flash.sh — build one sketch and upload it. No checks, no ceremony.
#
#   scripts/flash.sh jali_truchet.cpp     # a sketch file...
#   scripts/flash.sh jali_truchet         # ...with or without the extension
#   scripts/flash.sh jali_147             # or name the env outright
#
# The only thing it does beyond `pio run -t upload` is resolve a sketch name to
# its device env — the one per board whose name has no _native/_shot suffix —
# by finding which build_src_filter names that file. That is not a safety
# check — it is the whole reason you can type the sketch name. Everything else
# (port probing, building the SDL target first, reading back the log) is
# deliberately absent.
#
# With the sketch on more than one board there is one device env per board, so
# a bare sketch name goes ambiguous: name the env outright.
set -e
cd "$(dirname "$0")/.."

t=${1:?usage: scripts/flash.sh <sketch.cpp|env>}

if grep -q "^\[env:$t\]" platformio.ini; then
  env=$t
else
  env=$(awk -v want="$(basename "$t" .cpp).cpp" '
    /^\[env:/                { e = substr($0, 6, length($0) - 6) }
    /build_src_filter/       { if (index($0, "+<" want ">") && e !~ /_(native|shot)$/) print e }
  ' platformio.ini)
  case $env in *$'\n'*)
    printf 'ambiguous — %s is on more than one board, name the env:\n%s\n' "$t" "$env" >&2
    exit 1
  esac
fi

exec pio run -e "${env:?no device env builds $t}" -t upload
