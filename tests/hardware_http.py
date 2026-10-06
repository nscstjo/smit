"""Real HTTP transport validation, no DVR byte filtering or recordings."""
import argparse
import importlib.util
import json
import math
from pathlib import Path
import time
import threading
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('monitor', ROOT / 'tools/client/monitor_ts.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--url', required=True)
    parser.add_argument('--report', required=True)
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--alternations', type=int, default=10)
    parser.add_argument('--control-probe', action='store_true', help='At 3s request absent section with 1s timeout while reading raw stream')
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or args.seconds <= 0 or args.alternations < 0:
        raise ValueError('seconds must be finite and positive; alternations nonnegative')
    if Path(args.report).exists():
        raise FileExistsError(args.report)
    events, captures = [], []

    def call(path, data=None, expected=200):
        start = time.monotonic()
        req = urllib.request.Request(args.url + path, data=json.dumps(data).encode() if data is not None else None,
                                     headers={'Content-Type': 'application/json'})
        try:
            response = urllib.request.urlopen(req, timeout=15)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            result = json.load(response)
            events.append(dict(path=path, request=data, status=response.status, result=result,
                               elapsed=round(time.monotonic()-start, 4)))
        if response.status != expected or (expected == 200 and not result.get('ok')):
            raise RuntimeError(result)
        return result

    def tune(freq, service):
        call('/stream/stop', {})
        call('/frontend/tune', dict(frequency=freq, symbol_rate=6875000, modulation=64))
        deadline = time.monotonic() + 8
        while not call('/frontend/status', {}).get('locked'):
            if time.monotonic() > deadline:
                raise TimeoutError('frontend did not lock')
            time.sleep(.1)
        call('/service/select', dict(service=service, timeout_ms=5000))

    def capture(freq, service, targets, duration):
        monitor = mod.Monitor(targets)
        clear_payload = {pid: 0 for pid in targets}
        pending = b''
        first, last, second_errors = {}, {}, {}
        samples = []
        total = 0
        probe = None
        probe_failure = []
        probe_timing = {}
        read_blocks = []
        def control_probe():
            probe_timing['start_seconds'] = time.monotonic()-start
            try:
                result = call('/demux/section', dict(pid=8190, table=0, timeout_ms=1000), expected=502)
                # Linux MIPS ETIMEDOUT=145; asm-generic/x86 ETIMEDOUT=110.
                if result.get('errno') not in (110, 145):
                    raise RuntimeError('expected ETIMEDOUT: ' + repr(result))
            except Exception as error:
                probe_failure.append(repr(error))
            finally:
                probe_timing['finish_seconds'] = time.monotonic()-start
        request_start = time.monotonic()
        with urllib.request.urlopen(args.url + '/stream', timeout=15) as stream:
            start = time.monotonic()
            first_data = None
            while time.monotonic()-start < duration or pending:
                elapsed = time.monotonic()-start
                block = stream.read1(188-len(pending) if elapsed >= duration else 188*256)
                if not block:
                    raise EOFError('stream ended early')
                now = time.monotonic()-start
                read_blocks.append(dict(seconds=round(now, 6), bytes=len(block)))
                if args.control_probe and now >= 3 and probe is None:
                    probe = threading.Thread(target=control_probe)
                    probe.start()
                if first_data is None:
                    first_data = time.monotonic()-request_start
                total += len(block)
                pending += block
                end = len(pending)//188*188
                for pos in range(0, end, 188):
                    before = dict(monitor.errors)
                    packet = pending[pos:pos+188]
                    monitor.packet(packet)
                    pid = ((packet[1]&31)<<8)|packet[2]
                    afc = (packet[3]>>4)&3
                    offset = 5+packet[4] if afc&2 else 4
                    if pid in clear_payload and packet[0]==0x47 and not packet[1]&0x80 and not packet[3]&0xc0 and afc&1 and offset<188:
                        clear_payload[pid] += 1
                    for name, count in monitor.errors.items():
                        delta = count-before.get(name, 0)
                        if delta:
                            event = dict(seconds=round(now, 6), packet=monitor.packets,
                                         pid=((pending[pos+1]&31)<<8)|pending[pos+2])
                            first.setdefault(name, event)
                            last[name] = event
                            bucket = second_errors.setdefault(str(int(now)), {})
                            bucket[name] = bucket.get(name, 0)+delta
                            if len(samples) < 30:
                                samples.append(dict(error=name, **event))
                pending = pending[end:]
            elapsed = time.monotonic()-start
        if probe:
            probe.join()
        result = dict(frequency=freq, service=service, targets=targets,
                      duration_requested=duration, bytes_received=total, trailing_bytes=len(pending),
                      first_data_seconds=round(first_data, 6), **monitor.report(elapsed),
                      first_error=first, last_error=last, errors_by_second=second_errors,
                      first_error_samples=samples,
                      control_probe_failure=probe_failure, control_probe_timing=probe_timing, read_blocks=read_blocks,
                      clear_payload_packets=clear_payload,
                      clear_verified=all(clear_payload[p] and not monitor.scrambled[p] for p in targets),
                      note='All packets inspected from first DVR byte; times are HTTP receive observations, no byte filtering.')
        captures.append(result)
        print(json.dumps({key: value for key, value in result.items() if key != 'read_blocks'}), flush=True)
        call('/stream/stop', {})
        call('/stream/stats', {})

    failure = None
    try:
        call('/health')
        for freq, service, targets in [(147000000, 115, [32, 33]), (474000000, 120, [121, 122])]:
            tune(freq, service)
            capture(freq, service, targets, args.seconds)
        for index in range(args.alternations):
            freq, service, targets = [(147000000, 115, [32, 33]), (474000000, 120, [121, 122])][index % 2]
            tune(freq, service)
            capture(freq, service, targets, 2)
    except Exception as error:
        failure = repr(error)
        print(failure, flush=True)
    finally:
        try:
            tune(147000000, 115)
            final = call('/stream/stats', {})
            final_frontend = call('/frontend/status', {})
        except Exception as error:
            final, final_frontend = None, None
            failure = (failure or '') + ' cleanup: ' + repr(error)
        with Path(args.report).open('x', encoding='utf-8') as report:
            report.write(json.dumps(dict(events=events, captures=captures, failure=failure,
                                         final=final, final_frontend=final_frontend), indent=2))
    return int(bool(failure or any(c['errors'] or c['control_probe_failure'] or not c['clear_verified'] for c in captures)))


if __name__ == '__main__':
    raise SystemExit(main())
