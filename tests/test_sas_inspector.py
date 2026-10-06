#!/usr/bin/env python3
"""Offline synthetic boundary checks; no USB access."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('inspector', Path(__file__).resolve().parents[1] / 'tools/client/inspect_sas.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def frame(counter=255, payload=b'GSTA'):
    msg = b'\x00\x05' + len(payload).to_bytes(2, 'big') + payload
    body = bytes([counter]) + len(msg).to_bytes(2, 'big') + msg
    length = bytes([len(body)]) if len(body) < 128 else bytes([129, len(body)])
    app = b'\x01\x90\x02\x00\x03\x9f\x9a\x07' + length + body
    outer_length = bytes([len(app)]) if len(app) < 128 else bytes([129, len(app)])
    return b'\x01\x00\xa0' + outer_length + app


def line(data, declared=None, kind='S Bo', status='-115'):
    event, pipe = kind.split()
    return f'abc 123 {event} {pipe}:3:7:1 {status} {declared or len(data)} = {data.hex()}'


class InspectorTest(unittest.TestCase):
    def test_counter_and_lengths(self):
        got = module.inspect(line(frame()))
        self.assertEqual((got['counter_byte'], got['message_length'], got['data_length']), (255, 8, 4))
        self.assertEqual(got['errors'], [])

    def test_wrap_zero(self):
        self.assertEqual(module.inspect(line(frame(0)))['counter_byte'], 0)

    def test_truncated_capture(self):
        data = frame(payload=b'x'*16)
        got = module.inspect(line(data[:24], len(data)))
        self.assertTrue(got['capture_truncated'])
        self.assertFalse(got['apdu_complete'])
        self.assertEqual(len(got['data_hex']), 8)

    def test_ber_long(self):
        got = module.inspect(line(frame(payload=b'x'*144)))
        self.assertTrue(got['apdu_complete'])
        self.assertEqual(got['data_hex'], (b'x'*144).hex())
        self.assertEqual(got['errors'], [])

    def test_mismatched_nested_length(self):
        data = bytearray(frame())
        data[15] = 9
        self.assertIn('apdu_message_length_mismatch', module.inspect(line(data))['errors'])

    def test_wrong_direction_or_failed_transfer(self):
        self.assertIsNone(module.inspect(line(frame(), kind='C Bo', status='0')))
        self.assertIsNone(module.inspect(line(frame(), kind='C Bi', status='-71')))

    def test_short_and_indefinite(self):
        data = bytearray(frame())
        data[12] = 128
        self.assertEqual(module.inspect(line(data))['error'], 'invalid_or_truncated_ber')
        self.assertEqual(module.inspect(line(frame()[:16]))['error'], 'short_sas_header')

    def test_incoming(self):
        self.assertEqual(module.inspect(line(frame(), kind='C Bi', status='0'))['direction'], 'in')


if __name__ == '__main__':
    unittest.main()
