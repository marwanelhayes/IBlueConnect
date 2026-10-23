#!/bin/sh
# Public CLI smoke test: exercise the mock incoming-call journey headlessly.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
sh "$root/packaging/build-mock-ui.sh" >/dev/null
cc -std=c11 -Wall -Wextra -Werror \
    "$root/src/mock-ui/test-mock-state.c" "$root/src/mock-ui/mock-state.c" \
    -o "$root/target/test-mock-state"
"$root/target/test-mock-state"
cc -std=c11 -Wall -Wextra -Werror -Wno-error=deprecated-declarations \
    "$root/src/mock-ui/test-main-view.c" "$root/src/mock-ui/main-view.c" \
    "$root/src/mock-ui/mock-state.c" -o "$root/target/test-main-view" \
    $(pkg-config --cflags --libs gtk4 libadwaita-1)
cc -std=c11 -Wall -Wextra -Werror -Wno-error=deprecated-declarations \
    "$root/src/mock-ui/test-call-popup.c" "$root/src/mock-ui/call-popup.c" \
    "$root/src/mock-ui/mock-state.c" -o "$root/target/test-call-popup" \
    $(pkg-config --cflags --libs gtk4 gtk4-x11 libadwaita-1 x11) -lm
GDK_BACKEND=x11 GSK_RENDERER=cairo GSETTINGS_BACKEND=memory \
GTK_USE_PORTAL=0 GIO_USE_VFS=local NO_AT_BRIDGE=1 \
    xvfb-run -a sh -c '
        set -eu
        timeout 20 "$1" --self-test
        timeout 20 "$2"
        timeout 20 "$3"
    ' sh "$root/target/iblueconnect-mock" "$root/target/test-main-view" "$root/target/test-call-popup"
cc -std=c11 -Wall -Wextra -Werror \
    "$root/src/mock-ui/test-live.c" "$root/src/mock-ui/live.c" "$root/src/mock-ui/mock-state.c" \
    -o "$root/target/test-live" $(pkg-config --cflags --libs gio-2.0) -lm
"$root/target/test-live"
cc -std=c11 -Wall -Wextra -Werror \
    "$root/src/mock-ui/test-privacy.c" "$root/src/mock-ui/privacy.c" \
    -o "$root/target/test-privacy" $(pkg-config --cflags --libs gio-2.0)
timeout 20 "$root/target/test-privacy"
cc -std=c11 -Wall -Wextra -Werror -Wno-error=deprecated-declarations \
    "$root/src/mock-ui/test-app.c" "$root/src/mock-ui/mock-state.c" \
    "$root/src/mock-ui/main-view.c" "$root/src/mock-ui/call-popup.c" \
    "$root/src/mock-ui/live.c" "$root/src/mock-ui/privacy.c" \
    -o "$root/target/test-app" $(pkg-config --cflags --libs gtk4 gtk4-x11 libadwaita-1 x11) -lm
GDK_BACKEND=x11 GSK_RENDERER=cairo GSETTINGS_BACKEND=memory \
GTK_USE_PORTAL=0 GIO_USE_VFS=local NO_AT_BRIDGE=1 \
    xvfb-run -a timeout 25 "$root/target/test-app"
