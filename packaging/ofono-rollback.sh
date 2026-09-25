#!/bin/sh
# Restore PipeWire's native HFP backend (the original iPhone -> ASUS route).
set -eu

systemctl --user stop iblueconnectd.service || true
rm -f "$HOME/.config/wireplumber/bluetooth.lua.d/51-iblueconnect-ofono.lua"
sudo systemctl disable --now ofono.service || true
sudo apt-get purge -y ofono
systemctl --user restart wireplumber

echo 'Native Bluetooth calls restored. If the iPhone disconnected, reconnect it from Bluetooth settings.'
