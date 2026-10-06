#!/usr/bin/env python3
"""Offline HTTP/JSON regression against smit --mock (no DVB hardware).

Usage: python3 tests/test_http_server.py --binary build/ubuntu-x86_64/smit
       python3 tests/test_http_server.py --url http://127.0.0.1:38212
The URL mode requires an explicitly started mock server; /health verifies it.
"""
import argparse
import http.client
import json
from pathlib import Path
import signal
import socket
import subprocess
import time
import unittest
from urllib.parse import urlsplit


HOST = '127.0.0.1'
PORT = None
PROCESS = None


def request(method, path, body=None, raw=None):
    connection = http.client.HTTPConnection(HOST, PORT, timeout=4)
    try:
        payload = raw if raw is not None else (json.dumps(body) if body is not None else None)
        connection.request(method, path, payload, {'Content-Type': 'application/json'})
        response = connection.getresponse()
        data = response.read()
        document = json.loads(data)
        return response.status, response.getheader('Content-Type', ''), document
    finally:
        connection.close()


class ServerTest(unittest.TestCase):
    def success(self, path, body=None, method='POST'):
        status, content_type, data = request(method, path, {} if body is None and method == 'POST' else body)
        self.assertEqual(status, 200, data)
        self.assertIn('application/json', content_type)
        self.assertIs(data.get('ok'), True, data)
        return data

    def failure(self, path, body=None, raw=None, method='POST', status=None):
        actual, content_type, data = request(method, path, {} if body is None and method == 'POST' else body, raw)
        self.assertGreaterEqual(actual, 400, data)
        if status is not None:
            self.assertEqual(actual, status, data)
        self.assertIn('application/json', content_type)
        self.assertIs(data.get('ok'), False, data)
        self.assertIsInstance(data.get('error'), str)
        self.assertTrue(data['error'])
        self.assertIs(type(data.get('errno')), int)
        self.assertGreater(data['errno'], 0)
        return data

    def test_discovery(self):
        health = self.success('/health', method='GET')
        self.assertIs(health.get('mock'), True, 'Refusing tests against real DVB hardware')
        self.success('/api', method='GET')

    def test_frontend(self):
        self.success('/frontend/info')
        self.success('/frontend/status')
        self.success('/frontend/tune', {'frequency': 147000000, 'symbol_rate': 6875000})
        self.success('/frontend/tune', {'frequency': 474000000, 'symbol_rate': 6875000,
                                       'modulation': 64})
        self.success('/frontend/status')

    def test_ca(self):
        self.success('/ca/caps')
        self.success('/ca/slot')
        self.success('/ca/slot', {'slot': 0})
        self.success('/ca/send', {'hex': '9f803000'})
        response = self.success('/ca/receive')
        if 'hex' in response:
            self.assertEqual(len(response['hex']) % 2, 0)
            bytes.fromhex(response['hex'])
        self.failure('/ca/reset')
        self.failure('/ca/reset', {'confirm': 0})
        self.success('/ca/reset', {'confirm': 1})

    def test_demux_and_service(self):
        response = self.success('/demux/section', {'pid': 0, 'table': 0, 'timeout_ms': 1000})
        if 'hex' in response:
            section = bytes.fromhex(response['hex'])
            self.assertGreaterEqual(len(section), 3)
            self.assertEqual(section[0], 0)
            self.assertEqual(len(section), 3 + ((section[1] & 15) << 8 | section[2]))
        self.success('/service/select', {'service': 115, 'timeout_ms': 3000})

    def test_json_rejection(self):
        malformed = ('', '[]', 'null', '{', '{} trailing', '{"a":1,}',
                     '{"frequency":-1,"symbol_rate":6875000}',
                     '{"frequency":1.0,"symbol_rate":6875000}',
                     '{"frequency":1e8,"symbol_rate":6875000}',
                     '{"frequency":0147000000,"symbol_rate":6875000}',
                     '{"frequency":true,"symbol_rate":6875000}',
                     '{"frequency":null,"symbol_rate":6875000}',
                     '{"frequency":{},"symbol_rate":6875000}',
                     '{"frequency":[1],"symbol_rate":6875000}',
                     '{"frequency":147000000,"frequency":474000000,"symbol_rate":6875000}',
                     '{"frequency":18446744073709551616,"symbol_rate":6875000}',
                     '{"frequency":"147000000","symbol_rate":6875000}')
        for payload in malformed:
            with self.subTest(payload=payload):
                self.failure('/frontend/tune', raw=payload)
        self.failure('/frontend/info', {'unknown': 1})
        self.failure('/frontend/tune', {'': 1, 'frequency': 147000000,
                                        'symbol_rate': 6875000})
        self.failure('/frontend/info', raw='{"x":"\u00e9"}')

    def test_parameter_bounds(self):
        invalid = (('/frontend/tune', {}),
                   ('/frontend/tune', {'frequency': 0, 'symbol_rate': 6875000}),
                   ('/frontend/tune', {'frequency': 147000000, 'symbol_rate': 0}),
                   ('/frontend/tune', {'frequency': 147000000, 'symbol_rate': 6875000,
                                       'modulation': 123}),
                   ('/ca/slot', {'slot': 256}),
                   ('/demux/section', {'pid': 8192, 'table': 0}),
                   ('/demux/section', {'pid': 0, 'table': 256}),
                   ('/demux/section', {'pid': 0, 'table': 0, 'timeout_ms': 0}),
                   ('/service/select', {'service': 0}),
                   ('/service/select', {'service': 65536}),
                   ('/ca/send', {}), ('/ca/send', {'hex': ''}),
                   ('/ca/send', {'hex': '9'}), ('/ca/send', {'hex': 'gg'}),
                   ('/ca/send', {'hex': '9f 80 30 00'}),
                   ('/ca/send', {'hex': 'aa' * 251}))
        for path, body in invalid:
            with self.subTest(path=path, body=body):
                self.failure(path, body)

    def test_paths_and_methods(self):
        self.failure('/not-an-api', method='GET', status=404)
        self.failure('/front', method='GET', status=404)
        self.failure('/', method='GET', status=404)
        self.failure('/frontend/info', method='GET', status=405)
        self.failure('/health', method='POST', status=405)
        self.failure('/frontend/info', method='PUT', status=405)
        self.failure('/stream', method='POST', status=405)

    def test_http_request_limits(self):
        requests = (
            b'POST /frontend/info HTTP/1.1\r\nHost: localhost\r\nContent-Length: -1\r\n\r\n',
            b'POST /frontend/info HTTP/1.1\r\nHost: localhost\r\nContent-Length: 99999999\r\n\r\n',
            b'POST /frontend/info HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n',
            b'POST /frontend/info HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\n{}',
            b'POST /frontend/info HTTP/1.1\r\nHost: localhost\r\nContent-Length: 20\r\n\r\n{}',
        )
        for wire in requests:
            with self.subTest(wire=wire):
                with socket.create_connection((HOST, PORT), timeout=4) as sock:
                    sock.sendall(wire)
                    sock.shutdown(socket.SHUT_WR)
                    response = http.client.HTTPResponse(sock)
                    response.begin()
                    self.assertGreaterEqual(response.status, 400)
                    data = json.loads(response.read())
                    self.assertIs(data.get('ok'), False)
                    self.assertIs(type(data.get('errno')), int)
                self.success('/health', method='GET')

    def test_fragmented_http(self):
        wire = (b'POST /frontend/info HTTP/1.1\r\nHost: localhost\r\n'
                b'Content-Length: 2\r\nContent-Type: application/json\r\n\r\n{}')
        with socket.create_connection((HOST, PORT), timeout=4) as sock:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            for offset in range(0, len(wire), 7):
                sock.sendall(wire[offset:offset + 7])
                time.sleep(0.005)
            response = http.client.HTTPResponse(sock)
            response.begin()
            self.assertEqual(response.status, 200)
            self.assertIs(json.loads(response.read())['ok'], True)

    def test_stream_raw_and_control(self):
        self.success('/stream/stop')
        connection = http.client.HTTPConnection(HOST, PORT, timeout=4)
        try:
            connection.request('GET', '/stream')
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertNotIn('application/json', response.getheader('Content-Type', ''))
            data = response.read(188 * 8)
            self.assertEqual(len(data), 188 * 8)
            for offset in range(0, len(data), 188):
                packet = data[offset:offset + 188]
                self.assertEqual(packet[0], 0x47)
                self.assertEqual((packet[1] & 31) << 8 | packet[2], 8191)
            self.success('/health', method='GET')
            self.success('/frontend/status')
            self.success('/stream/stats')
            self.failure('/stream', method='GET', status=409)
            self.failure('/frontend/tune', {'frequency': 147000000, 'symbol_rate': 6875000}, status=409)
            self.failure('/service/select', {'service': 115}, status=409)
            self.failure('/ca/reset', {'confirm': 1}, status=409)
            self.success('/stream/stop')
            self.success('/stream/stop')
        finally:
            connection.close()
        self.success('/frontend/tune', {'frequency': 147000000, 'symbol_rate': 6875000})

    def test_z_shutdown_cancels_pending_clients(self):
        if PROCESS is None:
            self.skipTest('External mock process is not owned by this test')
        self.success('/stream/stop')
        with socket.create_connection((HOST, PORT), timeout=4) as partial, socket.socket() as stalled:
            partial.sendall(b'POST /frontend/info HTTP/1.1\r\nHost: localhost\r\n')
            stalled.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            stalled.settimeout(4)
            stalled.connect((HOST, PORT))
            stalled.sendall(b'GET /stream HTTP/1.1\r\nHost: localhost\r\n\r\n')
            self.assertIn(b'200 OK', stalled.recv(512))
            self.success('/health', method='GET')
            start = time.monotonic()
            PROCESS.send_signal(signal.SIGTERM)
            PROCESS.wait(timeout=2)
            self.assertEqual(PROCESS.returncode, 0)
            self.assertLess(time.monotonic() - start, 2)

    def test_stream_disconnect_releases_slot(self):
        self.success('/stream/stop')
        connection = http.client.HTTPConnection(HOST, PORT, timeout=4)
        connection.request('GET', '/stream')
        response = connection.getresponse()
        self.assertEqual(response.status, 200)
        self.assertEqual(len(response.read(188)), 188)
        response.close()
        connection.close()
        deadline = time.monotonic() + 3
        while True:
            status, _, data = request('POST', '/frontend/tune',
                                      {'frequency': 147000000, 'symbol_rate': 6875000})
            if status == 200:
                self.assertIs(data['ok'], True)
                break
            if time.monotonic() >= deadline:
                self.fail('Disconnected stream retained its slot: ' + repr(data))
            time.sleep(0.05)
        self.success('/stream/stats')

    def test_stream_backpressure_is_bounded(self):
        self.success('/stream/stop')
        with socket.socket() as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            sock.settimeout(4)
            sock.connect((HOST, PORT))
            sock.sendall(b'GET /stream HTTP/1.1\r\nHost: localhost\r\n\r\n')
            # Leave the TCP window closed. The server must preserve control
            # responsiveness and eventually release the stalled output slot.
            deadline = time.monotonic() + 6
            observed = False
            while time.monotonic() < deadline:
                start = time.monotonic()
                stats = self.success('/stream/stats')
                self.assertLess(time.monotonic() - start, 1)
                observed = observed or stats['active']
                if observed and not stats['active']:
                    self.assertGreater(stats['errors'], 0)
                    self.assertGreater(stats['last_errno'], 0)
                    break
                time.sleep(0.05)
            else:
                self.fail('Stalled raw stream did not release its slot within six seconds')
        self.success('/frontend/tune', {'frequency': 147000000, 'symbol_rate': 6875000})


