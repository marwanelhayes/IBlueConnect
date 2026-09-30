"""Measure binaries on a private D-Bus/Xvfb session with mock UI data, never a phone."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def memory(process):
    if process.poll() is not None:
        raise RuntimeError('A measured process exited unexpectedly.')
    fields = {}
    for line in Path(f'/proc/{process.pid}/smaps_rollup').read_text().splitlines():
        key, _, value = line.partition(':')
        if key in ('Rss', 'Pss'):
            fields[key.lower() + '_kib'] = int(value.split()[0])
    assert fields.keys() == {'rss_kib', 'pss_kib'}
    return fields


def stop(process):
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


daemon_path, ui_path, output_path = map(Path, sys.argv[1:])
environment = os.environ | {'GDK_BACKEND': 'x11', 'GSK_RENDERER': 'cairo',
                            'GTK_A11Y': 'none', 'GSETTINGS_BACKEND': 'memory'}
record = {'method': '/proc/PID/smaps_rollup; five samples per scenario at two-second intervals',
          'environment': 'isolated Ubuntu container, private D-Bus and Xvfb, Cairo renderer, mock contacts/calls, no connected phone',
          'scenarios': {}}
with tempfile.TemporaryDirectory(prefix='iblueconnect-memory-') as data:
    environment['XDG_DATA_HOME'] = data
    daemon = subprocess.Popen([str(daemon_path.resolve())], env=environment,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ui = None
    try:
        time.sleep(2)
        for scenario, args in [('background', None), ('main_window', ['--mock']),
                               ('call_controller', ['--preview=active'])]:
            if args:
                ui = subprocess.Popen([str(ui_path.resolve()), *args], env=environment,
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                time.sleep(2)
            samples = []
            for _ in range(5):
                row = {'daemon': memory(daemon)}
                if ui:
                    row['ui'] = memory(ui)
                row['total'] = {key: sum(values[key] for values in row.values())
                                for key in ('rss_kib', 'pss_kib')}
                samples.append(row)
                time.sleep(2)
            record['scenarios'][scenario] = samples
            stop(ui)
            ui = None
    finally:
        stop(ui)
        stop(daemon)
Path(output_path).write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps({scenario: {'rss_mib': round(samples[-1]['total']['rss_kib'] / 1024, 1),
                             'pss_mib': round(samples[-1]['total']['pss_kib'] / 1024, 1)}
                  for scenario, samples in record['scenarios'].items()}))
