#!/usr/bin/env python3
"""Builds testmedia/arib/tbs-captions-wrap.ts from testmedia/arib/tbs-captions.ts.

An excerpt of the fixture is re-timed so the 33-bit MPEG-TS clock wraps WRAP_AT seconds in (PTS, DTS and PCR
shifted modulo 2^33), and the caption component is announced by a second PMT version only SWITCH_AT seconds
in. libavformat then creates the caption stream after the program's wrap reference was set from the first
video packet, which is how recordings with a PMT change at the start behave.

  python make_arib_wrap_fixture.py [source.ts] [output.ts]
"""
import os
import sys

WRAP = 1 << 33
DURATION = 18.0   # seconds of the source kept
WRAP_AT = 9.0     # seconds after the first video PTS at which the clock wraps
SWITCH_AT = 1.5   # seconds after the first video PTS at which the PMT lists the caption component

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def crc32_mpeg(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def payload_offset(pkt):
    afc = (pkt[3] >> 4) & 3
    if afc == 1:
        return 4
    if afc == 3:
        return 5 + pkt[4]
    return 188


def read_ts(b, o):
    return (((b[o] >> 1) & 7) << 30) | (b[o + 1] << 22) | ((b[o + 2] >> 1) << 15) | (b[o + 3] << 7) | (b[o + 4] >> 1)


def write_ts(b, o, prefix, v):
    v %= WRAP
    b[o] = (prefix << 4) | (((v >> 30) & 7) << 1) | 1
    b[o + 1] = (v >> 22) & 0xFF
    b[o + 2] = (((v >> 15) & 0x7F) << 1) | 1
    b[o + 3] = (v >> 7) & 0xFF
    b[o + 4] = ((v & 0x7F) << 1) | 1


def psi_section(pkt):
    off = payload_offset(pkt)
    off += 1 + pkt[off]
    return pkt[off:]


def parse_pmt(sec):
    section_length = ((sec[1] & 0xF) << 8) | sec[2]
    program_number = (sec[3] << 8) | sec[4]
    pcr_pid = ((sec[8] & 0x1F) << 8) | sec[9]
    pi_len = ((sec[10] & 0xF) << 8) | sec[11]
    prog_info = bytes(sec[12:12 + pi_len])
    streams = []
    p, end = 12 + pi_len, 3 + section_length - 4
    while p + 5 <= end:
        es_len = ((sec[p + 3] & 0xF) << 8) | sec[p + 4]
        streams.append((sec[p], ((sec[p + 1] & 0x1F) << 8) | sec[p + 2], bytes(sec[p + 5:p + 5 + es_len])))
        p += 5 + es_len
    return program_number, pcr_pid, prog_info, streams


def build_pmt_packet(pid, cc, program_number, version, pcr_pid, prog_info, streams):
    body = bytearray([program_number >> 8, program_number & 0xFF, 0xC1 | (version << 1), 0, 0,
                      0xE0 | (pcr_pid >> 8), pcr_pid & 0xFF, 0xF0 | (len(prog_info) >> 8), len(prog_info) & 0xFF])
    body += prog_info
    for st, spid, es in streams:
        body += bytes([st, 0xE0 | (spid >> 8), spid & 0xFF, 0xF0 | (len(es) >> 8), len(es) & 0xFF]) + es
    length = len(body) + 4
    sec = bytearray([0x02, 0xB0 | (length >> 8), length & 0xFF]) + body
    sec += crc32_mpeg(sec).to_bytes(4, "big")
    pkt = bytearray([0x47, 0x40 | (pid >> 8), pid & 0xFF, 0x10 | cc, 0]) + sec
    assert len(pkt) <= 188, "PMT does not fit one packet"
    return pkt + b"\xFF" * (188 - len(pkt))


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "testmedia", "arib", "tbs-captions.ts")
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "testmedia", "arib", "tbs-captions-wrap.ts")
    with open(src, "rb") as f:
        data = f.read(int(DURATION * 4_000_000) + 4_000_000)
    packets = [bytearray(data[i:i + 188]) for i in range(0, len(data) - 187, 188)]

    pat = next(p for p in packets if ((p[1] & 0x1F) << 8 | p[2]) == 0 and p[1] & 0x40)
    sec = psi_section(pat)
    pmt_pid = None
    for p in range(8, 3 + (((sec[1] & 0xF) << 8) | sec[2]) - 4, 4):
        if (sec[p] << 8 | sec[p + 1]) != 0:
            pmt_pid = (sec[p + 2] & 0x1F) << 8 | sec[p + 3]
            break
    pmt = next(p for p in packets if ((p[1] & 0x1F) << 8 | p[2]) == pmt_pid and p[1] & 0x40)
    program_number, pcr_pid, prog_info, streams = parse_pmt(psi_section(pmt))
    # video, AAC and the component with a data component descriptor (0xFD, the caption); other private streams are dropped
    kept = [s for s in streams if s[0] in (0x02, 0x0F) or (s[0] == 0x06 and bytes([0xFD, 3, 0, 8]) in s[2])]
    video_pid = next(s[1] for s in kept if s[0] == 0x02)
    caption_pids = {s[1] for s in kept if s[0] == 0x06}
    early = [s for s in kept if s[1] not in caption_pids]
    keep_pids = {0, pmt_pid} | {s[1] for s in kept}

    first_video = next(read_ts(p, payload_offset(p) + 9) for p in packets
                       if ((p[1] & 0x1F) << 8 | p[2]) == video_pid and p[1] & 0x40
                       and p[payload_offset(p) + 7] & 0x80)
    delta = (WRAP - int(WRAP_AT * 90000) - first_video) % WRAP
    end_pts = first_video + int(DURATION * 90000)

    out = bytearray()
    pmt_cc = 0
    elapsed = 0  # furthest source video PTS past the first video PTS so far, in ticks
    for p in packets:
        pid = (p[1] & 0x1F) << 8 | p[2]
        if pid not in keep_pids or (pid in caption_pids and elapsed < int(SWITCH_AT * 90000)):
            continue
        off = payload_offset(p)
        if p[1] & 0x40 and off + 14 <= 188 and p[off:off + 3] == b"\x00\x00\x01" and p[off + 7] & 0x80:
            flags = p[off + 7]
            pts = read_ts(p, off + 9)
            if pid == video_pid:
                elapsed = max(elapsed, (pts - first_video + WRAP // 2) % WRAP - WRAP // 2)
                if elapsed > int(DURATION * 90000):
                    break
            write_ts(p, off + 9, 3 if flags & 0x40 else 2, pts + delta)
            if flags & 0x40:
                write_ts(p, off + 14, 1, read_ts(p, off + 14) + delta)
        if (p[3] >> 4) & 2 and p[4] >= 7 and p[5] & 0x10:
            base = (int.from_bytes(p[6:10], "big") << 1) | (p[10] >> 7)
            base = (base + delta) % WRAP
            p[6:10] = (base >> 1).to_bytes(4, "big")
            p[10] = ((base & 1) << 7) | 0x7E | (p[10] & 1)
        if pid == pmt_pid:
            if not p[1] & 0x40:
                continue
            late = elapsed >= int(SWITCH_AT * 90000)
            p = build_pmt_packet(pmt_pid, pmt_cc, program_number, 1 if late else 0, pcr_pid, prog_info,
                                 kept if late else early)
            pmt_cc = (pmt_cc + 1) & 15
        out += p

    with open(dst, "wb") as f:
        f.write(out)
    print(f"{dst}: {len(out)} bytes, clock wraps {WRAP_AT} s in, caption PIDs {sorted(hex(x) for x in caption_pids)} listed after {SWITCH_AT} s")


if __name__ == "__main__":
    main()
