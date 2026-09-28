#!/bin/sh
# Enable or remove the user-level RNNoise mic filter. Does not alter oFono.
set -eu

conf="$HOME/.config/pipewire/filter-chain.conf.d/99-iblueconnect-rnnoise.conf"
plugin="$HOME/.local/lib/iblueconnect/librnnoise_ladspa.so"

fail() { echo "$1" >&2; exit 1; }

safe_to_restart() {
    snapshot=$(busctl --user --json=short --timeout=5 call org.iblueconnect.Daemon \
        /org/iblueconnect/Daemon org.iblueconnect.Daemon Snapshot 2>/dev/null) ||
        fail 'Cannot verify call state.'
    printf '%s\n' "$snapshot" | jq -e '.type == "sssss" and
        (.data | type == "array" and length == 5 and .[0] == "ready" and .[2] == "idle")' >/dev/null ||
        fail 'Call is not confirmed ready and idle.'
    graph=$(pw-dump 2>/dev/null) || fail 'Cannot verify audio graph.'
    printf '%s\n' "$graph" | jq -e 'type == "array" and
        ([.[] | select(.type == "PipeWire:Interface:Node" and
        .info.props."api.bluez5.profile" == "headset-audio-gateway")] | length == 0)' >/dev/null ||
        fail 'Bluetooth call audio is active or graph is unavailable.'
}

filter_ready() {
    current_graph=$(pw-dump 2>/dev/null) || return 1
    printf '%s\n' "$current_graph" | jq -e '
      . as $g |
      [$g[] | select(.type == "PipeWire:Interface:Node" and .info.props."node.name" == "echo-cancel-source") | .id] as $virtual |
      [$g[] | select(.type == "PipeWire:Interface:Node" and .info.props."node.name" == "iblueconnect-rnnoise-capture") | .id] as $capture |
      [$g[] | select(.type == "PipeWire:Interface:Node" and .info.props."media.class" == "Audio/Source" and
        .info.props."node.name" != "echo-cancel-source" and .info.props."node.virtual" != true) | .id] as $physical |
      ($virtual | length) == 1 and ($capture | length) == 1 and
      any($g[]; .type == "PipeWire:Interface:Metadata" and .props."metadata.name" == "default" and
        any(.metadata[]?; .key == "default.audio.source" and .value.name == "echo-cancel-source")) and
      any($g[]; .type == "PipeWire:Interface:Link" and (.info.state == "active" or .info.state == "paused") and
        .info."input-node-id" == $capture[0] and (.info."output-node-id" as $id | $physical | index($id) != null))
    ' >/dev/null 2>&1
}

case ${1:-} in
    enable)
        [ "$#" -eq 2 ] || fail 'Usage: rnnoise.sh enable /path/to/librnnoise_ladspa.so'
        [ -s "$2" ] || fail 'RNNoise LADSPA plugin is missing.'
        [ ! -e "$conf" ] && [ ! -e "$plugin" ] || fail 'RNNoise is already installed; roll it back first.'
        systemctl --user is-enabled --quiet filter-chain.service >/dev/null 2>&1 ||
            fail 'Enable the user filter-chain service first: systemctl --user enable filter-chain.service'
        safe_to_restart
        printf '%s\n' "$graph" | jq -e '[.[] | select(.type == "PipeWire:Interface:Node" and
            .info.props."node.name" == "echo-cancel-source")] | length > 0' >/dev/null &&
            fail 'A temporary noise filter is still running; stop it first.'
        mkdir -p "$(dirname "$conf")" "$(dirname "$plugin")"
        cleanup_pending=1
        trap 'if [ "$cleanup_pending" -eq 1 ]; then rm -f "$conf" "$plugin"; fi' EXIT
        install -m 755 "$2" "$plugin"
        plugin_json=$(jq -n --arg path "$plugin" '$path')
        cat > "$conf" <<EOF
# IBlueConnect RNNoise; remove with packaging/rnnoise.sh rollback.
context.modules = [
  { name = libpipewire-module-filter-chain
    args = {
      node.description = "IBlueConnect RNNoise"
      filter.graph = { nodes = [
        { type = ladspa name = rnnoise plugin = $plugin_json
          label = noise_suppressor_mono
          control = { "VAD Threshold (%)" = 50.0 } }
      ] }
      audio.position = [ MONO ]
      capture.props = {
        node.name = "iblueconnect-rnnoise-capture"
        state.restore-target = false
        node.passive = true
        audio.rate = 48000
      }
      playback.props = {
        node.name = "echo-cancel-source"
        node.description = "IBlueConnect RNNoise Mic"
        media.class = Audio/Source
        audio.rate = 48000
      }
    }
  }
]
EOF
        safe_to_restart
        if ! systemctl --user restart filter-chain.service >/dev/null 2>&1; then
            rm -f "$conf" "$plugin"
            safe_to_restart && systemctl --user restart filter-chain.service >/dev/null 2>&1 || true
            fail 'RNNoise start failed; configuration was removed.'
        fi
        ready=0
        for attempt in 1 2 3 4 5; do
            if filter_ready; then ready=1; break; fi
            [ "$attempt" -eq 5 ] || sleep 1
        done
        if [ "$ready" -ne 1 ]; then
            rm -f "$conf" "$plugin"
            safe_to_restart && systemctl --user restart filter-chain.service >/dev/null 2>&1 || true
            fail 'RNNoise route was not verified; configuration was removed.'
        fi
        cleanup_pending=0
        trap - EXIT
        echo 'RNNoise enabled for future calls.'
        ;;
    rollback)
        [ "$#" -eq 1 ] || fail 'Usage: rnnoise.sh rollback'
        [ -f "$conf" ] && head -n 1 "$conf" | grep -Fqx '# IBlueConnect RNNoise; remove with packaging/rnnoise.sh rollback.' ||
            fail 'IBlueConnect RNNoise configuration is not installed.'
        safe_to_restart
        backup=$(mktemp "$conf.XXXXXX")
        mv "$conf" "$backup"
        if ! systemctl --user restart filter-chain.service >/dev/null 2>&1; then
            mv "$backup" "$conf"
            safe_to_restart && systemctl --user restart filter-chain.service >/dev/null 2>&1 || true
            fail 'Filter restart failed; RNNoise configuration restored.'
        fi
        rm -f "$backup" "$plugin"
        echo 'RNNoise removed; oFono unchanged.'
        ;;
    *) fail 'Usage: rnnoise.sh enable /path/to/librnnoise_ladspa.so | rollback' ;;
esac
