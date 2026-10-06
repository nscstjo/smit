import importlib.util
import io
import struct
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('pcap', Path(__file__).parents[1] / 'tools/client/usbmon_pcap.py')
pcap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pcap)


def capture(endian='<', payload=b'abc', declared=3):
    header = struct.pack(endian + 'IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 262144, 220)
    usb = struct.pack(endian + 'QBBBBHBBqiiII', 1, ord('C'), 3, 130, 3, 3, 45, 0, 1, 2, 0, declared, len(payload)) + bytes(24) + payload
    return header + struct.pack(endian + 'IIII', 1, 2, len(usb), len(usb)) + usb


class PcapTest(unittest.TestCase):
    def test_endian_and_full_payload(self):
        for endian in ('<', '>'):
            rows = list(pcap.convert(io.BytesIO(capture(endian)), 3, 3))
            self.assertEqual(rows, ['1 1000002 C Bi:3:003:2 0 3 = 616263'])

    def test_device_filter(self):
        self.assertEqual(list(pcap.convert(io.BytesIO(capture()), 3, 4)), [])

    def test_truncated(self):
        for size in (3, 25, 50, len(capture()) - 1):
            with self.assertRaises(ValueError):
                list(pcap.convert(io.BytesIO(capture()[:size]), 3, 3))

    def test_missing_usb_bytes_stay_missing(self):
        row = list(pcap.convert(io.BytesIO(capture(declared=10)), 3, 3))[0]
        self.assertIn('0 10 = 616263', row)


if __name__ == '__main__':
    unittest.main()
