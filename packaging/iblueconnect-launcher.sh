#!/bin/sh
set -eu
libdir=$(CDPATH= cd -- "$(dirname -- "$0")/../lib/iblueconnect" && pwd)
. /etc/os-release
expected=$(cat "$libdir/ubuntu-version")
if [ "${ID:-}" != ubuntu ] || [ "${VERSION_ID:-}" != "$expected" ]; then
    zenity --error --title='IBlueConnect package mismatch' --text="Install the IBlueConnect package for your Ubuntu version. This package is for Ubuntu $expected."
    exit 1
fi
if ! sh "$libdir/first-run.sh"; then
    exit 1
fi
exec "$libdir/iblueconnect" "$@"
