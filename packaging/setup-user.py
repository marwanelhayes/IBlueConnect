#!/usr/bin/python3
"""Consent-based package setup; never connects a phone or prints private replies."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def run(*args):
    return subprocess.run(args, capture_output=True, text=True,
                          timeout=None if args[0] in ('zenity', 'pkexec') else 15)


def checked(*args):
    result = run(*args)
    if result.returncode:
        raise RuntimeError('Setup could not complete a required step. No phone connection was attempted.')
    return result.stdout


def reply(bus, destination, path, interface, method, expected, *args):
    raw = checked('busctl', bus, '--json=short', '--timeout=5', 'call', destination, path, interface, method, *args)
    value = json.loads(raw)
    if value.get('type') != expected or not isinstance(value.get('data'), list):
        raise RuntimeError('Cannot verify call state. End all calls and disconnect all Bluetooth devices before retrying.')
    return value['data']


def owned(bus, name):
    data = reply(bus, 'org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                 'NameHasOwner', 'b', 's', name)
    if len(data) != 1 or type(data[0]) is not bool:
        raise RuntimeError('Cannot verify call state.')
    return data[0]


def safe(strict_idle=False):
    graph = json.loads(checked('pw-dump'))
    if not isinstance(graph, list) or not all(isinstance(item, dict) for item in graph):
        raise RuntimeError('Cannot verify audio state.')
    if any(item.get('info', {}).get('props', {}).get('api.bluez5.profile') == 'headset-audio-gateway' for item in graph):
        raise RuntimeError('End all calls before setup; phone audio is still active.')
    snapshot = run('busctl', '--user', '--json=short', '--timeout=5', 'call', 'org.iblueconnect.Daemon',
                   '/org/iblueconnect/Daemon', 'org.iblueconnect.Daemon', 'Snapshot')
    if snapshot.returncode == 0:
        value = json.loads(snapshot.stdout)
        data = value.get('data')
        if (value.get('type') != 'sssss' or not isinstance(data, list) or len(data) != 5
                or not all(isinstance(v, str) for v in data)
                or not (data[0] in ('ready', 'calling unavailable') and data[2] == 'idle'
                        or not strict_idle and data[0] == 'calling unavailable' and data[2] == 'unavailable')):
            raise RuntimeError('End all calls before setup; the app cannot confirm that calls are idle.')
    elif owned('--user', 'org.iblueconnect.Daemon'):
        raise RuntimeError('The running app cannot confirm call state. Retry after ending all calls.')
    devices = reply('--system', 'org.bluez', '/', 'org.freedesktop.DBus.ObjectManager',
                    'GetManagedObjects', 'a{oa{sa{sv}}}')
    if len(devices) != 1 or not isinstance(devices[0], dict):
        raise RuntimeError('Cannot verify Bluetooth state.')
    for interfaces in devices[0].values():
        if 'org.bluez.Device1' in interfaces:
            connected = interfaces['org.bluez.Device1'].get('Connected')
            if not isinstance(connected, dict) or connected.get('type') != 'b' or connected.get('data') is not False:
                raise RuntimeError('Disconnect all Bluetooth devices, including phones and headsets, in Settings before setup, then retry.')
    if owned('--system', 'org.ofono'):
        modems = reply('--system', 'org.ofono', '/', 'org.ofono.Manager', 'GetModems', 'a(oa{sv})')
        if len(modems) != 1 or not isinstance(modems[0], list):
            raise RuntimeError('Cannot verify phone calls.')
        for modem in modems[0]:
            if not isinstance(modem, list) or len(modem) != 2 or not isinstance(modem[1], dict):
                raise RuntimeError('Cannot verify phone calls.')
            interfaces = modem[1].get('Interfaces', {})
            if interfaces.get('type') != 'as' or not isinstance(interfaces.get('data'), list):
                raise RuntimeError('Cannot verify phone calls.')
            if 'org.ofono.VoiceCallManager' in interfaces['data']:
                calls = reply('--system', 'org.ofono', modem[0], 'org.ofono.VoiceCallManager',
                              'GetCalls', 'a(oa{sv})')
                if calls != [[]]:
                    raise RuntimeError('End all calls before setup; the phone still reports a call.')
    return graph


def default_source(graph):
    for item in graph:
        if item.get('type') == 'PipeWire:Interface:Metadata' and item.get('props', {}).get('metadata.name') == 'default':
            for entry in item.get('metadata', []):
                if entry.get('key') == 'default.audio.source':
                    value = entry.get('value')
                    if isinstance(value, str):
                        value = json.loads(value)
                    return value.get('name')
    return None


def verify_filter(original_source):
    for attempt in range(5):
        graph = safe()
        nodes = {item['id']: item.get('info', {}).get('props', {}) for item in graph
                 if item.get('type') == 'PipeWire:Interface:Node'}
        capture = [node_id for node_id, props in nodes.items() if props.get('node.name') == 'iblueconnect-rnnoise-capture']
        source = [node_id for node_id, props in nodes.items() if props.get('node.name') == 'echo-cancel-source']
        physical = [node_id for node_id, props in nodes.items() if props.get('node.name') == original_source]
        if len(physical) == 1 and default_source(graph) != original_source:
            if default_source(graph) != 'echo-cancel-source':
                raise RuntimeError('The selected microphone changed during setup. Keep your preferred microphone selected and retry.')
            safe()
            checked('wpctl', 'set-default', str(physical[0]))
            continue
        linked = any(item.get('type') == 'PipeWire:Interface:Link'
                     and item.get('info', {}).get('input-node-id') in capture
                     and item.get('info', {}).get('output-node-id') in physical
                     and item.get('info', {}).get('state') in ('active', 'paused') for item in graph)
        if len(capture) == 1 and len(source) == 1 and len(physical) == 1 and linked and default_source(graph) == original_source:
            return
        if attempt < 4:
            time.sleep(1)
    raise RuntimeError('The microphone noise filter could not be verified. Original settings were backed up; setup can be retried.')


def write_json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value) + '\n')
    temporary.chmod(0o600)
    temporary.replace(path)


def package_identity(libdir):
    return {'format': 1, **{key: (libdir / name).read_text().strip() if (libdir / name).exists() else ''
                           for key, name in [('ubuntu_version', 'ubuntu-version'), ('app_version', 'version')]}}


def restore(state):
    pending = state / 'setup-pending.json'
    if not pending.exists():
        raise RuntimeError('The original setup backup is unavailable; settings need manual review.')
    manifest = json.loads(pending.read_text())
    for entry in manifest['files']:
        path = Path(entry['path'])
        if entry['present']:
            shutil.copy2(entry['backup'], path)
        else:
            path.unlink(missing_ok=True)
    (state / 'setup-consent.json').unlink(missing_ok=True)
    # Restoring files is safe during a call; reloading services requires fresh proof.
    for service in manifest['services']:
        prefix = ['systemctl', '--user'] if service['user'] else ['pkexec', 'systemctl']
        for operation in [('enable' if service['enabled'] else 'disable'),
                          ('restart' if service['active'] else 'stop')]:
            safe(strict_idle=service['name'] == 'iblueconnectd.service')
            checked(*prefix, operation, service['name'])


def configure(libdir, state):
    if not (libdir / 'librnnoise_ladspa.so').is_file():
        raise RuntimeError('The noise suppression plugin is missing. Reinstall the correct package.')
    if (Path.home() / '.config/systemd/user/iblueconnectd.service').exists():
        raise RuntimeError('An older user installation is present. Ask for migration before using this package.')
    initial_graph = safe()
    if owned('--user', 'org.iblueconnect.Daemon'):
        raise RuntimeError('End all calls and stop the existing IBlueConnect background service before retrying setup. '
                           'Reconnect your phone manually after setup finishes.')
    original_source = default_source(initial_graph)
    if not original_source or not any(item.get('info', {}).get('props', {}).get('node.name') == original_source
                                      and item.get('info', {}).get('props', {}).get('media.class') == 'Audio/Source'
                                      and not item.get('info', {}).get('props', {}).get('node.virtual') for item in initial_graph):
        raise RuntimeError('Select a physical microphone in Ubuntu Sound settings before setup.')
    backend = checked('sh', str(libdir / 'ofono-enable.sh'), '--print-config')
    noise = checked('sh', str(libdir / 'rnnoise.sh'), '--print-config', str(libdir / 'librnnoise_ladspa.so'))
    if not backend.startswith(('# IBlueConnect:', '-- IBlueConnect:')) or not noise.startswith('# IBlueConnect RNNoise;'):
        raise RuntimeError('The packaged audio configuration is incomplete.')
    legacy = Path.home() / '.config/wireplumber/bluetooth.lua.d/51-iblueconnect-ofono.lua'
    modern = Path.home() / '.config/wireplumber/wireplumber.conf.d/51-iblueconnect-ofono.conf'
    filter_config = Path.home() / '.config/pipewire/filter-chain.conf.d/99-iblueconnect-rnnoise.conf'
    files = [legacy, modern, filter_config]
    for path in files:
        if path.exists() and not path.read_text().startswith(('# IBlueConnect:', '-- IBlueConnect:', '# IBlueConnect RNNoise;')):
            raise RuntimeError('Existing custom audio settings need manual review before setup.')
    state.mkdir(parents=True, exist_ok=True, mode=0o700)
    pending = state / 'setup-pending.json'
    if not pending.exists():
        backup = Path(tempfile.mkdtemp(prefix='package-setup-backup-', dir=state))
        manifest = []
        for index, path in enumerate(files):
            saved = backup / str(index)
            if path.exists():
                shutil.copy2(path, saved)
            manifest.append({'path': str(path), 'backup': str(saved), 'present': path.exists()})
        services = []
        for name, user in [('wireplumber.service', True), ('ofono.service', False), ('filter-chain.service', True),
                           ('dundee.service', False), ('iblueconnectd.service', True)]:
            prefix = ['systemctl', '--user'] if user else ['systemctl']
            services.append({'name': name, 'user': user,
                             'active': run(*prefix, 'is-active', '--quiet', name).returncode == 0,
                             'enabled': run(*prefix, 'is-enabled', '--quiet', name).returncode == 0})
        write_json(pending, {'files': manifest, 'services': services})
    try:
        write_json(state / 'setup-consent.json', {'background': True, 'audio': True, 'contact_history_sync': True})
        selected = modern if backend.startswith('#') else legacy
        selected.parent.mkdir(parents=True, exist_ok=True)
        filter_config.parent.mkdir(parents=True, exist_ok=True)
        selected.write_text(backend)
        (legacy if selected == modern else modern).unlink(missing_ok=True)
        filter_config.write_text(noise)
        for command in [('pkexec', 'systemctl', 'disable', '--now', 'dundee.service'),
                        ('systemctl', '--user', 'restart', 'wireplumber.service'),
                        ('pkexec', 'systemctl', 'enable', 'ofono.service'),
                        ('pkexec', 'systemctl', 'restart', 'ofono.service'),
                        ('systemctl', '--user', 'enable', 'filter-chain.service'),
                        ('systemctl', '--user', 'restart', 'filter-chain.service')]:
            safe()
            checked(*command)
        verify_filter(original_source)
        safe(strict_idle=True)
        checked('systemctl', '--user', 'enable', 'iblueconnectd.service')
        daemon_operation = 'restart' if owned('--user', 'org.iblueconnect.Daemon') else 'start'
        safe(strict_idle=True)
        checked('systemctl', '--user', daemon_operation, 'iblueconnectd.service')
        safe()
        write_json(state / 'setup-complete.json', package_identity(libdir) | {'backup_manifest': str(pending)})
    except (RuntimeError, ValueError, OSError, subprocess.TimeoutExpired):
        try:
            restore(state)
        except (RuntimeError, ValueError, OSError, subprocess.TimeoutExpired):
            raise RuntimeError('Setup paused. Original files were backed up, but service restoration did not finish. '
                               'End calls, disconnect all Bluetooth devices, and run Undo before retrying.') from None
        raise


def main():
    state = Path.home() / '.local/share/iblueconnect'
    if sys.argv[2:] == ['--undo']:
        if run('zenity', '--question', '--title=Undo IBlueConnect setup', '--text=Restore the audio settings '
               'and background service state saved before setup? Contacts and call history stay on this laptop. '
               'End calls and disconnect all Bluetooth devices, including phones and headsets, first.').returncode:
            return 2
        try:
            safe(strict_idle=True)
            write_json(state / 'setup-undo-pending.json', {'pending': True})
            restore(state)
            (state / 'setup-complete.json').unlink(missing_ok=True)
            (state / 'setup-undo-pending.json').unlink()
        except (RuntimeError, ValueError, OSError, subprocess.TimeoutExpired):
            run('zenity', '--error', '--title=Undo paused', '--text=Undo did not finish. End all calls, '
                'disconnect all Bluetooth devices, and run Undo again before removing the package. Original backups and private caches are retained.')
            return 1
        run('zenity', '--info', '--title=Setup undone', '--text=Original settings were restored. '
            'You can now remove the package; cached contacts and history were retained.')
        return 0
    if sys.argv[2:]:
        return 1
    if (state / 'setup-undo-pending.json').exists():
        run('zenity', '--error', '--title=Undo is unfinished', '--text=End all calls and finish Undo before opening the app.')
        return 1
    complete = state / 'setup-complete.json'
    if complete.exists():
        try:
            saved = json.loads(complete.read_text())
            if all(saved.get(key) == value for key, value in package_identity(Path(sys.argv[1])).items()):
                return 0
        except (ValueError, OSError, AttributeError):
            pass
    consent = run('zenity', '--question', '--title=Set up IBlueConnect',
                  '--ok-label=Allow and set up', '--cancel-label=Cancel',
                  '--text=Allow IBlueConnect to run quietly at login, show incoming call controls, '
                  'configure phone call audio and noise reduction, and automatically sync contacts '
                  'and call history from your manually connected phone? Phone sharing permission '
                  'is also required. Setup may briefly interrupt audio. End all calls and disconnect '
                  'all Bluetooth devices, including phones and headsets, in Settings before continuing. Your default microphone stays unchanged.')
    if consent.returncode:
        return 2
    try:
        configure(Path(sys.argv[1]), state)
    except (RuntimeError, ValueError, OSError, subprocess.TimeoutExpired) as error:
        message = str(error) if isinstance(error, RuntimeError) else 'Setup could not verify system state. Please retry after ending all calls.'
        run('zenity', '--error', '--title=IBlueConnect setup paused', '--text=' + message)
        return 1
    run('zenity', '--info', '--title=IBlueConnect is ready', '--text=Connect your phone manually in Bluetooth settings. '
        'On iPhone, enable Sync Contacts for this laptop. Arrange a test call and select the laptop for call audio.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
