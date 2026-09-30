#!/bin/sh
# Run inside the target Ubuntu environment. Builds/tests only; never installs the app.
set -eu
[ "$#" -eq 1 ] || { echo 'Usage: build-release.sh 24.04|26.04' >&2; exit 1; }
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
. /etc/os-release
[ "$ID" = ubuntu ] && [ "$VERSION_ID" = "$1" ] || {
    echo 'Use a matching Ubuntu build environment.' >&2; exit 1;
}
python3 -m venv .venv
cargo build --locked --release --bin iblueconnectd
cargo test --locked
GTK_A11Y=none dbus-run-session sh packaging/test-mock-ui.sh
.venv/bin/python -B packaging/test_ofono.py
.venv/bin/python -B packaging/test_rnnoise.py
.venv/bin/python -B packaging/test_setup.py
.venv/bin/python -B packaging/test_deb.py
mkdir -p target/releases
archive=target/releases/noise-suppression-for-voice-v1.10.tar.gz
if [ ! -f "$archive" ]; then
    curl -fsSL --retry 2 --max-time 180 \
        https://github.com/werman/noise-suppression-for-voice/archive/refs/tags/v1.10.tar.gz \
        -o "$archive"
fi
sh packaging/build-rnnoise.sh "$archive"
sh packaging/build-deb.sh "$1" target/release/iblueconnectd target/iblueconnect-mock \
    target/rnnoise/build/bin/ladspa/librnnoise_ladspa.so
