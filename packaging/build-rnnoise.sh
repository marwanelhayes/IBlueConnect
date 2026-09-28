#!/bin/sh
# Build the RNNoise LADSPA plugin from pinned upstream source; never change audio services.
set -eu

[ "$#" -le 1 ] || { echo 'Usage: build-rnnoise.sh [v1.10 source archive]' >&2; exit 1; }
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
archive=${1:-}
if [ -z "$archive" ]; then
    archive=$(mktemp)
    trap 'rm -f "$archive"' EXIT
    curl -fsSL --retry 2 --max-time 180 \
        https://github.com/werman/noise-suppression-for-voice/archive/refs/tags/v1.10.tar.gz \
        -o "$archive"
fi
printf '%s  %s\n' \
    '6e0c11aeb8392891750b0243c2ba695dab07654bf3f4e01adbed927b36cc690a' "$archive" |
    sha256sum -c - >/dev/null || { echo 'RNNoise source checksum failed.' >&2; exit 1; }

build="$root/target/rnnoise"
mkdir -p "$build"
tar -xzf "$archive" -C "$build"
cmake -S "$build/noise-suppression-for-voice-1.10" -B "$build/build" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF \
    -DBUILD_VST_PLUGIN=OFF -DBUILD_VST3_PLUGIN=OFF -DBUILD_LV2_PLUGIN=OFF \
    -DBUILD_AU_PLUGIN=OFF -DBUILD_AUV3_PLUGIN=OFF
cmake --build "$build/build" --target rnnoise_ladspa -j 2
plugin="$build/build/bin/ladspa/librnnoise_ladspa.so"
[ -s "$plugin" ] || { echo 'RNNoise LADSPA build produced no plugin.' >&2; exit 1; }
printf '%s\n' "$plugin"
