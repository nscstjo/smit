import importlib.util
from pathlib import Path
import tempfile
import unittest


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).parents[1] / ('tools/client/' + name.replace('-', '_') + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


builder, analyzer = load('build-ca-pmt'), load('analyze-ca')
psi = load('analyze-ts')


def valid_pmt(data):
    body = bytearray(data)
    length = len(body) + 1
    body[1:3] = (0xb000 | length).to_bytes(2, 'big')
    return bytes(body) + psi.crc32_mpeg(body).to_bytes(4, 'big')


class CaTest(unittest.TestCase):
    def test_pmt_conversion(self):
        pmt = valid_pmt(bytes.fromhex('02 b0 18 00 73 c1 00 00 e0 20 f0 06 09 04 4a 02 e4 70 1b e0 20 f0 00'))
        result = builder.encode(pmt)
        self.assertEqual(result.hex(), '9f803212030073c1f0070109044a02e4701be020f000')

    def test_truncated_descriptors(self):
        pmt = valid_pmt(bytes.fromhex('02 b0 0d 00 73 c1 00 00 e0 20 ff ff'))
        with self.assertRaises(ValueError):
            builder.encode(pmt)

    def test_crc_length_and_service(self):
        pmt = valid_pmt(bytes.fromhex('02 b0 12 00 73 c1 00 00 e0 20 f0 00 1b e0 20 f0 00'))
        self.assertEqual(builder.crc32_mpeg(b'123456789'), 0x0376e6e7)
        for bad in (pmt[:-1], pmt + b'\x00', pmt[:-1] + bytes([pmt[-1] ^ 1])):
            with self.assertRaises(ValueError):
                builder.encode(bad)
        with self.assertRaisesRegex(ValueError, 'service mismatch'):
            builder.encode(pmt, expected_service=116)

    def test_future_and_multisection(self):
        base = bytearray.fromhex('02 b0 12 00 73 c1 00 00 e0 20 f0 00 1b e0 20 f0 00')
        for offset, value in ((5, 0xc0), (6, 1), (7, 1)):
            bad = base.copy()
            bad[offset] = value
            with self.assertRaisesRegex(ValueError, 'current and single-section'):
                builder.encode(valid_pmt(bad))

    def test_invalid_ca_descriptor(self):
        body = bytes.fromhex('02 b0 00 00 73 c1 00 00 e0 20 f0 03 09 01 00 1b e0 20 f0 00')
        with self.assertRaisesRegex(ValueError, 'Short CA descriptor'):
            builder.encode(valid_pmt(body))

    def test_large_capmt_explicit_error(self):
        desc = b'\x80\xfa' + bytes(250)
        body = bytes.fromhex('02 b0 00 00 73 c1 00 00 e0 20') + (0xf000 | len(desc)).to_bytes(2, 'big') + desc
        with self.assertRaisesRegex(ValueError, 'transport limit'):
            builder.encode(valid_pmt(body))

    def test_scrambling_vs_pes(self):
        def packet(tsc, payload):
            return bytes([0x47, 0x40, 32, 0x10 | (tsc << 6)]) + payload.ljust(184, b'\xff')
        data = packet(2, b'\0\0\1') + packet(0, b'\0\0\1\xe0') + packet(0, b'bad')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.ts'
            path.write_bytes(data)
            report = analyzer.analyze(path, {'pmt': [{'service': 115, 'streams': [{'pid': 32}]}], 'services': []})
        stream = report['services'][0]['streams'][0]
        self.assertEqual(stream['tsc'], [2, 0, 1, 0])
        self.assertEqual(stream['clear_pes_starts'], 1)
        self.assertEqual(stream['bad_clear_pes_starts'], 1)
        self.assertEqual(stream['payload_tsc'], [2, 0, 1, 0])
        self.assertEqual(stream['first_clear_pes_packet'], 1)
        self.assertEqual(stream['last_scrambled_payload_packet'], 0)

    def test_adaptation_only_is_not_clear_payload(self):
        packet = bytes([0x47, 0, 32, 0x20, 183, 0]) + bytes(182)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.ts'
            path.write_bytes(packet)
            report = analyzer.analyze(path, {'pmt': [], 'services': []})
        self.assertEqual(report['pid_stats'][32]['tsc'], [1, 0, 0, 0])
        self.assertEqual(report['pid_stats'][32]['payload_tsc'], [0, 0, 0, 0])
        self.assertIsNone(report['pid_stats'][32]['first_clear_pes_packet'])


if __name__ == '__main__':
    unittest.main()
