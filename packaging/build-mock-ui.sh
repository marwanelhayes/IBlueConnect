#!/bin/sh
# Build the standalone UI-only GNOME prototype. No daemon or audio service is touched.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/target"
cc -std=c11 -Wall -Wextra -g \
    "$root/src/mock-ui/app.c" \
    "$root/src/mock-ui/mock-state.c" \
    "$root/src/mock-ui/main-view.c" \
    "$root/src/mock-ui/call-popup.c" \
    -o "$root/target/iblueconnect-mock" \
    $(pkg-config --cflags --libs gtk4 gtk4-x11 libadwaita-1 x11)
printf '%s\n' "$root/target/iblueconnect-mock"
