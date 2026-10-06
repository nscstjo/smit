#!/usr/bin/env python3
"""Read-only MPEG-TS continuity and PSI/SI report; no third-party dependencies."""
import collections
import hashlib
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


def descriptors(data):
    pos = 0
    while pos + 2 <= len(data):
        tag, size = data[pos:pos + 2]
        pos += 2
        if pos + size > len(data):
            raise ValueError("Truncated descriptor")
        yield tag, data[pos:pos + size]
        pos += size
    if pos != len(data):
        raise ValueError("Trailing descriptor byte")


def text(data):
    # Preserve exact bytes: Chinese broadcaster encoding is not assumed.
    return {"hex": data.hex(), "display": data.decode("utf-8", errors="replace")}


def ca_descriptors(data):
    return [{"ca_system_id": int.from_bytes(desc[:2], "big"),
             "ca_pid": int.from_bytes(desc[2:4], "big") & 0x1fff}
            for tag, desc in descriptors(data) if tag == 9 and len(desc) >= 4]


def bcd(data, digits):
    value = 0
    for i in range(digits):
        digit = (data[i // 2] >> (0 if i & 1 else 4)) & 15
        if digit > 9:
            raise ValueError("Invalid BCD")
        value = value * 10 + digit
    return value


def analyze(path):
    counts = collections.Counter()
    errors = collections.Counter()
    error_pids = collections.defaultdict(collections.Counter)
    error_windows = collections.defaultdict(collections.Counter)
    cc = {}
    previous = {}
    partial = {}
    tables = {}
    programs = {}
    psi_pids = {0, 1, 0x10, 0x11}
    crc_failures = collections.Counter()

    def section(pid, data):
        if len(data) < 8 or not data[1] & 0x80:
            return
        if crc32_mpeg(data):
            crc_failures[pid] += 1
            error_windows[total // 25000]["psi_crc"] += 1
            return
        if not data[5] & 1:
            return
        key = (pid, data[0], int.from_bytes(data[3:5], "big"), data[5] >> 1 & 31, data[6])
        tables[key] = data
        if pid == 0 and data[0] == 0:
            for p in range(8, len(data) - 4, 4):
                service = int.from_bytes(data[p:p + 2], "big")
                target = int.from_bytes(data[p + 2:p + 4], "big") & 0x1fff
                if service:
                    programs[service] = target
                    psi_pids.add(target)

    def feed(pid, data, start=False):
        if start:
            partial[pid] = bytearray()
        if pid not in partial:
            return
        buf = partial[pid]
        buf.extend(data)
        while buf:
            if buf[0] == 0xff:
                buf.clear()
                break
            if len(buf) < 3:
                break
            length = 3 + ((buf[1] & 15) << 8 | buf[2])
            if length > 4096:
                errors["section_length"] += 1
                buf.clear()
                break
            if len(buf) < length:
                break
            section(pid, bytes(buf[:length]))
            del buf[:length]

    digest = hashlib.sha256()
    total = 0
    with open(path, "rb") as source:
        while packet := source.read(188):
            digest.update(packet)
            if len(packet) != 188:
                errors["trailing_bytes"] += len(packet)
                break
            total += 1
            if packet[0] != 0x47:
                errors["sync"] += 1
                continue
            pid = (packet[1] & 31) << 8 | packet[2]
            counts[pid] += 1
            if packet[1] & 0x80:
                errors["transport_error"] += 1
                error_windows[total // 25000]["transport_error"] += 1
                error_pids["transport_error"][pid] += 1
                partial.pop(pid, None)
                continue
            if packet[3] & 0xc0:
                errors["scrambled_packets"] += 1
            afc, counter = (packet[3] >> 4) & 3, packet[3] & 15
            if not afc:
                errors["reserved_afc"] += 1
                error_windows[total // 25000]["reserved_afc"] += 1
                error_pids["reserved_afc"][pid] += 1
                continue
            offset, discontinuity = 4, False
            if afc & 2:
                size = packet[4]
                offset = 5 + size
                if offset > 188:
                    errors["adaptation_length"] += 1
                    error_windows[total // 25000]["adaptation_length"] += 1
                    error_pids["adaptation_length"][pid] += 1
                    partial.pop(pid, None)
                    continue
                discontinuity = bool(size and packet[5] & 0x80)
            if discontinuity:
                cc.pop(pid, None)
                partial.pop(pid, None)
            if not afc & 1 or pid == 0x1fff:
                continue
            if pid in cc:
                if counter == cc[pid] and packet == previous[pid]:
                    errors["duplicate_packets"] += 1
                    continue
                if counter != (cc[pid] + 1) & 15:
                    errors["continuity"] += 1
                    error_windows[total // 25000]["continuity"] += 1
                    error_pids["continuity"][pid] += 1
                    partial.pop(pid, None)
            cc[pid], previous[pid] = counter, packet
            if pid not in psi_pids or packet[3] & 0xc0 or offset >= 188:
                continue
            payload = packet[offset:]
            if packet[1] & 0x40:
                pointer = payload[0]
                if pointer + 1 > len(payload):
                    errors["pointer"] += 1
                    partial.pop(pid, None)
                    continue
                if pointer:
                    feed(pid, payload[1:1 + pointer])
                feed(pid, payload[1 + pointer:], start=True)
            else:
                feed(pid, payload)

    report = {"file": str(path), "bytes": Path(path).stat().st_size, "sha256": digest.hexdigest(),
              "packets": total, "pid_packets": dict(sorted(counts.items())), "errors": dict(errors),
              "error_pids": dict(error_pids),
              "errors_by_25000_packets": dict(error_windows),
              "psi_crc_failures": dict(crc_failures), "programs": programs,
              "pmt": [], "services": [], "networks": [], "cat": [], "table_sections": []}
    for key, data in sorted(tables.items()):
        pid, tid, extension, version, number = key
        report["table_sections"].append({"pid": pid, "table_id": tid, "extension": extension,
                                          "version": version, "section": number, "last_section": data[7]})
        if tid == 2 and pid in programs.values():
            item = {"service": extension, "pmt_pid": pid, "pcr_pid": int.from_bytes(data[8:10], "big") & 0x1fff,
                    "section_hex": data.hex(), "streams": []}
            pos = 12 + (int.from_bytes(data[10:12], "big") & 0xfff)
            item["ca"] = ca_descriptors(data[12:pos])
            while pos + 5 <= len(data) - 4:
                end = pos + 5 + (int.from_bytes(data[pos + 3:pos + 5], "big") & 0xfff)
                item["streams"].append({"type": data[pos], "pid": int.from_bytes(data[pos + 1:pos + 3], "big") & 0x1fff,
                                        "ca": ca_descriptors(data[pos + 5:end])})
                pos = end
            report["pmt"].append(item)
        elif tid == 1 and pid == 1:
            report["cat"].extend(ca_descriptors(data[8:-4]))
        elif tid == 0x42:
            pos = 11
            while pos + 5 <= len(data) - 4:
                size = int.from_bytes(data[pos + 3:pos + 5], "big") & 0xfff
                item = {"service": int.from_bytes(data[pos:pos + 2], "big"), "tsid": extension,
                        "free_ca_mode": bool(data[pos + 3] & 0x10)}
                for tag, desc in descriptors(data[pos + 5:pos + 5 + size]):
                    if tag == 0x48 and len(desc) >= 3:
                        provider_size = desc[1]
                        name_pos = 2 + provider_size
                        if name_pos < len(desc):
                            item.update(type=desc[0], provider=text(desc[2:name_pos]),
                                        name=text(desc[name_pos + 1:name_pos + 1 + desc[name_pos]]))
                report["services"].append(item)
                pos += 5 + size
        elif tid == 0x40:
            size = int.from_bytes(data[8:10], "big") & 0xfff
            item = {"network_id": extension, "transports": []}
            for tag, desc in descriptors(data[10:10 + size]):
                if tag == 0x40:
                    item["name"] = text(desc)
            pos = 12 + size
            while pos + 6 <= len(data) - 4:
                ts = {"tsid": int.from_bytes(data[pos:pos + 2], "big"), "original_network_id": int.from_bytes(data[pos + 2:pos + 4], "big")}
                size = int.from_bytes(data[pos + 4:pos + 6], "big") & 0xfff
                for tag, desc in descriptors(data[pos + 6:pos + 6 + size]):
                    if tag == 0x44 and len(desc) == 11:
                        ts.update(frequency_hz=bcd(desc[:4], 8) * 100, symbol_rate=bcd(desc[7:], 7) * 100,
                                  modulation=desc[6], fec_inner=desc[10] & 15)
                item["transports"].append(ts)
                pos += 6 + size
            report["networks"].append(item)
    report["missing_pmt_services"] = sorted(set(programs) - {item["service"] for item in report["pmt"]})
    return report


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: analyze-ts.py sample.ts")
    print(json.dumps(analyze(sys.argv[1]), ensure_ascii=False, indent=2))
