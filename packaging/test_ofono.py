import ctypes
import ctypes.util
import json
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
import unittest


PACKAGING = Path(__file__).resolve().parent
LEGACY = '.config/wireplumber/bluetooth.lua.d/51-iblueconnect-ofono.lua'
MODERN = '.config/wireplumber/wireplumber.conf.d/51-iblueconnect-ofono.conf'
LEGACY_TEXT = '-- IBlueConnect: oFono owns HFP calls. Remove this file to restore the native backend.\nbluez_monitor.properties["bluez5.hfphsp-backend"] = "ofono"\n'


class OfonoScriptsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        commands = self.root / 'bin'
        commands.mkdir()
        scripts = {
            'sudo': '#!/bin/sh\nexec "$@"\n',
            'apt-get': '#!/bin/sh\nprintf "apt-get %s\\n" "$*" >> "$TEST_LOG"\n',
            'systemctl': '#!/bin/sh\nprintf "systemctl %s\\n" "$*" >> "$TEST_LOG"\nif [ "$*" = "--user restart wireplumber" ] && [ -f "$TEST_NEXT_SNAPSHOT" ]; then cp "$TEST_NEXT_SNAPSHOT" "$TEST_SNAPSHOT"; fi\nif [ "$*" = "--user stop iblueconnectd.service" ]; then rm -f "$TEST_SNAPSHOT"; fi\n',
            'dpkg-query': '#!/bin/sh\nprintf "install ok installed\\n"\n',
            'wireplumber': '#!/bin/sh\nprintf "wireplumber\\nCompiled with libwireplumber %s\\nLinked with libwireplumber %s\\n" "$TEST_VERSION" "$TEST_VERSION"\n',
            'busctl': '#!/bin/sh\nprintf "busctl\\n" >> "$TEST_QUERY_LOG"\nexec /bin/cat "$TEST_SNAPSHOT"\n',
            'pw-dump': '#!/bin/sh\nprintf "pw-dump\\n" >> "$TEST_QUERY_LOG"\nexec /bin/cat "$TEST_GRAPH"\n',
        }
        for name, content in scripts.items():
            command = commands / name
            command.write_text(content)
            command.chmod(0o755)
        self.snapshot = self.root / 'snapshot.json'
        self.graph = self.root / 'graph.json'
        self.log = self.root / 'actions.log'
        self.status('idle')
        self.graph.write_text('[]')
        self.env = os.environ | {
            'HOME': self.tmp.name, 'PATH': f"{commands}:{os.environ['PATH']}",
            'TEST_VERSION': '0.5.13', 'TEST_SNAPSHOT': str(self.snapshot),
            'TEST_GRAPH': str(self.graph), 'TEST_LOG': str(self.log),
            'TEST_QUERY_LOG': str(self.root / 'queries.log'),
            'TEST_NEXT_SNAPSHOT': str(self.root / 'next-snapshot.json'),
        }

    def status(self, phase, state='ready'):
        self.snapshot.write_text(json.dumps({'type': 'sssss', 'data': [state, '', phase, '', '']}))

    def write_legacy(self, text=LEGACY_TEXT):
        path = self.root / LEGACY
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def run_script(self, name, *args):
        return subprocess.run(['sh', str(PACKAGING / name), *args], env=self.env, capture_output=True)

    def test_print_config_is_identical_and_needs_no_call_or_audio_state(self):
        for version, config in (('0.4.17', LEGACY), ('0.5.13', MODERN)):
            with self.subTest(version=version):
                self.env['TEST_VERSION'] = version
                self.status('idle')
                self.graph.write_text('[]')
                self.assertEqual(self.run_script('ofono-enable.sh').returncode, 0)
                expected = (self.root / config).read_bytes()
                (self.root / config).unlink()
                self.log.unlink()
                queries = Path(self.env['TEST_QUERY_LOG'])
                queries.unlink()
                # Both active and missing state must allow offline generation.
                for phase in ('active', None):
                    if phase:
                        self.status(phase)
                    else:
                        self.snapshot.unlink()
                    self.graph.unlink(missing_ok=True)
                    result = self.run_script('ofono-enable.sh', '--print-config')
                    self.assertEqual(result.returncode, 0, result.stderr.decode())
                    self.assertEqual(result.stdout, expected)
                    self.assertFalse(self.log.exists())
                    self.assertFalse(queries.exists(), 'Printer must not query desktop services')
                    self.assertFalse((self.root / config).exists())

    def test_upgrade_selects_ofono_in_modern_configuration(self):
        self.write_legacy()
        result = self.run_script('ofono-enable.sh')
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        config = self.root / MODERN
        self.assertTrue(config.exists(), 'WirePlumber 0.5 must receive a conf.d fragment')
        self.assertIn('monitor.bluez.properties', config.read_text())
        self.assertIn('bluez5.hfphsp-backend = "ofono"', config.read_text())
        self.assertNotIn('msbc', config.read_text())
        self.assertFalse((self.root / LEGACY).exists())

    def test_unsafe_or_unverifiable_call_state_blocks_changes(self):
        for script in ('ofono-enable.sh', 'ofono-rollback.sh'):
            for phase, state in (('incoming', 'ready'), ('active', 'ready'),
                                 ('unavailable', 'ready'), ('idle', 'manage calls on phone')):
                with self.subTest(script=script, phase=phase, state=state):
                    self.status(phase, state)
                    self.log.unlink(missing_ok=True)
                    self.write_legacy()
                    result = self.run_script(script)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse(self.log.exists(), 'No package or service change is allowed')
                    self.assertTrue((self.root / LEGACY).exists())
            for snapshot in ('', '{}', '{"type":"sssss","data":["ready","","idle"]}', 'not json'):
                with self.subTest(script=script, malformed=snapshot):
                    self.snapshot.write_text(snapshot)
                    self.log.unlink(missing_ok=True)
                    self.assertNotEqual(self.run_script(script).returncode, 0)
                    self.assertFalse(self.log.exists())

    def test_new_call_after_wireplumber_restart_blocks_next_restart(self):
        next_snapshot = Path(self.env['TEST_NEXT_SNAPSHOT'])
        next_snapshot.write_text(json.dumps({'type': 'sssss', 'data': ['ready', '', 'incoming', '', '']}))
        result = self.run_script('ofono-enable.sh')
        self.assertNotEqual(result.returncode, 0)
        actions = self.log.read_text()
        self.assertIn('systemctl --user restart wireplumber\n', actions)
        self.assertNotIn('systemctl restart ofono.service\n', actions)
        self.assertNotIn('systemctl --user restart iblueconnectd.service\n', actions)

    def test_existing_ofono_package_is_not_installed_again(self):
        self.assertEqual(self.run_script('ofono-enable.sh').returncode, 0)
        self.assertNotIn('apt-get', self.log.read_text())

    def test_rollback_removes_both_config_formats_and_stops_daemon_last(self):
        self.write_legacy()
        modern = self.root / MODERN
        modern.parent.mkdir(parents=True, exist_ok=True)
        modern.write_text('# IBlueConnect: oFono owns HFP calls. Remove this file to restore the native backend.\n')
        self.status('idle', 'calling unavailable')
        result = self.run_script('ofono-rollback.sh')
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertFalse((self.root / LEGACY).exists())
        self.assertFalse(modern.exists())
        self.assertEqual(self.log.read_text().splitlines()[-1], 'systemctl --user stop iblueconnectd.service')

    def test_audio_graph_must_be_valid_and_have_no_hfp_nodes(self):
        for script in ('ofono-enable.sh', 'ofono-rollback.sh'):
            for graph in ('', '{}', 'not json', json.dumps([{'type': 'PipeWire:Interface:Node',
                    'info': {'props': {'api.bluez5.profile': 'headset-audio-gateway'}}}])):
                with self.subTest(script=script, graph=graph):
                    self.graph.write_text(graph)
                    self.log.unlink(missing_ok=True)
                    self.assertNotEqual(self.run_script(script).returncode, 0)
                    self.assertFalse(self.log.exists())
            self.graph.unlink()
            self.assertNotEqual(self.run_script(script).returncode, 0)
            self.assertFalse(self.log.exists())

    def test_disconnected_idle_repair_and_legacy_compatibility(self):
        self.status('idle', 'calling unavailable')
        for version, config in (('0.4.17', LEGACY), ('0.5.13', MODERN), ('0.10.0', MODERN), ('1.0.0', MODERN)):
            with self.subTest(version=version):
                self.env['TEST_VERSION'] = version
                result = self.run_script('ofono-enable.sh')
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertIn('bluez5.hfphsp-backend', (self.root / config).read_text())

    def test_upgrade_preserves_user_written_legacy_file(self):
        custom = '-- User Bluetooth settings\nbluez_monitor.properties["custom"] = true\n'
        self.write_legacy(custom)
        self.assertEqual(self.run_script('ofono-enable.sh').returncode, 0)
        self.assertEqual((self.root / LEGACY).read_text(), custom)

    def test_downgrade_removes_app_managed_modern_format(self):
        self.assertEqual(self.run_script('ofono-enable.sh').returncode, 0)
        self.env['TEST_VERSION'] = '0.4.17'
        self.assertEqual(self.run_script('ofono-enable.sh').returncode, 0)
        self.assertTrue((self.root / LEGACY).exists())
        self.assertFalse((self.root / MODERN).exists())

    def test_noise_filter_targets_only_outgoing_phone_audio(self):
        library = ctypes.util.find_library('wireplumber-0.5')
        parser = shutil.which('spa-json-dump')
        if not library or not parser:
            self.skipTest('WirePlumber 0.5 library and SPA JSON parser are required')
        self.assertEqual(self.run_script('ofono-enable.sh').returncode, 0)
        parsed = json.loads(subprocess.check_output([parser, str(self.root / MODERN)]))
        rules = parsed['monitor.bluez.rules']
        wp = ctypes.CDLL(library)
        signatures = {
            'wp_spa_json_new_from_string': ([ctypes.c_char_p], ctypes.c_void_p),
            'wp_properties_new_empty': ([], ctypes.c_void_p),
            'wp_properties_set': ([ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p], ctypes.c_int),
            'wp_properties_get': ([ctypes.c_void_p, ctypes.c_char_p], ctypes.c_char_p),
            'wp_json_utils_match_rules_update_properties': ([ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
            'wp_properties_unref': ([ctypes.c_void_p], None),
            'wp_spa_json_unref': ([ctypes.c_void_p], None),
        }
        for name, (args, result) in signatures.items():
            function = getattr(wp, name)
            function.argtypes, function.restype = args, result
        native_rules = wp.wp_spa_json_new_from_string(json.dumps(rules).encode())
        self.addCleanup(wp.wp_spa_json_unref, native_rules)
        # bluez.lua sets factory.name before matching; media.class is exported later.
        for profile, factory, expected in (
            ('headset-audio-gateway', 'api.bluez5.sco.sink', b'echo-cancel-source'),
            ('headset-audio-gateway', 'api.bluez5.sco.source', None),
            ('headset-head-unit', 'api.bluez5.sco.sink', None),
            ('', 'ordinary-app-capture', None),
            ('', 'physical-source', None),
        ):
            with self.subTest(profile=profile, factory=factory):
                props = wp.wp_properties_new_empty()
                try:
                    wp.wp_properties_set(props, b'api.bluez5.profile', profile.encode())
                    wp.wp_properties_set(props, b'factory.name', factory.encode())
                    wp.wp_json_utils_match_rules_update_properties(native_rules, props)
                    self.assertEqual(wp.wp_properties_get(props, b'target.object'), expected)
                    self.assertNotEqual(wp.wp_properties_get(props, b'node.dont-fallback'), b'true')
                finally:
                    wp.wp_properties_unref(props)

    def test_legacy_noise_rule_combines_profile_and_capture_direction(self):
        library = ctypes.util.find_library('lua5.4')
        if not library:
            self.skipTest('Lua library is required to validate the legacy fragment')
        self.env['TEST_VERSION'] = '0.4.17'
        self.assertEqual(self.run_script('ofono-enable.sh').returncode, 0)
        lua = ctypes.CDLL(library)
        lua.luaL_newstate.restype = ctypes.c_void_p
        lua.luaL_loadstring.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lua.lua_pcallk.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ssize_t, ctypes.c_void_p]
        lua.luaL_openlibs.argtypes = lua.lua_close.argtypes = [ctypes.c_void_p]
        state = lua.luaL_newstate()
        self.addCleanup(lua.lua_close, state)
        lua.luaL_openlibs(state)
        source = 'bluez_monitor = { properties = {}, rules = {} }\n' + (self.root / LEGACY).read_text() + '''
assert(#bluez_monitor.rules == 1)
local rule = bluez_monitor.rules[1]
assert(#rule.matches == 1 and #rule.matches[1] == 2)
assert(rule.matches[1][1][1] == "api.bluez5.profile" and rule.matches[1][1][2] == "equals" and rule.matches[1][1][3] == "headset-audio-gateway")
assert(rule.matches[1][2][1] == "factory.name" and rule.matches[1][2][2] == "equals" and rule.matches[1][2][3] == "api.bluez5.sco.sink")
assert(rule.apply_properties["target.object"] == "echo-cancel-source")
assert(rule.apply_properties["node.dont-fallback"] ~= true)
'''
        self.assertEqual(lua.luaL_loadstring(state, source.encode()), 0, 'Legacy configuration must parse as Lua')
        self.assertEqual(lua.lua_pcallk(state, 0, 0, 0, 0, None), 0, 'Legacy rule must target only outgoing phone audio')


if __name__ == '__main__':
    unittest.main()
