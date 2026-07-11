#!/usr/bin/env bash
# Build and run ChordLoop's host-side unit tests (no hardware, no SDK needed).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
fw="$here/../firmware"
out="${TMPDIR:-/tmp}/chordloop_tests"

g++ -std=c++17 -Wall -Wextra -I "$fw" \
    "$here/test_all.cpp" \
    "$fw/chord_engine.cpp" "$fw/music_tables.cpp" "$fw/arp.cpp" \
    "$fw/persist.cpp" "$fw/serial_proto.cpp" \
    -o "$out"

"$out"
