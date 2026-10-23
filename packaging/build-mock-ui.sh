#!/bin/sh
# Build the GNOME UI. It follows the daemon by default; --mock/--developer/--preview use mock data.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/target"
cc -std=c11 -Wall -Wextra -g \
    "$root/src/mock-ui/app.c" \
    "$root/src/mock-ui/mock-state.c" \
    "$root/src/mock-ui/main-view.c" \
    "$root/src/mock-ui/call-popup.c" \
    "$root/src/mock-ui/live.c" \
    "$root/src/mock-ui/privacy.c" \
    "$root/src/mock-ui/appearance.c" \
    -o "$root/target/iblueconnect-mock" \
    $(pkg-config --cflags --libs gtk4 gtk4-x11 libadwaita-1 x11) -lm
printf '%s\n' "$root/target/iblueconnect-mock"