def main():
    global HOST, PORT, PROCESS
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--binary', type=Path)
    group.add_argument('--url', help='Explicit mock server URL; real hardware is refused')
    args = parser.parse_args()
    process = None
    try:
        if args.binary:
            with socket.socket() as allocation:
                allocation.bind(('127.0.0.1', 0))
                PORT = allocation.getsockname()[1]
            process = subprocess.Popen([str(args.binary.resolve()), '--mock', '--bind', HOST,
                                        '--port', str(PORT), '--adapter', '/nonexistent-smit-test'],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            PROCESS = process
        else:
            address = urlsplit(args.url)
            if address.scheme != 'http' or not address.hostname or address.path not in ('', '/'):
                parser.error('--url must be an http origin')
            HOST, PORT = address.hostname, address.port or 80
        deadline = time.monotonic() + 5
        while True:
            try:
                status, _, health = request('GET', '/health')
                if status != 200 or health.get('mock') is not True:
                    raise RuntimeError('Tests require /health mock:true; refusing real DVB hardware')
                break
            except (OSError, http.client.HTTPException):
                if time.monotonic() >= deadline or (process and process.poll() is not None):
                    raise RuntimeError('Mock server did not become ready')
                time.sleep(0.05)
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(ServerTest)
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        return 0 if result.wasSuccessful() else 1
    finally:
        if process:
            process.send_signal(signal.SIGTERM) if process.poll() is None else None
            try:
                _, diagnostic = process.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                _, diagnostic = process.communicate()
                raise RuntimeError('Server did not stop within three seconds after SIGTERM')
            if diagnostic:
                print(diagnostic.decode(errors='replace'))
            if process.returncode != 0:
                raise RuntimeError('Server exited with code ' + str(process.returncode))


if __name__ == '__main__':
    raise SystemExit(main())
