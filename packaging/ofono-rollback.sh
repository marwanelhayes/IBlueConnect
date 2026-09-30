#!/bin/sh
# Restore PipeWire's native HFP backend (the original iPhone -> ASUS route).
set -eu

fail() { echo "$1" >&2; exit 1; }

safe_to_restart() {
    snapshot=$(busctl --user --json=short --timeout=5 call org.iblueconnect.Daemon \
        /org/iblueconnect/Daemon org.iblueconnect.Daemon Snapshot 2>/dev/null) ||
        fail 'Cannot verify call state.'
    printf '%s\n' "$snapshot" | jq -es 'length == 1 and (.[0] |
        .type == "sssss" and (.data | type == "array" and length == 5 and
        all(.[]; type == "string") and (.[0] == "ready" or .[0] == "calling unavailable") and
        .[2] == "idle"))' >/dev/null 2>&1 ||
        fail 'Call is not confirmed idle.'
    graph=$(pw-dump 2>/dev/null) || fail 'Cannot verify audio graph.'
    printf '%s\n' "$graph" | jq -es 'length == 1 and (.[0] | type == "array" and
        ([.[] | select(.type == "PipeWire:Interface:Node" and
        .info.props."api.bluez5.profile" == "headset-audio-gateway")] | length == 0))' >/dev/null 2>&1 ||
        fail 'Bluetooth call audio is active or graph is unavailable.'
}

safe_to_restart

rm -f "$HOME/.config/wireplumber/bluetooth.lua.d/51-iblueconnect-ofono.lua"
rm -f "$HOME/.config/wireplumber/wireplumber.conf.d/51-iblueconnect-ofono.conf"
safe_to_restart
sudo systemctl disable --now ofono.service || true
safe_to_restart
sudo apt-get purge -y ofono
safe_to_restart
systemctl --user restart wireplumber
safe_to_restart
systemctl --user stop iblueconnectd.service

echo 'Native Bluetooth calls restored. If the iPhone disconnected, reconnect it from Bluetooth settings.'
