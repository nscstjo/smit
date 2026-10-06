import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("analyzer", Path(__file__).parents[1] / "tools/client/analyze_ts.py")
analyzer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analyzer)


def section(hex_string):
    data = bytes.fromhex(hex_string)
    return data + analyzer.crc32_mpeg(data).to_bytes(4, "big")


def packet(pid, counter, data, start=True):
    header = bytes([0x47, (pid >> 8) | (0x40 if start else 0), pid & 255, 0x30 | counter])
    size = 183 - len(data)
    return header + bytes([size]) + (bytes([0]) + b'\xff' * (size - 1) if size else b'') + data


class AnalyzerTest(unittest.TestCase):
    def report(self, data):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "fixture.ts"
            path.write_bytes(data)
            return analyzer.analyze(path)

    def test_crc_known_vector(self):
        self.assertEqual(analyzer.crc32_mpeg(b'123456789'), 0x0376e6e7)

    def test_pat_pmt_and_fragment(self):
        pat = section('00 b0 0d 00 01 c1 00 00 00 01 e1 00')
        pmt = section('02 b0 12 00 01 c1 00 00 e1 01 f0 00 1b e1 01 f0 00')
        data = packet(0, 0, b'\0' + pat[:7]) + packet(0, 1, pat[7:], False)
        data += packet(256, 0, b'\0' + pmt)
        report = self.report(data)
        self.assertEqual(report['programs'], {1: 256})
        self.assertEqual(report['pmt'][0]['streams'], [{'type': 27, 'pid': 257, 'ca': []}])
        self.assertEqual(report['errors'], {})
        self.assertEqual(report['psi_crc_failures'], {})

    def test_bad_crc(self):
        pat = bytearray(section('00 b0 0d 00 01 c1 00 00 00 01 e1 00'))
        pat[-1] ^= 1
        report = self.report(packet(0, 0, b'\0' + pat))
        self.assertEqual(report['psi_crc_failures'], {0: 1})
        self.assertEqual(report['programs'], {})

    def test_duplicate_and_gap(self):
        first = packet(100, 0, b'abc', False)
        report = self.report(first + first + packet(100, 3, b'def', False))
        self.assertEqual(report['errors'], {'duplicate_packets': 1, 'continuity': 1})

    def test_bcd_cable_units(self):
        self.assertEqual(analyzer.bcd(bytes.fromhex('04 74 00 00'), 8) * 100, 474000000)
        self.assertEqual(analyzer.bcd(bytes.fromhex('00 68 75 0f'), 7) * 100, 6875000)


if __name__ == '__main__':
    unittest.main()
