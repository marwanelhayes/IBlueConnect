import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import shutil
import platform
import shlex
import sys


PACKAGING = Path(__file__).resolve().parent


class SetupTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.commands = self.root / 'bin'
        self.commands.mkdir()
        self.log = self.root / 'actions'
        self.lib = self.root / 'lib'
        self.lib.mkdir()
        interpreter = self.lib / 'setup-venv/bin/python3'
        interpreter.parent.mkdir(parents=True)
        interpreter.write_text('#!/bin/sh\nprintf "private-venv\\n" >> "$TEST_INTERPRETER_LOG"\nexec '
                               + shlex.quote(sys.executable) + ' "$@"\n')
        interpreter.chmod(0o755)
        for name in ('first-run.sh', 'setup-user.py'):
            shutil.copy2(PACKAGING / name, self.lib / name)
        (self.lib / 'librnnoise_ladspa.so').write_bytes(b'offline-plugin')
        self.fixtures = self.root / 'fixtures.json'
        self.fixtures.write_text(json.dumps({'Snapshot': None, 'GetManagedObjects': {}, 'GetModems': [], 'GetCalls': [], 'owners': {}}))
        self.graph = self.root / 'graph.json'
        graph = [{'id': 1, 'type': 'PipeWire:Interface:Node', 'info': {'props': {'node.name': 'physical-mic', 'media.class': 'Audio/Source'}}},
                 {'type': 'PipeWire:Interface:Metadata', 'props': {'metadata.name': 'default'},
                  'metadata': [{'key': 'default.audio.source', 'value': {'name': 'physical-mic'}}]}]
        self.graph.write_text(json.dumps(graph))
        loaded = graph + [{'id': 2, 'type': 'PipeWire:Interface:Node', 'info': {'props': {'node.name': 'echo-cancel-source'}}},
                          {'id': 3, 'type': 'PipeWire:Interface:Node', 'info': {'props': {'node.name': 'iblueconnect-rnnoise-capture'}}},
                          {'type': 'PipeWire:Interface:Link', 'info': {'input-node-id': 3, 'output-node-id': 1, 'state': 'paused'}}]
        loaded_path = self.root / 'loaded.json'
        loaded_path.write_text(json.dumps(loaded))
        self.env = os.environ | {'HOME': str(self.root), 'PATH': f'{self.commands}:{os.environ["PATH"]}',
                                 'TEST_LOG': str(self.log), 'TEST_CONSENT': '1', 'TEST_FIXTURES': str(self.fixtures),
                                 'TEST_GRAPH': str(self.graph), 'TEST_LOADED': str(loaded_path),
                                 'TEST_INTERPRETER_LOG': str(self.root / 'interpreter.log')}
        self.command('zenity', 'exit "$TEST_CONSENT"')
        self.command('pw-dump', 'cat "$TEST_GRAPH"')
        self.command('busctl', '''exec python3 - "$@" <<'PY'
import json, os, sys
f = json.load(open(os.environ['TEST_FIXTURES']))
m = sys.argv[-1]
if 'NameHasOwner' in sys.argv:
    if sys.argv[-1] == 'org.iblueconnect.Daemon' and os.path.exists(os.environ.get('TEST_OWNER_TRIGGER', '/nonexistent-test-trigger')):
        f['owners']['org.iblueconnect.Daemon'] = True
        f['Snapshot'] = ['ready', '', 'incoming', '', '']
        json.dump(f, open(os.environ['TEST_FIXTURES'], 'w'))
    print(json.dumps({'type':'b','data':[f['owners'].get(sys.argv[-1], False)]}))
elif f.get(m) is None:
    sys.exit(1)
else:
    types = {'Snapshot':'sssss','GetManagedObjects':'a{oa{sa{sv}}}','GetModems':'a(oa{sv})','GetCalls':'a(oa{sv})'}
    print(json.dumps({'type':types[m], 'data': f[m] if m == 'Snapshot' else [f[m]]}))
PY''')
        self.command('systemctl', '''printf "%s\\n" "$*" >> "$TEST_LOG"
case "$*" in
*is-active*iblueconnectd.service|*is-enabled*iblueconnectd.service) exit 1;;
esac
if [ "$*" = '--user restart filter-chain.service' ]; then cp "$TEST_LOADED" "$TEST_GRAPH"; fi
if [ "$*" = "${TEST_FAIL_STEP:-never}" ]; then exit 1; fi
if [ "$*" = '--user restart wireplumber.service' ] && [ -n "${TEST_NEXT_FIXTURES:-}" ]; then cp "$TEST_NEXT_FIXTURES" "$TEST_FIXTURES"; fi''')
        self.command('pkexec', 'exec "$@"')
        for name, content in [('ofono-enable.sh', '# IBlueConnect: oFono owns HFP calls. Remove this file to restore the native backend.\\nmonitor.bluez.properties = {}'),
                              ('rnnoise.sh', '# IBlueConnect RNNoise; remove with packaging/rnnoise.sh rollback.\\ncontext.modules = []')]:
            (self.lib / name).write_text('#!/bin/sh\nprintf "' + content + '\\n"\n')
        for name in ('ofono-enable.sh', 'rnnoise.sh'):
            shutil.copy2(PACKAGING / name, self.lib / name)
        self.command('wireplumber', 'printf "wireplumber\\nCompiled with libwireplumber ${TEST_WP_VERSION:-0.5.13}\\n"')

    def command(self, name, body):
        path = self.commands / name
        path.write_text('#!/bin/sh\n' + body + '\n')
        path.chmod(0o755)

    def run_setup(self, *args):
        return subprocess.run(['sh', str(self.lib / 'first-run.sh'), *args], env=self.env, capture_output=True)

    def test_cancelling_setup_changes_nothing(self):
        result = self.run_setup()
        self.assertEqual(result.returncode, 2, result.stderr.decode())
        self.assertEqual(result.stderr, b'')
        self.assertFalse((self.root / '.local/share/iblueconnect').exists())
        self.assertFalse(self.log.exists())

    def test_setup_uses_packaged_private_python_interpreter(self):
        self.assertEqual(self.run_setup().returncode, 2)
        self.assertEqual(Path(self.env['TEST_INTERPRETER_LOG']).read_text(), 'private-venv\n')

    def test_first_run_without_daemon_succeeds_and_is_repeatable(self):
        self.env['TEST_CONSENT'] = '0'
        result = self.run_setup()
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertTrue((self.root / '.local/share/iblueconnect/setup-complete.json').exists())
        self.assertIn('--user restart wireplumber.service', self.log.read_text())
        self.assertIn('restart ofono.service', self.log.read_text())
        self.assertNotIn('set-default', self.log.read_text())
        before = self.log.read_text()
        self.assertEqual(self.run_setup().returncode, 0)
        self.assertEqual(self.log.read_text(), before)

    def test_active_unknown_or_connected_phone_blocks_changes(self):
        self.env['TEST_CONSENT'] = '0'
        initial = json.loads(self.fixtures.read_text())
        cases = [{'Snapshot': ['ready', '', 'active', '', '']},
                 {'Snapshot': ['manage calls on phone', '', 'idle', '', '']},
                 {'Snapshot': None, 'owners': {'org.iblueconnect.Daemon': True}},
                 {'GetManagedObjects': {'/device': {'org.bluez.Device1': {'Connected': {'type': 'b', 'data': True}}}}},
                 {'owners': {'org.ofono': True}, 'GetModems': [['/modem', {'Interfaces': {'type': 'as', 'data': ['org.ofono.VoiceCallManager']}}]],
                  'GetCalls': [['/call', {}]]}]
        for case in cases:
            with self.subTest(case=case):
                self.fixtures.write_text(json.dumps(initial | case))
                result = self.run_setup()
                self.assertEqual(result.returncode, 1)
                self.assertFalse(self.log.exists())
                self.assertFalse((self.root / '.local/share/iblueconnect/setup-consent.json').exists())

    def test_new_call_between_service_steps_pauses_and_restores_files(self):
        self.env['TEST_CONSENT'] = '0'
        next_fixture = self.root / 'next.json'
        state = json.loads(self.fixtures.read_text())
        state['Snapshot'] = ['ready', '', 'incoming', '', '']
        next_fixture.write_text(json.dumps(state))
        self.env['TEST_NEXT_FIXTURES'] = str(next_fixture)
        result = self.run_setup()
        self.assertEqual(result.returncode, 1)
        self.assertNotIn('restart ofono.service', self.log.read_text())
        self.assertFalse((self.root / '.local/share/iblueconnect/setup-complete.json').exists())
        self.assertFalse((self.root / '.config/wireplumber/wireplumber.conf.d/51-iblueconnect-ofono.conf').exists())

    def test_unlinked_filter_does_not_claim_setup_complete(self):
        self.env['TEST_CONSENT'] = '0'
        loaded = Path(self.env['TEST_LOADED'])
        graph = json.loads(loaded.read_text())
        graph.pop()
        loaded.write_text(json.dumps(graph))
        result = self.run_setup()
        self.assertEqual(result.returncode, 1)
        self.assertFalse((self.root / '.local/share/iblueconnect/setup-complete.json').exists())
        self.assertNotIn('--user restart iblueconnectd.service', self.log.read_text())

    def test_failure_restores_original_service_state(self):
        self.env['TEST_CONSENT'] = '0'
        self.env['TEST_FAIL_STEP'] = '--user restart filter-chain.service'
        self.command('systemctl', '''case "$*" in
*is-enabled*|*is-active*) exit 1;;
esac
printf "%s\\n" "$*" >> "$TEST_LOG"
[ "$*" != "$TEST_FAIL_STEP" ]''')
        result = self.run_setup()
        self.assertEqual(result.returncode, 1)
        self.assertIn('stop ofono.service', self.log.read_text())
        self.assertIn('--user disable filter-chain.service', self.log.read_text())
        self.assertFalse((self.root / '.config/pipewire/filter-chain.conf.d/99-iblueconnect-rnnoise.conf').exists())

    def test_legacy_wireplumber_receives_legacy_config(self):
        self.env['TEST_CONSENT'] = '0'
        self.env['TEST_WP_VERSION'] = '0.4.17'
        result = self.run_setup()
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        text = (self.root / '.config/wireplumber/bluetooth.lua.d/51-iblueconnect-ofono.lua').read_text()
        self.assertIn('factory.name', text)
        self.assertIn('api.bluez5.sco.sink', text)
        self.assertFalse((self.root / '.config/wireplumber/wireplumber.conf.d/51-iblueconnect-ofono.conf').exists())

    def test_undo_restores_settings_and_keeps_private_cache(self):
        self.env['TEST_CONSENT'] = '0'
        self.assertEqual(self.run_setup().returncode, 0)
        cache = self.root / '.local/share/iblueconnect/contacts-cache'
        cache.write_text('offline fixture')
        self.assertEqual(self.run_setup('--undo').returncode, 0)
        self.assertEqual(cache.read_text(), 'offline fixture')
        self.assertFalse((cache.parent / 'setup-complete.json').exists())
        self.assertFalse((self.root / '.config/wireplumber/wireplumber.conf.d/51-iblueconnect-ofono.conf').exists())

    def test_undo_refuses_active_call(self):
        self.env['TEST_CONSENT'] = '0'
        self.assertEqual(self.run_setup().returncode, 0)
        initial = json.loads(self.fixtures.read_text())
        initial['Snapshot'] = ['ready', '', 'active', '', '']
        self.fixtures.write_text(json.dumps(initial))
        before = self.log.read_text()
        self.assertEqual(self.run_setup('--undo').returncode, 1)
        self.assertEqual(self.log.read_text(), before)
        self.assertTrue((self.root / '.local/share/iblueconnect/setup-complete.json').exists())

    def test_undo_pauses_on_new_call_and_can_resume(self):
        self.env['TEST_CONSENT'] = '0'
        self.assertEqual(self.run_setup().returncode, 0)
        initial = json.loads(self.fixtures.read_text())
        next_fixture = self.root / 'next.json'
        next_fixture.write_text(json.dumps(initial | {'Snapshot': ['ready', '', 'incoming', '', '']}))
        self.env['TEST_NEXT_FIXTURES'] = str(next_fixture)
        self.assertEqual(self.run_setup('--undo').returncode, 1)
        state = self.root / '.local/share/iblueconnect'
        self.assertTrue((state / 'setup-undo-pending.json').exists())
        before = self.log.read_text()
        self.assertEqual(self.run_setup().returncode, 1)
        self.assertEqual(self.log.read_text(), before)
        self.fixtures.write_text(json.dumps(initial))
        self.env.pop('TEST_NEXT_FIXTURES')
        self.assertEqual(self.run_setup('--undo').returncode, 0)
        self.assertFalse((state / 'setup-undo-pending.json').exists())

    def test_filter_taking_default_is_corrected_to_original_mic(self):
        self.env['TEST_CONSENT'] = '0'
        loaded = Path(self.env['TEST_LOADED'])
        graph = json.loads(loaded.read_text())
        graph[1]['metadata'][0]['value']['name'] = 'echo-cancel-source'
        loaded.write_text(json.dumps(graph))
        self.command('wpctl', '''printf "wpctl %s\\n" "$*" >> "$TEST_LOG"
exec python3 - <<'PY'
import json, os
p = os.environ['TEST_GRAPH']
g = json.load(open(p))
g[1]['metadata'][0]['value']['name'] = 'physical-mic'
json.dump(g, open(p, 'w'))
PY''')
        self.assertEqual(self.run_setup().returncode, 0)
        self.assertIn('wpctl set-default 1', self.log.read_text())

    def test_user_switching_physical_mic_is_not_overridden(self):
        self.env['TEST_CONSENT'] = '0'
        loaded = Path(self.env['TEST_LOADED'])
        graph = json.loads(loaded.read_text())
        graph[1]['metadata'][0]['value']['name'] = 'other-physical-mic'
        loaded.write_text(json.dumps(graph))
        self.command('wpctl', 'printf "wpctl %s\\n" "$*" >> "$TEST_LOG"')
        self.assertEqual(self.run_setup().returncode, 1)
        self.assertNotIn('wpctl set-default', self.log.read_text())

    def test_launcher_checks_ubuntu_version_and_passes_gui_arguments(self):
        release = platform.freedesktop_os_release()
        if release.get('ID') != 'ubuntu':
            self.skipTest('Package launcher runs on Ubuntu only')
        prefix = self.root / 'package/usr'
        lib = prefix / 'lib/iblueconnect'
        lib.mkdir(parents=True)
        binary = prefix / 'bin/iblueconnect'
        binary.parent.mkdir()
        shutil.copy2(PACKAGING / 'iblueconnect-launcher.sh', binary)
        (lib / 'first-run.sh').write_text('#!/bin/sh\nexit "${TEST_SETUP_EXIT:-0}"\n')
        gui = lib / 'iblueconnect'
        gui.write_text('#!/bin/sh\nprintf "gui %s\\n" "$*" >> "$TEST_LOG"\n')
        gui.chmod(0o755)
        version = lib / 'ubuntu-version'
        version.write_text('0.00\n')
        result = subprocess.run(['sh', str(binary)], env=self.env, capture_output=True)
        self.assertEqual(result.returncode, 1)
        self.assertFalse(self.log.exists())
        version.write_text(release['VERSION_ID'] + '\n')
        result = subprocess.run(['sh', str(binary), '--developer'], env=self.env, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(self.log.read_text(), 'gui --developer\n')
        self.env['TEST_SETUP_EXIT'] = '2'
        result = subprocess.run(['sh', str(binary)], env=self.env, capture_output=True)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.log.read_text(), 'gui --developer\n')

    def test_new_ubuntu_package_rechecks_setup_and_migrates_config(self):
        self.env['TEST_CONSENT'] = '0'
        self.env['TEST_WP_VERSION'] = '0.4.17'
        version = self.lib / 'ubuntu-version'
        version.write_text('24.04\n')
        self.assertEqual(self.run_setup().returncode, 0)
        self.env['TEST_WP_VERSION'] = '0.5.13'
        version.write_text('26.04\n')
        self.assertEqual(self.run_setup().returncode, 0)
        self.assertFalse((self.root / '.config/wireplumber/bluetooth.lua.d/51-iblueconnect-ofono.lua').exists())
        self.assertTrue((self.root / '.config/wireplumber/wireplumber.conf.d/51-iblueconnect-ofono.conf').exists())

    def test_upgrade_preflight_failure_keeps_new_custom_settings(self):
        self.env['TEST_CONSENT'] = '0'
        version = self.lib / 'version'
        version.write_text('0.2.0\n')
        self.assertEqual(self.run_setup().returncode, 0)
        config = self.root / '.config/wireplumber/wireplumber.conf.d/51-iblueconnect-ofono.conf'
        config.write_text('# Custom user configuration\n')
        version.write_text('0.2.1\n')
        before = self.log.read_text()
        self.assertEqual(self.run_setup().returncode, 1)
        self.assertEqual(config.read_text(), '# Custom user configuration\n')
        self.assertEqual(self.log.read_text(), before)

    def test_running_daemon_blocks_incomplete_or_upgrade_setup_before_changes(self):
        self.env['TEST_CONSENT'] = '0'
        initial = json.loads(self.fixtures.read_text())
        initial['owners'] = {'org.iblueconnect.Daemon': True}
        initial['Snapshot'] = ['calling unavailable', '', 'unavailable', '', '']
        self.fixtures.write_text(json.dumps(initial))
        self.assertEqual(self.run_setup().returncode, 1)
        self.assertFalse(self.log.exists())
        self.assertFalse((self.root / '.local/share/iblueconnect/setup-consent.json').exists())

    def test_daemon_appearing_during_setup_gets_fresh_snapshot_before_restart(self):
        self.env['TEST_CONSENT'] = '0'
        self.env['TEST_OWNER_TRIGGER'] = str(self.root / 'owner-trigger')
        body = (self.commands / 'systemctl').read_text()
        body += '''
if [ "$*" = "--user enable iblueconnectd.service" ]; then
python3 - <<'PY'
import json, os
p = os.environ['TEST_FIXTURES']
f = json.load(open(p))
f['Snapshot'] = ['ready', '', 'idle', '', '']
json.dump(f, open(p, 'w'))
PY
touch "$TEST_OWNER_TRIGGER"
fi
'''
        self.command('systemctl', body)
        self.assertEqual(self.run_setup().returncode, 1)
        self.assertNotIn('--user restart iblueconnectd.service', self.log.read_text())
        self.assertFalse((self.root / '.local/share/iblueconnect/setup-complete.json').exists())


if __name__ == '__main__':
    unittest.main()
