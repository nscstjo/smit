"""Deterministic client regression: preserve DVR bytes and handle finite reads."""
import argparse
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import urllib.error


spec = importlib.util.spec_from_file_location('smit_client',
                                            Path(__file__).parents[1] / 'tools/smit.py')
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)


def packet(pid, cc=0, scrambled=0):
    return bytes((0x47, pid >> 8, pid & 255, 0x10 | scrambled << 6 | cc)) + bytes(range(184))


class FragmentedStream:
    def __init__(self, data, fragments, cutoff=None, failure=None):
        self.data = data
        self.fragments = iter(fragments)
        self.cutoff = len(data) if cutoff is None else cutoff
        self.failure = failure
        self.position = 0
        self.read_sizes = []

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False

    def read1(self, maximum):
        self.read_sizes.append(maximum)
        if self.failure:
            raise self.failure
        count = min(maximum, next(self.fragments, maximum))
        data = self.data[self.position:self.position + count]
        self.position += len(data)
        return data

    def clock(self):
        return 100.0 if self.position < self.cutoff else 102.0


class ClientTest(unittest.TestCase):
    def test_nonfinite_duration_rejected_before_network(self):
        for seconds in (float('nan'), float('inf'), -float('inf')):
            args = argparse.Namespace(seconds=seconds, pid=[])
            with mock.patch.object(client.urllib.request, 'urlopen') as opener:
                with self.assertRaises(ValueError):
                    client.capture(args)
                opener.assert_not_called()

    def test_nonobject_cli_json_rejected_before_network(self):
        for document in ('null', '[]', '1', 'true', '"text"'):
            with mock.patch.object(sys, 'argv', ['smit.py', 'call', '/health', document]), \
                    mock.patch.object(client.urllib.request, 'urlopen') as opener:
                with self.assertRaises(ValueError):
                    client.main()
                opener.assert_not_called()

    def test_clear_verification_requires_clean_payload(self):
        adaptation_only = bytearray(packet(32))
        adaptation_only[3] = 0x20
        adaptation_only[4] = 183
        adaptation_only[5] = 0
        tei = bytearray(packet(32))
        tei[1] |= 0x80
        empty_payload = bytearray(adaptation_only)
        empty_payload[3] = 0x30
        for data in (bytes(adaptation_only), bytes(tei), bytes(empty_payload)):
            result, report, saved = self.capture(FragmentedStream(data, [188]), output=True)
            self.assertEqual(result, 1)
            self.assertFalse(report['clear_verified'])
            self.assertEqual(report['clear_payload_packets']['32'], 0)
            self.assertEqual(saved, data)

    def capture(self, stream, output=False, targets=(32,)):
        with tempfile.TemporaryDirectory() as directory:
            report_path = Path(directory) / 'report.json'
            output_path = Path(directory) / 'original.ts'
            args = argparse.Namespace(seconds=1, pid=list(targets), report=str(report_path),
                                      output=str(output_path) if output else None,
                                      url='http://offline-test.invalid:38212')
            with mock.patch.object(client.urllib.request, 'urlopen', return_value=stream), \
                    mock.patch.object(client.time, 'monotonic', side_effect=stream.clock), \
                    mock.patch.object(client, 'awake', side_effect=contextlib.nullcontext), \
                    contextlib.redirect_stdout(io.StringIO()):
                result = client.capture(args)
            return result, json.loads(report_path.read_text()), output_path.read_bytes() if output else None

    def test_raw_output_is_identical_including_other_scrambled_pids(self):
        original = packet(32, 0) + packet(49, 0, 2) + packet(32, 1)
        stream = FragmentedStream(original, [1, 100, 188, 275])
        result, report, saved = self.capture(stream, output=True)
        self.assertEqual(result, 0)
        self.assertEqual(saved, original)
        self.assertEqual(report['bytes'], len(original))
        self.assertEqual(report['packets'], 3)
        self.assertEqual(report['scrambled'], {'49': 1})
        self.assertTrue(report['clear_verified'])
        self.assertEqual(report['trailing_bytes'], 0)

    def test_time_cutoff_finishes_only_current_packet(self):
        original = packet(32, 0) + packet(32, 1) + packet(32, 2)
        stream = FragmentedStream(original, [205, 171, 188], cutoff=205)
        result, report, saved = self.capture(stream, output=True)
        self.assertEqual(result, 0)
        self.assertEqual(saved, original[:376])
        self.assertEqual(report['packets'], 2)
        self.assertEqual(report['trailing_bytes'], 0)
        self.assertEqual(stream.read_sizes, [188 * 256, 171])

    def test_eof_before_duration_is_failure(self):
        stream = FragmentedStream(packet(32), [188], cutoff=1000)
        result, report, _ = self.capture(stream)
        self.assertEqual(result, 1)
        self.assertIn('ended stream', report['failure'])

    def test_eof_mid_packet_is_failure(self):
        stream = FragmentedStream(packet(32)[:101], [101], cutoff=101)
        result, report, _ = self.capture(stream)
        self.assertEqual(result, 1)
        self.assertEqual(report['trailing_bytes'], 101)
        self.assertIsNotNone(report['failure'])

    def test_stream_read_error_is_reported(self):
        stream = FragmentedStream(b'', [], cutoff=1, failure=OSError('offline read error'))
        result, report, _ = self.capture(stream)
        self.assertEqual(result, 1)
        self.assertEqual(report['failure'], 'offline read error')

    def test_target_scrambling_is_failure_and_keeps_original_bytes(self):
        original = packet(32, scrambled=2)
        result, report, saved = self.capture(FragmentedStream(original, [188]), output=True)
        self.assertEqual(result, 1)
        self.assertEqual(saved, original)
        self.assertFalse(report['clear_verified'])
        self.assertEqual(report['errors']['target_scrambled'], 1)

    def test_continuity_diagnostic_has_packet_and_pid(self):
        data = packet(32, 0) + packet(32, 4)
        result, report, _ = self.capture(FragmentedStream(data, [376]))
        self.assertEqual(result, 1)
        self.assertEqual(report['continuity_samples'], [{'packet': 2, 'pid': 32}])

    def test_error_json_from_http_is_preserved(self):
        document = {'ok': False, 'errno': 16, 'error': 'one raw stream is already active'}
        error = urllib.error.HTTPError('http://offline-test.invalid/stream', 409,
                                       'Conflict', {}, io.BytesIO(json.dumps(document).encode()))
        with mock.patch.object(client.urllib.request, 'urlopen', side_effect=error):
            status, response = client.call('http://offline-test.invalid', '/stream')
        self.assertEqual(status, 409)
        self.assertEqual(response, document)

    def test_call_failure_sets_exit_code(self):
        with mock.patch.object(sys, 'argv', ['smit.py', 'call', '/frontend/info', '{}']), \
                mock.patch.object(client, 'call', return_value=(502, {'ok': False, 'errno': 19})), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(client.main(), 1)

    def test_existing_output_is_protected_before_network_access(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'existing.ts'
            output.write_bytes(b'existing recording')
            args = argparse.Namespace(seconds=1, pid=[], report=None, output=str(output),
                                      url='http://offline-test.invalid')
            with mock.patch.object(client.urllib.request, 'urlopen') as open_stream:
                with self.assertRaises(FileExistsError):
                    client.capture(args)
                open_stream.assert_not_called()
            self.assertEqual(output.read_bytes(), b'existing recording')


if __name__ == '__main__':
    unittest.main()
