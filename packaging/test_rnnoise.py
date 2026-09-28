import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("rnnoise.sh")


class RnnoiseScriptTest(unittest.TestCase):
    def test_enable_rollback_and_call_guards(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            for name, variable in (("busctl", "TEST_SNAPSHOT"), ("pw-dump", "TEST_GRAPH")):
                command = bin_dir / name
                command.write_text(f'#!/bin/sh\nexec /bin/cat "${variable}"\n')
                command.chmod(0o755)
            systemctl = bin_dir / "systemctl"
            systemctl.write_text('''#!/bin/sh
[ "$2" = is-enabled ] && exit 0
printf "%s\\n" "$*" >> "$TEST_SYSTEMCTL_LOG"
if [ -f "$HOME/.config/pipewire/filter-chain.conf.d/99-iblueconnect-rnnoise.conf" ] &&
   [ "${TEST_LOAD_FAIL:-0}" = 1 ]; then
    /bin/cp "$TEST_GRAPH_BAD" "$TEST_GRAPH"
elif [ -f "$HOME/.config/pipewire/filter-chain.conf.d/99-iblueconnect-rnnoise.conf" ]; then
    /bin/cp "$TEST_GRAPH_LOADED" "$TEST_GRAPH"
else
    printf '[]\\n' > "$TEST_GRAPH"
fi
''')
            systemctl.chmod(0o755)
            plugin = root / "librnnoise_ladspa.so"
            plugin.write_bytes(b"fake offline plugin")
            snapshot = root / "snapshot.json"
            graph = root / "graph.json"
            loaded_graph = root / "loaded.json"
            bad_graph = root / "bad.json"
            log = root / "systemctl.log"
            graph.write_text("[]")
            loaded_graph.write_text(json.dumps([
                {"id": 1, "type": "PipeWire:Interface:Node", "info": {"props": {"node.name": "echo-cancel-source", "media.class": "Audio/Source"}}},
                {"id": 2, "type": "PipeWire:Interface:Node", "info": {"props": {"node.name": "iblueconnect-rnnoise-capture"}}},
                {"id": 3, "type": "PipeWire:Interface:Node", "info": {"props": {"node.name": "wired-mic", "media.class": "Audio/Source"}}},
                {"type": "PipeWire:Interface:Metadata", "props": {"metadata.name": "default"}, "metadata": [
                    {"key": "default.audio.source", "value": {"name": "echo-cancel-source"}}]},
                {"type": "PipeWire:Interface:Link", "info": {"output-node-id": 3, "input-node-id": 2, "state": "paused"}},
            ]))
            bad = json.loads(loaded_graph.read_text())
            bad[3]["metadata"][0]["value"]["name"] = "wired-mic"
            bad_graph.write_text(json.dumps(bad))
            env = os.environ | {
                "HOME": tmp, "PATH": f"{bin_dir}:{os.environ['PATH']}",
                "TEST_SNAPSHOT": str(snapshot), "TEST_GRAPH": str(graph),
                "TEST_GRAPH_LOADED": str(loaded_graph), "TEST_GRAPH_BAD": str(bad_graph),
                "TEST_SYSTEMCTL_LOG": str(log),
            }

            def run(*args):
                return subprocess.run(["sh", str(SCRIPT), *args], env=env, capture_output=True)

            def status(phase, state="ready"):
                snapshot.write_text(json.dumps({"type": "sssss", "data": [state, "", phase, "", ""]}))

            config = root / ".config/pipewire/filter-chain.conf.d/99-iblueconnect-rnnoise.conf"
            installed = root / ".local/lib/iblueconnect/librnnoise_ladspa.so"
            status("active")
            self.assertNotEqual(run("enable", str(plugin)).returncode, 0)
            self.assertFalse(config.exists())
            status("idle", "manage calls on phone")
            self.assertNotEqual(run("enable", str(plugin)).returncode, 0)
            status("idle")
            graph.write_text(json.dumps([{"type": "PipeWire:Interface:Node", "info": {"props": {
                "api.bluez5.profile": "headset-audio-gateway", "media.class": "Stream/Input/Audio"
            }}}]))
            self.assertNotEqual(run("enable", str(plugin)).returncode, 0)
            graph.write_text(json.dumps([{"type": "PipeWire:Interface:Node", "info": {"props": {"node.name": "echo-cancel-source"}}}]))
            self.assertNotEqual(run("enable", str(plugin)).returncode, 0)
            graph.write_text("[]")
            self.assertEqual(run("enable", str(plugin)).returncode, 0)
            self.assertTrue(config.exists())
            self.assertEqual(installed.read_bytes(), plugin.read_bytes())
            self.assertIn('node.name = "echo-cancel-source"', config.read_text())
            self.assertIn('audio.position = [ MONO ]', config.read_text())
            status("active")
            self.assertNotEqual(run("rollback").returncode, 0)
            self.assertTrue(config.exists())
            status("idle")
            self.assertEqual(run("rollback").returncode, 0)
            self.assertFalse(config.exists())
            self.assertFalse(installed.exists())
            self.assertEqual(log.read_text().splitlines(), ["--user restart filter-chain.service"] * 2)
            env["TEST_LOAD_FAIL"] = "1"
            self.assertNotEqual(run("enable", str(plugin)).returncode, 0)
            self.assertFalse(config.exists())
            self.assertFalse(installed.exists())
            self.assertEqual(log.read_text().splitlines(), ["--user restart filter-chain.service"] * 4)


if __name__ == "__main__":
    unittest.main()
