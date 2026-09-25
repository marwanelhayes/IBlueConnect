#!/bin/sh
# Hand HFP call handling from PipeWire's native backend to oFono, persistently.
# Undo with packaging/ofono-rollback.sh. Never connects Bluetooth: reconnect the iPhone yourself.
set -eu

conf="$HOME/.config/wireplumber/bluetooth.lua.d/51-iblueconnect-ofono.lua"

if pw-dump 2>/dev/null | grep -q '"headset-audio-gateway"'; then
    echo 'A Bluetooth call is active; run this after it ends.' >&2
    exit 1
fi

sudo apt-get install -y ofono
# The package also starts dundee (dial-up networking), which IBlueConnect does not use.
sudo systemctl disable --now dundee.service

mkdir -p "$(dirname "$conf")"
printf '%s\n' '-- IBlueConnect: oFono owns HFP calls. Remove this file to restore the native backend.' \
    'bluez_monitor.properties["bluez5.hfphsp-backend"] = "ofono"' > "$conf"

# WirePlumber must release native HFP before oFono registers the profile with BlueZ.
systemctl --user restart wireplumber
sudo systemctl enable ofono.service
sudo systemctl restart ofono.service
systemctl --user restart iblueconnectd.service

echo 'oFono enabled. If the iPhone disconnected, reconnect it from Bluetooth settings.'
