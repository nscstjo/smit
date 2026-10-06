#!/usr/bin/env python3
"""Read-only SAS header inventory from Linux usbmon text; never transmit.

Only OUT submissions and successful IN completions are considered. Text captures
can truncate payloads; header observations are retained without inventing bytes.
No cross-URB reassembly or automatic request/reply pairing is performed.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path


def inspect(line):
    fields = line.split()
    if len(fields) < 8 or '=' not in fields:
        return None
    direction = ('out' if fields[2] == 'S' and fields[3].startswith('Bo:') else
                 'in' if fields[2] == 'C' and fields[3].startswith('Bi:') and fields[4] == '0' else None)
    if direction is None:
        return None
    tokens = fields[fields.index('=') + 1:]
    if not tokens or any(not re.fullmatch(r'[0-9a-fA-F]+', x) or len(x) % 2 for x in tokens):
        return None
    data = bytes.fromhex(''.join(tokens))
    if len(data) < 13 or data[:2] != b'\x01\x00':
        return None
    outer = 5 if data[3] == 0x81 else 4
    if data[3] >= 0x80 and data[3] != 0x81:
        return None
    outer_length = data[outer - 1]
    if data[outer:outer+3] != b'\x01\x90\x02' or data[outer+5:outer+8] != b'\x9f\x9a\x07':
        return None
    pos = outer + 8
    if pos >= len(data):
        return dict(error='invalid_or_truncated_ber', direction=direction)
    first = data[pos]
    pos += 1
    if first & 128:
        count = first & 127
        if count == 0 or count > 4 or pos + count > len(data):
            return dict(error='invalid_or_truncated_ber', direction=direction)
        apdu_len = int.from_bytes(data[pos:pos+count], 'big')
        pos += count
    else:
        apdu_len = first
    if apdu_len < 7 or pos + 7 > len(data):
        return dict(error='short_sas_header', direction=direction)
    body = data[pos:min(pos + apdu_len, len(data))]
    message_len = int.from_bytes(body[1:3], 'big')
    payload_len = int.from_bytes(body[5:7], 'big')
    errors = []
    if apdu_len != message_len + 3:
        errors.append('apdu_message_length_mismatch')
    if message_len != payload_len + 4:
        errors.append('message_data_length_mismatch')
    if pos + apdu_len != outer + outer_length:
        errors.append('outer_apdu_length_mismatch')
    return dict(direction=direction, timestamp_us=int(fields[1]), urb=fields[0],
                endpoint=fields[3], usb_declared_length=int(fields[5]),
                captured_length=len(data), session=int.from_bytes(data[outer+3:outer+5], 'big'),
                tag='9f9a07', counter_byte=body[0], message_length=message_len,
                command=f'{int.from_bytes(body[3:5], "big"):04x}', data_length=payload_len,
                data_hex=body[7:].hex(), apdu_complete=len(body) == apdu_len,
                capture_truncated=len(data) < int(fields[5]), errors=errors)


def report(path):
    rows = []
    for number, line in enumerate(path.read_text(errors='replace').splitlines(), 1):
        result = inspect(line)
        if result is not None:
            rows.append(dict(line=number, **result))
    counts = {}
    for row in rows:
        key = row['direction'] + ':' + row.get('command', 'parse_error')
        counts[key] = counts.get(key, 0) + 1
    return dict(source=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                counts=counts, rows=rows,
                limitation='Observed headers only; no automatic reply pairing, no cross-URB reassembly; truncated data stays truncated.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    encoded = json.dumps(report(args.capture), ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(encoded + '\n', encoding='utf8')
    else:
        print(encoded)
