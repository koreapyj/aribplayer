#!/usr/bin/env python3
"""Adds an ARIB STD-B24 caption stream to an FFmpeg-muxed MPEG-TS (the test clips carry none).

  python3 -I mac/app/make-arib-caption-clip.py IN.ts OUT.ts [--every S]

IN must be an FFmpeg mpegts output (one program, PMT PID 0x1000, video PID 0x100), e.g. the
av_1080p_h264_aac.ts written by make-testmedia.sh. OUT gets PID 0x130: PMT entry stream_type 0x06 with stream_identifier_descriptor (component
tag 0x30) and data_component_descriptor (data_component_id 0x0008), i.e. what libavformat's mpegts demuxer maps to
AV_CODEC_ID_ARIB_CAPTION profile A, arib_type "caption" (and the program's "arib" verdict). Every S seconds (default 2)
from the first video PTS a synchronised PES (stream_id 0xBD, data_identifier 0x80) carries caption management data and
a caption statement: CS, SWF 960x540 horizontal, SDF / SDP / SSM / SHS / SVS, APS row 7, white "CAPTION n" in
alphanumerics (LS1) at normal size, so libaribcaption draws a bitmap on the caption layer that stays until the next
statement. Stdlib only; writes only OUT.
"""
import struct
import sys

TS = 188
PMT_PID, VIDEO_PID, CAPTION_PID = 0x1000, 0x100, 0x130


def crc32_mpeg(data):
    c = 0xFFFFFFFF
    for b in data:
        c ^= b << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
    return c


def crc16_ccitt(data):   # ARIB STD-B24 data_group CRC_16: x^16 + x^12 + x^5 + 1, initial 0
    c = 0
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def csi(params, final):
    return b'\x9b' + params.encode() + b'\x20' + bytes([final])


def data_group(group_id, body):
    head = bytes([(group_id << 2) | 0, 0, 0]) + struct.pack('>H', len(body))
    g = head + body
    return g + struct.pack('>H', crc16_ccitt(g))


def management():
    # TMD free; 1 language: tag 0, DMF 0000 (automatic display), "jpn", format 0000, TCS 8-bit, no rollup; no units
    lang = bytes([0x10]) + b'jpn' + bytes([0x00])
    return data_group(0x00, bytes([0x3F, 1]) + lang + b'\x00\x00\x00')


def statement(text):
    unit = (b'\x0c' + csi('7', 0x53) + csi('960;540', 0x56) + csi('0;0', 0x5F) + csi('36;36', 0x57) +
            csi('4', 0x58) + csi('24', 0x59) + b'\x1c' + bytes([0x40 + 7, 0x40 + 4]) + b'\x8a' + b'\x87' +
            b'\x0e' + text.encode('ascii'))
    du = b'\x1f\x20' + struct.pack('>I', len(unit))[1:] + unit
    return data_group(0x01, bytes([0x3F]) + struct.pack('>I', len(du))[1:] + du)


def pts_bytes(pts):
    return bytes([0x21 | ((pts >> 29) & 0x0E), (pts >> 22) & 0xFF, 0x01 | ((pts >> 14) & 0xFE), (pts >> 7) & 0xFF,
                  0x01 | ((pts << 1) & 0xFE)])


def caption_pes(pts, group):
    payload = bytes([0x80, 0xFF, 0xF0]) + group   # synchronised PES, private_stream_id, no PES data packet header
    header = bytes([0x80, 0x80, 5]) + pts_bytes(pts)
    body = header + payload
    return b'\x00\x00\x01\xbd' + struct.pack('>H', len(body)) + body


