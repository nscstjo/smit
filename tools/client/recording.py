"""SMIT iCast USB-C/DTMB USB Tuner byte-preserving stream recording."""
import contextlib
import ctypes
import importlib.util
import json
import math
from pathlib import Path
import sys
import time
import urllib.error
import urllib.request

def load_monitor():
    spec = importlib.util.spec_from_file_location("monitor_ts", Path(__file__).with_name("monitor_ts.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@contextlib.contextmanager
def awake():
    if sys.platform != 'win32':
        yield
        return
    power = ctypes.windll.kernel32.SetThreadExecutionState
    if not power(0x80000001):
        raise OSError('cannot prevent idle sleep during capture')
    try:
        yield
    finally:
        power(0x80000000)


def capture(args, awake_factory=awake):
    if not math.isfinite(args.seconds) or args.seconds <= 0 or any(not 0 <= p <= 8191 for p in args.pid):
        raise ValueError('seconds must be positive; PIDs must be 0..8191')
    monitor = load_monitor().Monitor(args.pid)
    report_path = Path(args.report) if args.report else None
    if report_path and report_path.exists():
        raise FileExistsError(report_path)
    if args.output and report_path and Path(args.output).resolve() == report_path.resolve():
        raise ValueError('output and report must differ')
    pending = b''
    total = 0
    # Reserve paths before touching hardware; never overwrite captures.
    with contextlib.ExitStack() as stack:
        output = stack.enter_context(open(args.output, 'xb')) if args.output else None
        report_file = stack.enter_context(report_path.open('x', encoding='utf-8')) if report_path else None
        stack.enter_context(awake_factory())
        stream = stack.enter_context(urllib.request.urlopen(args.url.rstrip('/') + '/stream', timeout=15))
        start = time.monotonic()
        failure = None
        continuity_samples = []
        clear_payload = {pid: 0 for pid in args.pid}
        try:
            while time.monotonic() - start < args.seconds or pending:
                block = stream.read1(188 - len(pending) if time.monotonic() - start >= args.seconds else 188 * 256)
                if not block:
                    raise EOFError('server ended stream before duration elapsed')
                total += len(block)
                if output:
                    output.write(block)
                pending += block
                end = len(pending) // 188 * 188
                for i in range(0, end, 188):
                    before = monitor.errors.get('continuity', 0)
                    packet = pending[i:i+188]
                    monitor.packet(packet)
                    pid = (packet[1] & 31) * 256 + packet[2]
                    afc = packet[3] >> 4 & 3
                    offset = 5 + packet[4] if afc & 2 else 4
                    if (pid in clear_payload and packet[0] == 0x47 and not packet[1] & 0x80
                            and not packet[3] & 0xc0 and afc & 1 and offset < 188):
                        clear_payload[pid] += 1
                    if monitor.errors.get('continuity', 0) != before and len(continuity_samples) < 16:
                        continuity_samples.append({'packet': monitor.packets, 'pid': (pending[i+1] & 31) * 256 + pending[i+2]})
                pending = pending[end:]
        except (OSError, EOFError) as exc:
            failure = str(exc)
        elapsed = time.monotonic() - start
        report = {'bytes': total, 'seconds': elapsed, 'packets': monitor.packets,
                  'trailing_bytes': len(pending), 'errors': dict(monitor.errors), 'continuity_samples': continuity_samples,
                  'scrambled': dict(monitor.scrambled), 'counts': dict(monitor.counts),
                  'failure': failure, 'targets': args.pid,
                  'clear_payload_packets': clear_payload,
                  'clear_verified': bool(args.pid) and all(clear_payload[p] > 0 for p in args.pid)
                       and not any(monitor.scrambled[p] for p in args.pid),
                  'note': 'Clear TSC is not a decoder or entitlement validation.'}
        text = json.dumps(report, indent=2)
        print(text)
        if report_file:
            report_file.write(text + '\n')
        return 1 if failure or pending or monitor.errors or (args.pid and not report['clear_verified']) else 0
