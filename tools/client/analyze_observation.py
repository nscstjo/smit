#!/usr/bin/env python3
"""Summarize observe.sh/check-no-device.sh logs without claiming leak freedom."""
import argparse
import json
import re
import statistics
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('logs', nargs='+', type=Path)
args = parser.parse_args()
metrics = ('MemAvailable', 'Slab', 'SReclaimable', 'SUnreclaim', 'KernelStack', 'VmallocUsed')
reports = []
for path in args.logs:
    text = path.read_text(encoding='utf-8-sig', errors='replace').replace('\x00', '')
    observations, cycles = [], []
    for line in text.splitlines():
        if not (line.startswith('sample uptime=') or line.startswith('cycle=')):
            continue
        values = dict(re.findall(r'(\w+)[=:]([^ ]+)', line))
        if not all(name in values for name in (*metrics, 'uptime', 'taint')):
            raise SystemExit(f'Incomplete sample in {path}: {line}')
        row = {name: int(values[name]) for name in (*metrics, 'uptime', 'taint')}
        if 'elapsed' in values:
            row['elapsed'] = int(values['elapsed'])
            observations.append(row)
        else:
            row['cycle'] = values['cycle']
            cycles.append(row)
    report = {'file': str(path), 'observation_complete': '=== observation complete ===' in text,
              'cycles_complete': '=== no-device cycles complete ===' in text,
              'observation_samples': len(observations),
              'measured_cycles': sum(row['cycle'].isdigit() for row in cycles)}
    report['duration_seconds'] = observations[-1]['elapsed'] if observations else 0
    report['taint_values'] = sorted({row['taint'] for row in observations + cycles})
    report['observation_kib'] = {}
    for name in metrics:
        if not observations:
            break
        values = [row[name] for row in observations]
        first_window = [row[name] for row in observations if row['elapsed'] <= 60]
        last_window = [row[name] for row in observations if row['elapsed'] >= report['duration_seconds'] - 60]
        report['observation_kib'][name] = {
            'first': values[0], 'last': values[-1], 'minimum': min(values), 'maximum': max(values),
            'first_60s_median': statistics.median(first_window),
            'last_60s_median': statistics.median(last_window),
            'median_delta': statistics.median(last_window) - statistics.median(first_window)}
    report['cycle_memory_kib'] = [row for row in cycles if row['cycle'] in ('loaded', '10', '100', 'unloaded-settled')]
    # Includes historical dmesg too; findings require baseline comparison.
    report['kernel_error_candidates'] = sorted(set(re.findall(
        r'^.*(?:BUG:|Oops:|Kernel panic|Out of memory:|oom-kill:|hung task|WARNING: CPU:|Unknown symbol).*$',
        text, re.MULTILINE)))
    reports.append(report)
print(json.dumps(reports, indent=2, ensure_ascii=False))
