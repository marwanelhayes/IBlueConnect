#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
rust_bin="$root/.tools/rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin"
if [ -x "$rust_bin/cargo" ]; then
    cargo="$rust_bin/cargo"
    export RUSTC="$rust_bin/rustc" RUSTDOC="$rust_bin/rustdoc"
else
    cargo=${CARGO:-cargo}
fi

CARGO_TARGET_DIR="$root/target" "$cargo" build --offline --release --manifest-path "$root/Cargo.toml" --bin iblueconnectd
mkdir -p "$root/target"
cc "$root/src/iblueconnect-ui.c" -o "$root/target/iblueconnect" $(pkg-config --cflags --libs gtk4 gtk4-x11 libadwaita-1 x11)

install -Dm755 "$root/target/release/iblueconnectd" "$HOME/.local/bin/iblueconnectd"
install -Dm755 "$root/target/iblueconnect" "$HOME/.local/bin/iblueconnect"
install -Dm644 "$root/packaging/systemd/iblueconnectd.service" "$HOME/.config/systemd/user/iblueconnectd.service"
install -Dm644 "$root/packaging/desktop/org.iblueconnect.IBlueConnect.desktop" "$HOME/.local/share/applications/org.iblueconnect.IBlueConnect.desktop"
systemctl --user daemon-reload
systemctl --user enable iblueconnectd.service

echo 'Installed. The daemon starts at the next login; launch the GUI from the applications menu.'
