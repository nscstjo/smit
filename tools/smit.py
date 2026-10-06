#!/usr/bin/env python3
"""SMIT iCast USB-C/DTMB USB Tuner — HTTP client and offline diagnostics."""
import argparse
import contextlib
import importlib.util
import json
from pathlib import Path
import runpy
import sys
import time
import urllib.error
import urllib.request

LIB = Path(__file__).resolve().parent / 'client'
OFFLINE = {'analyze-ts': 'analyze_ts', 'analyze-ca': 'analyze_ca',
           'monitor-ts': 'monitor_ts', 'build-ca-pmt': 'build_ca_pmt',
           'usbmon-pcap': 'usbmon_pcap', 'inspect-sas': 'inspect_sas',
           'analyze-observation': 'analyze_observation'}


def load(name):
    spec = importlib.util.spec_from_file_location(name, LIB / (name + '.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


call = load("http_client").call
_recording = load("recording")
awake = _recording.awake


def capture(args):
    return _recording.capture(args, awake_factory=awake)


def main():
    if len(sys.argv) > 1 and sys.argv[1] in OFFLINE:
        command = sys.argv.pop(1)
        runpy.run_path(str(LIB / (OFFLINE[command] + '.py')), run_name='__main__')
        return 0
    p = argparse.ArgumentParser(description=__doc__, epilog='Offline commands: ' + ', '.join(OFFLINE))
    p.add_argument('--url', default='http://127.0.0.1:38212')
    sub = p.add_subparsers(dest='command', required=True)
    c = sub.add_parser('call', help='GET without JSON; POST with JSON')
    c.add_argument('path')
    c.add_argument('json', nargs='?')
    s = sub.add_parser('stream', help='capture and/or monitor the original dvr0 bytes')
    s.add_argument('--seconds', type=float, default=30)
    s.add_argument('--output', help='new TS file; omitted means bounded-memory monitoring')
    s.add_argument('--report', help='new JSON report file')
    s.add_argument('--pid', type=int, action='append', default=[], help='target PID that must remain clear')
    args = p.parse_args()
    if args.command == 'call':
        data = json.loads(args.json) if args.json is not None else None
        if args.json is not None and not isinstance(data, dict):
            raise ValueError('POST JSON must be an object')
        status, result = call(args.url, args.path, data)
        print(json.dumps(result, indent=2, ensure_ascii=False))
        return 0 if status == 200 and result.get('ok') else 1
    return capture(args)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, OSError, urllib.error.URLError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
