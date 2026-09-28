#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
printf 'wrong source\n' > "$scratch/archive.tar.gz"

if sh "$root/packaging/build-rnnoise.sh" "$scratch/archive.tar.gz" >"$scratch/out" 2>"$scratch/err"; then
    echo 'Unverified RNNoise source was accepted.' >&2
    exit 1
fi
grep -q 'checksum' "$scratch/err"
echo 'RNNoise source checksum guard passed.'
