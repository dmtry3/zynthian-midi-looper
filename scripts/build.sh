#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build/midi_looper.lv2"
mkdir -p "$B"
cp "$ROOT/lv2/manifest.ttl" "$ROOT/lv2/midi_looper.ttl" "$B/"
gcc -O2 -fPIC -shared -Wall $(pkg-config --cflags lv2) \
    -o "$B/midi_looper.so" "$ROOT/src/midi_looper.c" -lm -pthread
echo "Built: $B"
