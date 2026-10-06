#!/usr/bin/env python3
import importlib.util
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('monitor', Path(__file__).resolve().parents[1] / 'tools/client/monitor_ts.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


def packet(pid=32, cc=0, afc=1, scrambled=False):
    p = bytearray([0xff] * 188)
    p[:4] = bytes([0x47, pid >> 8, pid & 255, afc << 4 | cc | (0x80 if scrambled else 0)])
    if afc & 2:
        p[4:6] = bytes([183 if afc == 2 else 1, 0])
    return bytes(p)


class Checks(unittest.TestCase):
    def test_wrap_and_adaptation_only(self):
        m = mod.Monitor([32])
        for p in (packet(cc=15), packet(cc=15, afc=2), packet(cc=0)):
            m.packet(p)
        self.assertFalse(m.errors)

    def test_gap_duplicate_and_discontinuity(self):
        m = mod.Monitor([32])
        for p in (packet(cc=0), packet(cc=2), packet(cc=2)):
            m.packet(p)
        self.assertEqual(m.errors, {'continuity': 1, 'duplicate': 1})
        p = bytearray(packet(cc=9, afc=3)); p[5] = 0x80
        m.packet(bytes(p)); m.packet(packet(cc=10))
        self.assertEqual(m.errors['continuity'], 1)
        self.assertEqual(m.errors['signalled_discontinuity'], 1)

    def test_other_service_scrambling_and_null_cc(self):
        m = mod.Monitor([32])
        m.packet(packet(pid=49, scrambled=True))
        m.packet(packet(pid=8191, cc=1)); m.packet(packet(pid=8191, cc=8))
        self.assertFalse(m.errors)
        m.packet(packet(scrambled=True))
        self.assertEqual(m.errors['target_scrambled'], 1)

    def test_malformed(self):
        m = mod.Monitor([32])
        m.packet(b'bad')
        p = bytearray(packet(afc=3)); p[4] = 184; p[1] |= 0x80
        m.packet(bytes(p))
        self.assertEqual(m.errors, {'sync_or_size': 1, 'tei': 1, 'afc_or_length': 1})


if __name__ == '__main__':
    unittest.main()
