#!/usr/bin/env python3
"""Convert classic USB_LINUX_MMAPPED pcap bulk events to untruncated usbmon text.

Header layout: https://docs.kernel.org/usb/usbmon.html#raw-binary-format-and-api
Supports microsecond classic pcap, link type 220, bulk only; never transmits.
"""
import argparse
import struct
from pathlib import Path


def convert(source, bus, device):
    header = source.read(24)
    if len(header) != 24 or header[:4] not in (b'\xd4\xc3\xb2\xa1', b'\xa1\xb2\xc3\xd4'):
        raise ValueError('Expected microsecond classic pcap')
    endian = '<' if header[0] == 0xd4 else '>'
    major, minor, _, _, snaplen, link = struct.unpack(endian + 'HHIIII', header[4:])
    if (major, minor, link) != (2, 4, 220):
        raise ValueError('Expected pcap 2.4 USB_LINUX_MMAPPED (220)')
    while raw := source.read(16):
        if len(raw) != 16:
            raise ValueError('Truncated pcap record header')
        _, _, size, original = struct.unpack(endian + 'IIII', raw)
        if size < 64 or size > snaplen or size > 16 * 1024 * 1024 or size > original:
            raise ValueError('Invalid pcap record length')
        data = source.read(size)
        if len(data) != size:
            raise ValueError('Truncated pcap record')
        urb, event, kind, endpoint, address, busnum, _, flag = struct.unpack(endian + 'QBBBBHBB', data[:16])
        if kind != 3 or busnum != bus or address != device:
            continue
        seconds, usec, status, length, captured = struct.unpack(endian + 'qiiII', data[16:40])
        if captured > len(data) - 64:
            raise ValueError('Truncated USB payload')
        if event not in (ord('S'), ord('C'), ord('E')):
            continue
        payload = data[64:64+captured] if flag == 0 else b''
        direction = 'i' if endpoint & 128 else 'o'
        tag = '= ' + payload.hex() if payload else ('<' if direction == 'i' else '>')
        yield f'{urb:x} {seconds * 1000000 + usec} {chr(event)} B{direction}:{busnum}:{address:03}:{endpoint & 15} {status} {length} {tag}'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--bus', type=int, required=True)
    parser.add_argument('--device', type=int, required=True)
    args = parser.parse_args()
    with args.capture.open('rb') as source:
        for line in convert(source, args.bus, args.device):
            print(line)