def packetize(pes, cc):
    out = []
    first = True
    while pes:
        room = TS - 4
        chunk = pes[:room]
        pes = pes[room:]
        pusi = 0x40 if first else 0
        if len(chunk) < room:   # adaptation field stuffing
            stuff = room - len(chunk)
            af = bytes([stuff - 1]) + (bytes([0x00]) + b'\xff' * (stuff - 2) if stuff >= 2 else b'')
            pkt = bytes([0x47, pusi | (CAPTION_PID >> 8), CAPTION_PID & 0xFF, 0x30 | cc]) + af + chunk
        else:
            pkt = bytes([0x47, pusi | (CAPTION_PID >> 8), CAPTION_PID & 0xFF, 0x10 | cc]) + chunk
        assert len(pkt) == TS
        out.append(pkt)
        cc = (cc + 1) & 15
        first = False
    return out, cc


def rewrite_pmt(pkt):
    """The PMT section (one packet, as FFmpeg writes it) plus the caption ES; same version, new CRC."""
    off = 4
    if pkt[3] & 0x20:
        off += 1 + pkt[4]
    ptr = pkt[off]
    sec = pkt[off + 1 + ptr:]
    length = ((sec[1] & 0x0F) << 8) | sec[2]
    body = sec[3:3 + length - 4]
    es = bytes([0x06, 0xE0 | (CAPTION_PID >> 8), CAPTION_PID & 0xFF])
    desc = bytes([0x52, 1, 0x30, 0xFD, 3, 0x00, 0x08, 0x3D])
    es += bytes([0xF0 | (len(desc) >> 8), len(desc) & 0xFF]) + desc
    body = body + es
    new_len = len(body) + 4
    head = bytes([sec[0], (sec[1] & 0xF0) | (new_len >> 8), new_len & 0xFF])
    section = head + body
    section += struct.pack('>I', crc32_mpeg(section))
    out = pkt[:off] + bytes([0]) + section
    if len(out) > TS:
        raise SystemExit('PMT does not fit one packet')
    return out + b'\xff' * (TS - len(out))


def pes_pts(pkt):
    if not (pkt[1] & 0x40):
        return None
    off = 4
    if pkt[3] & 0x20:
        off += 1 + pkt[4]
    p = pkt[off:]
    if len(p) < 14 or p[:3] != b'\x00\x00\x01' or not (p[7] & 0x80):
        return None
    b = p[9:14]
    return ((b[0] >> 1) & 7) << 30 | b[1] << 22 | (b[2] >> 1) << 15 | b[3] << 7 | b[4] >> 1


def main(argv):
    args = [a for a in argv if not a.startswith('--')]
    every = 2.0
    if '--every' in argv:
        every = float(argv[argv.index('--every') + 1])
        args.remove(argv[argv.index('--every') + 1])
    if len(args) != 2:
        print(__doc__)
        return 2
    data = open(args[0], 'rb').read()
    if not data or len(data) % TS or data[0] != 0x47:
        raise SystemExit('not a 188-byte TS')
    out = []
    cc = 0
    next_pts = None
    n = 0
    pmts = 0
    for i in range(0, len(data), TS):
        pkt = data[i:i + TS]
        pid = ((pkt[1] & 0x1F) << 8) | pkt[2]
        if pid == PMT_PID and pkt[1] & 0x40:
            pkt = rewrite_pmt(pkt)
            pmts += 1
        if pid == VIDEO_PID:
            pts = pes_pts(pkt)
            if pts is not None:
                if next_pts is None:
                    next_pts = pts + 45000
                if pts + 45000 >= next_pts:   # the caption half a second ahead of the video that shows it
                    n += 1
                    for group in (management(), statement('CAPTION %d' % n)):
                        pkts, cc = packetize(caption_pes(next_pts, group), cc)
                        out.extend(pkts)
                    next_pts += int(every * 90000)
        out.append(pkt)
    if not pmts:
        raise SystemExit('no PMT on PID 0x1000')
    if not n:
        raise SystemExit('no video PTS on PID 0x100; no captions generated')
    open(args[1], 'wb').write(b''.join(out))
    print('%s: %d caption statements every %.1f s on PID 0x%X, %d PMT sections rewritten' % (args[1], n, every, CAPTION_PID, pmts))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
