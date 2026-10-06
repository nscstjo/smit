#!/usr/bin/env python3
"""Inspect output TS scrambling and CA signalling, without changing the stream."""
import collections
import json
import sys
from pathlib import Path


def analyze(path, tables):
    pids = collections.defaultdict(lambda: {"tsc": [0, 0, 0, 0], "tei": 0,
                                          "payload_tsc": [0, 0, 0, 0],
                                          "first_clear_pes_packet": None,
                                          "last_scrambled_payload_packet": None,
                                          "clear_pes_starts": 0, "bad_clear_pes_starts": 0})
    windows = collections.defaultdict(lambda: [0, 0, 0, 0])
    with open(path, 'rb') as source:
        index = 0
        while packet := source.read(188):
            if len(packet) != 188 or packet[0] != 0x47:
                raise ValueError('Invalid packet alignment')
            pid = (packet[1] & 31) << 8 | packet[2]
            tsc = packet[3] >> 6
            pids[pid]['tsc'][tsc] += 1
            windows[index // 25000][tsc] += 1
            index += 1
            if packet[1] & 0x80:
                pids[pid]['tei'] += 1
                continue
            afc = packet[3] >> 4 & 3
            offset = 5 + packet[4] if afc & 2 else 4
            if afc & 1 and offset < 188:
                pids[pid]['payload_tsc'][tsc] += 1
                if tsc in (2, 3):
                    pids[pid]['last_scrambled_payload_packet'] = index - 1
            if tsc == 0 and afc & 1 and packet[1] & 0x40 and offset + 3 <= 188:
                field = 'clear_pes_starts' if packet[offset:offset+3] == b'\0\0\1' else 'bad_clear_pes_starts'
                pids[pid][field] += 1
                if field == 'clear_pes_starts' and pids[pid]['first_clear_pes_packet'] is None:
                    pids[pid]['first_clear_pes_packet'] = index - 1
    services = []
    for program in tables['pmt']:
        services.append({**program, 'streams': [{**s, **pids[s['pid']]} for s in program['streams']]})
    return {'file': str(path), 'tsc_order': ['clear', 'reserved', 'even_key', 'odd_key'],
            'services': services, 'pid_stats': dict(pids), 'windows_25000_packets': dict(windows),
            'cat': tables.get('cat', []), 'sdt': tables['services'],
            'note': 'CA descriptors/free_ca_mode describe broadcast signalling, not proof of scrambled output. Clear TSC and PES headers alone do not prove successful decoding.'}


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('Usage: analyze-ca.py input.ts input-report.json')
    tables = json.loads(Path(sys.argv[2]).read_text())
    print(json.dumps(analyze(sys.argv[1], tables), ensure_ascii=False, indent=2))
