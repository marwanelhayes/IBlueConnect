#!/bin/sh
# Hand HFP call handling from PipeWire's native backend to oFono, persistently.
# Undo with packaging/ofono-rollback.sh. Never connects Bluetooth: reconnect the iPhone yourself.
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

legacy="$HOME/.config/wireplumber/bluetooth.lua.d/51-iblueconnect-ofono.lua"
modern="$HOME/.config/wireplumber/wireplumber.conf.d/51-iblueconnect-ofono.conf"
version=$(wireplumber --version | sed -n 's/.*wireplumber \([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' | head -n 1)
case "$version" in
    0.4.*) conf="$legacy"; obsolete="$modern" ;;
    0.[5-9].*|0.[1-9][0-9]*.*|[1-9]*.*) conf="$modern"; obsolete="$legacy" ;;
    *) echo 'Unsupported WirePlumber version.' >&2; exit 1 ;;
esac

print_config() {
if [ "$conf" = "$legacy" ]; then
    printf '%s\n' '-- IBlueConnect: oFono owns HFP calls. Remove this file to restore the native backend.' \
        'bluez_monitor.properties["bluez5.hfphsp-backend"] = "ofono"'
    cat <<'EOF'
-- Use RNNoise only for outgoing phone audio; a missing filter falls back to the usual mic.
table.insert(bluez_monitor.rules, {
  matches = {
    {
      { "api.bluez5.profile", "equals", "headset-audio-gateway" },
      { "factory.name", "equals", "api.bluez5.sco.sink" },
    },
  },
  apply_properties = {
    ["target.object"] = "echo-cancel-source",
  },
})
EOF
else
    printf '%s\n' '# IBlueConnect: oFono owns HFP calls. Remove this file to restore the native backend.' \
        'monitor.bluez.properties = {' '  bluez5.hfphsp-backend = "ofono"' '}'
    cat <<'EOF'
# Use RNNoise only for outgoing phone audio; a missing filter falls back to the usual mic.
monitor.bluez.rules = [
  {
    matches = [
      {
        api.bluez5.profile = "headset-audio-gateway"
        factory.name = "api.bluez5.sco.sink"
      }
    ]
    actions = {
      update-props = {
        target.object = "echo-cancel-source"
      }
    }
  }
]
EOF
fi
}

case ${1:-} in
    --print-config)
        [ "$#" -eq 1 ] || fail "Usage: ofono-enable.sh [--print-config]"
        print_config
        exit 0
        ;;
    "") [ "$#" -eq 0 ] || fail "Usage: ofono-enable.sh [--print-config]" ;;
    *) fail "Usage: ofono-enable.sh [--print-config]" ;;
esac

safe_to_restart

if [ "$(dpkg-query -W -f='${Status}' ofono 2>/dev/null || true)" != 'install ok installed' ]; then
    safe_to_restart
    sudo apt-get install -y ofono
fi
# The package also starts dundee (dial-up networking), which IBlueConnect does not use.
safe_to_restart
sudo systemctl disable --now dundee.service

mkdir -p "$(dirname "$conf")"
print_config > "$conf"
if [ -f "$obsolete" ] && head -n 1 "$obsolete" | grep -Eq \
    '^(--|#) IBlueConnect: oFono owns HFP calls\. Remove this file to restore the native backend\.$'; then
    rm -f "$obsolete"
fi

# WirePlumber must release native HFP before oFono registers the profile with BlueZ.
safe_to_restart
systemctl --user restart wireplumber
safe_to_restart
sudo systemctl enable ofono.service
safe_to_restart
sudo systemctl restart ofono.service
safe_to_restart
systemctl --user restart iblueconnectd.service

echo 'oFono enabled. If the iPhone disconnected, reconnect it from Bluetooth settings.'
