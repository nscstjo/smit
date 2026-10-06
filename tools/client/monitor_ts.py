#!/usr/bin/env python3
"""Bounded-memory TS soak monitor. stdin TS; minute JSONL plus final JSON.

Checks packet transport integrity, not video decoding or complete SI semantics.
Keeps only first/last ~1 MiB of packets for later inspection.
"""
import collections
import json
import sys
import time
from pathlib import Path


class Monitor:
    def __init__(self, targets):
        self.targets = set(targets)
        self.packets = 0
        self.counts = collections.Counter()
        self.errors = collections.Counter()
        self.scrambled = collections.Counter()
        self.previous = {}

    def packet(self, p):
        self.packets += 1
        if len(p) != 188 or p[0] != 0x47:
            self.errors['sync_or_size'] += 1
            return
        pid = (p[1] & 31) << 8 | p[2]
        self.counts[pid] += 1
        if p[1] & 0x80:
            self.errors['tei'] += 1
        if p[3] & 0xc0:
            self.scrambled[pid] += 1
            if pid in self.targets:
                self.errors['target_scrambled'] += 1
        afc, cc = p[3] >> 4 & 3, p[3] & 15
        if not afc or (afc & 2 and 5 + p[4] > 188):
            self.errors['afc_or_length'] += 1
            self.previous.pop(pid, None)
            return
        discontinuity = afc & 2 and p[4] and p[5] & 0x80
        if discontinuity:
            self.errors['signalled_discontinuity'] += 1
            self.previous.pop(pid, None)
        if pid == 8191:
            return
        prior = self.previous.get(pid)
        if prior is not None:
            if p == prior and afc & 1:
                self.errors['duplicate'] += 1
            elif cc != ((prior[3] & 15) + bool(afc & 1)) & 15:
                self.errors['continuity'] += 1
        self.previous[pid] = p

    def report(self, elapsed):
        return dict(elapsed_seconds=round(elapsed, 3), packets=self.packets,
                    bytes=self.packets * 188, pid_packets=dict(self.counts),
                    scrambled_by_pid=dict(self.scrambled), errors=dict(self.errors))


def main():
    if len(sys.argv) < 3:
        raise SystemExit('Usage: monitor-ts.py output_prefix target_pid ... < input.ts')
    prefix = sys.argv[1]
    m = Monitor(map(int, sys.argv[2:]))
    start = last_report = time.monotonic()
    carry = b''
    tail = collections.deque(maxlen=32)
    first_bytes = 0
    with open(prefix + '-minutes.jsonl', 'x') as reports, open(prefix + '-first.ts', 'xb') as first:
        while True:
            chunk = sys.stdin.buffer.read1(188 * 256)
            if not chunk:
                break
            data = carry + chunk
            end = len(data) // 188 * 188
            aligned, carry = data[:end], data[end:]
            for pos in range(0, end, 188):
                m.packet(aligned[pos:pos + 188])
            if aligned:
                tail.append(aligned)
                if first_bytes < 1024 * 1024:
                    first.write(aligned)
                    first_bytes += len(aligned)
            current = time.monotonic()
            if current - last_report >= 60:
                reports.write(json.dumps(m.report(current - start)) + '\n')
                reports.flush()
                last_report = current
        if carry:
            m.errors['trailing_bytes'] += len(carry)
        final = m.report(time.monotonic() - start)
        reports.write(json.dumps(final) + '\n')
    Path(prefix + '-last.ts').write_bytes(b''.join(tail))
    Path(prefix + '-summary.json').write_text(json.dumps(final, indent=2) + '\n')
    print(json.dumps(final))
    return int(bool(m.errors) or not all(m.counts[p] for p in m.targets))


if __name__ == '__main__':
    raise SystemExit(main())
