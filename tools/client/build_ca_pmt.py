#!/usr/bin/env python3
"""Construct CA_PMT from a CRC-validated captured PMT, using normal CA selection."""
import json
import sys
from pathlib import Path


def crc32_mpeg(data):
    crc = 0xffffffff
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ (0x04c11db7 if crc & 0x80000000 else 0)) & 0xffffffff
    return crc


def validate_descriptors(data):
    pos = 0
    while pos < len(data):
        if len(data) - pos < 2 or data[pos + 1] > len(data) - pos - 2:
            raise ValueError('Truncated descriptor')
        if data[pos] == 9 and data[pos + 1] < 4:
            raise ValueError('Short CA descriptor')
        pos += 2 + data[pos + 1]


def encode(pmt, expected_service=None):
    if len(pmt) < 16 or pmt[0] != 2:
        raise ValueError('Not a PMT')
    section_length = int.from_bytes(pmt[1:3], 'big') & 0xfff
    if pmt[1] & 0xf0 != 0xb0 or section_length > 1021 or section_length + 3 != len(pmt):
        raise ValueError('Invalid PMT section length/header')
    if crc32_mpeg(pmt):
        raise ValueError('PMT CRC mismatch')
    service = int.from_bytes(pmt[3:5], 'big')
    if not service or (expected_service is not None and service != expected_service):
        raise ValueError('PMT service mismatch')
    if not pmt[5] & 1 or pmt[6] or pmt[7]:
        raise ValueError('PMT must be current and single-section')
    def info(data):
        validate_descriptors(data)
        body = b'\x01' + data if data else b''  # OK descrambling
        return (0xf000 | len(body)).to_bytes(2, 'big') + body
    length = int.from_bytes(pmt[10:12], 'big') & 0xfff
    pos = 12 + length
    if pos > len(pmt) - 4:
        raise ValueError('Truncated program descriptors')
    result = b'\x03' + pmt[3:6] + info(pmt[12:pos])  # only one selected program
    while pos < len(pmt) - 4:
        if pos + 5 > len(pmt) - 4:
            raise ValueError('Truncated stream')
        end = pos + 5 + (int.from_bytes(pmt[pos+3:pos+5], 'big') & 0xfff)
        if end > len(pmt) - 4:
            raise ValueError('Truncated stream descriptors')
        result += pmt[pos:pos+3] + info(pmt[pos+5:end])
        pos = end
    size = len(result)
    if size + (4 if size < 128 else 5) > 250:
        raise ValueError('Exceeds current SMIT transport limit')
    length_bytes = bytes([size]) if size < 128 else bytes([0x81, size])
    apdu = b'\x9f\x80\x32' + length_bytes + result
    if len(apdu) > 250:
        raise ValueError('Exceeds current SMIT transport limit')
    return apdu


if __name__ == '__main__':
    report = json.loads(Path(sys.argv[1]).read_text())
    service = int(sys.argv[2])
    matches = [p for p in report['pmt'] if p['service'] == service]
    if len(matches) != 1:
        raise SystemExit('Expected exactly one current PMT version')
    apdu = encode(bytes.fromhex(matches[0]['section_hex']), expected_service=service)
    Path(sys.argv[3]).write_bytes(apdu)
    print(f'service={service} CA_PMT={apdu.hex()}')
