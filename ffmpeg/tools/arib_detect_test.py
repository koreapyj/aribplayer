"""Demuxer-level tests for 1006a-mpegts-arib-detection, driven through arib_inspect.exe.

  python arib_detect_test.py [--real]

Synthetic part: ffmpeg makes a small TS (video 0x100, audio 0x101, PMT 0x1000); this script
rewrites its PMT packets (new section, version, CRC32) and optionally moves audio PES packets to
another PID, then checks the verdict, the exported metadata and the stream merge behaviour.
--real adds the recordings on Y: / Z: (read-only) and testmedia/arib.
Exit code 0 only if every check passed. Run from anywhere; stdlib only.
"""
import itertools
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
DIST = os.path.join(ROOT, 'ffmpeg', 'dist', 'win64')
INSPECT = os.path.join(ROOT, 'artifacts', 'scratch', 'aribdetect', 'bin', 'arib_inspect.exe')
WORK = os.path.join(ROOT, 'artifacts', 'scratch', 'aribdetect', 'synth')
ENV = dict(os.environ, PATH=DIST + os.pathsep + os.environ['PATH'])

PMT_PID, VID, AUD = 0x1000, 0x100, 0x101

_CRC = []
for _i in range(256):
    _c = _i << 24
    for _ in range(8):
        _c = ((_c << 1) ^ 0x04C11DB7) & 0xffffffff if _c & 0x80000000 else (_c << 1) & 0xffffffff
    _CRC.append(_c)


def crc32(data):
    c = 0xffffffff
    for b in data:
        c = ((c << 8) & 0xffffffff) ^ _CRC[(c >> 24) ^ b]
    return c


def desc(tag, body=b''):
    return bytes([tag, len(body)]) + body


def pmt_section(program, version, prog_descs, streams):
    """streams: list of (stream_type, pid, [descriptor bytes])."""
    pd = b''.join(prog_descs)
    body = bytes([0xe0 | (VID >> 8), VID & 0xff, 0xf0 | (len(pd) >> 8), len(pd) & 0xff]) + pd
    for st, pid, ds in streams:
        d = b''.join(ds)
        body += bytes([st, 0xe0 | (pid >> 8), pid & 0xff, 0xf0 | (len(d) >> 8), len(d) & 0xff]) + d
    hdr = bytes([program >> 8, program & 0xff, 0xc1 | (version << 1), 0, 0])
    sec = bytes([0x02, 0xb0 | ((len(hdr) + len(body) + 4) >> 8), (len(hdr) + len(body) + 4) & 0xff]) + hdr + body
    return sec + crc32(sec).to_bytes(4, 'big')


def rewrite(src, dst, program, pick, audio_move=None):
    """pick(i, n) -> section bytes for the i-th PMT packet of n. audio_move=(from_pkt_fraction, new_pid)."""
    data = bytearray(open(src, 'rb').read())
    npk = len(data) // 188
    pmts = [i for i in range(npk) if ((data[i * 188 + 1] & 0x1f) << 8 | data[i * 188 + 2]) == PMT_PID]
    for k, i in enumerate(pmts):
        o = i * 188
        sec = pick(k, len(pmts))
        assert len(sec) + 5 <= 184
        data[o + 4:o + 188] = bytes([0]) + sec + b'\xff' * (183 - len(sec))
    if audio_move:
        frac, newpid = audio_move
        for i in range(int(npk * frac), npk):
            o = i * 188
            if ((data[o + 1] & 0x1f) << 8 | data[o + 2]) == AUD:
                data[o + 1] = (data[o + 1] & 0xe0) | (newpid >> 8)
                data[o + 2] = newpid & 0xff
    open(dst, 'wb').write(data)


def inspect(path, seconds=100):
    r = subprocess.run([INSPECT, '--seconds', str(seconds), path], capture_output=True, env=ENV, timeout=120)
    return r.stdout.decode('utf8', 'replace')


FAILS = []


def check(cond, msg):
    print(('PASS  ' if cond else 'FAIL  ') + msg)
    if not cond:
        FAILS.append(msg)


def verdict(out):
    m = re.search(r'^after_probe: format arib=(\S+)', out, re.M)
    return m.group(1) if m else None


def final_prog(out, key):
    m = re.search(r'^final:\s+program id=\d+ .*?%s=(\S+)' % key, out, re.M)
    return m.group(1) if m else None


def make_base():
    os.makedirs(WORK, exist_ok=True)
    base = os.path.join(WORK, 'base.ts')
    if not os.path.exists(base):
        subprocess.run([os.path.join(DIST, 'ffmpeg.exe'), '-v', 'error', '-y', '-f', 'lavfi', '-i',
                        'testsrc2=size=320x240:rate=25:duration=6', '-f', 'lavfi', '-i', 'sine=frequency=440:duration=6',
                        '-c:v', 'mpeg2video', '-c:a', 'aac', '-f', 'mpegts', base], check=True, env=ENV)
    return base


VIDEO = (0x02, VID, [desc(0x52, bytes([0x00]))])
AUDIO = (0x0f, AUD, [desc(0x52, bytes([0x10]))])
MARK = {
    'prg_c1': ('prg', desc(0xc1, b'\x00\x00')),
    'prg_de': ('prg', desc(0xde, b'\x00\x00')),
    'prg_f6': ('prg', desc(0xf6, b'\x00\x00')),
    'es_c8': ('es', desc(0xc8, b'\x00\x00')),
    'es_c1': ('es', desc(0xc1, b'\x00\x00')),
    'st_0d': ('st', None),
}
CAP = lambda dc, tag, pid=0x110: (0x06, pid, [desc(0x52, bytes([tag])), desc(0xfd, dc.to_bytes(2, 'big') + b'\x00')])


def spec_for(marks, extra_streams=()):
    prg = [MARK[m][1] for m in marks if MARK[m][0] == 'prg']
    es = [MARK[m][1] for m in marks if MARK[m][0] == 'es']
    streams = [(VIDEO[0], VID, VIDEO[2] + es), AUDIO] + list(extra_streams)
    if 'st_0d' in marks:
        streams.append((0x0d, 0x120, [desc(0x52, bytes([0x40]))]))
    return prg, streams


def run_synthetic():
    base = make_base()
    names = list(MARK)
    cases = []
    for r in (0, 1, 2, 3):
        for combo in itertools.combinations(names, r):
            cases.append((combo, () , len(combo) >= 2))
    cases.append((('prg_c1', 'prg_c1'), (), False))
    cases.append(((), (CAP(0x0008, 0x30),), True))
    cases.append(((), (CAP(0x0008, 0x38),), True))
    cases.append(((), (CAP(0x0008, 0x3f),), True))
    cases.append(((), (CAP(0x0012, 0x87),), True))
    cases.append(((), (CAP(0x0012, 0x88),), True))
    cases.append(((), (CAP(0x0008, 0x40),), False))
    cases.append(((), (CAP(0x0012, 0x30),), False))
    cases.append(((), (CAP(0x0009, 0x30),), False))
    cases.append((('prg_c1',), (CAP(0x0008, 0x30),), True))
    for marks, extra, expect in cases:
        prg, streams = spec_for(marks, extra)
        dst = os.path.join(WORK, 'case.ts')
        rewrite(base, dst, 1, lambda k, n: pmt_section(1, 3, prg, streams))
        out = inspect(dst, 3)
        label = '+'.join(marks) + ('+cap%s' % ','.join('%04x/%02x' % (0, 0) for _ in extra) if extra else '')
        if extra:
            e = extra[0]
            label = '+'.join(marks) + ('+' if marks else '') + 'cap(pid %x tag %s dc %s)' % (e[1], e[2][0][2:].hex(), e[2][1][2:4].hex())
        check(verdict(out) == ('1' if expect else '0'), 'rule: %-60s -> arib=%s (expect %s)' % (label or 'no markers', verdict(out), int(expect)))
    # caption exception also needs the caption-profile hook of 1006: arib_type must be present
    prg, streams = spec_for((), (CAP(0x0008, 0x30),))
    rewrite(base, os.path.join(WORK, 'case.ts'), 1, lambda k, n: pmt_section(1, 3, prg, streams))
    out = inspect(os.path.join(WORK, 'case.ts'), 3)
    check('markers=caption' in out, 'caption-only PMT reports arib_markers=caption')

    # re-evaluation on a later PMT version, both directions
    plain_prg, plain = spec_for(())
    arib_prg, arib = spec_for(('prg_c1', 'prg_de'))
    for first, second, v1, v2, name in ((plain_prg, arib_prg, 0, 1, 'non-ARIB v4 -> ARIB v5'),
                                         (arib_prg, plain_prg, 1, 0, 'ARIB v4 -> non-ARIB v5')):
        st1 = plain if v1 == 0 else arib
        st2 = plain if v2 == 0 else arib
        rewrite(base, os.path.join(WORK, 'reeval.ts'), 1,
                lambda k, n: pmt_section(1, 4, first, st1) if k < n // 2 else pmt_section(1, 5, second, st2))
        out = inspect(os.path.join(WORK, 'reeval.ts'), 100)
        check(re.search(r'pmt_version 4 -> 5 arib %d -> %d' % (v1, v2), out) is not None, 're-evaluation: ' + name)
        check(final_prog(out, 'arib') == str(v2), 're-evaluation final verdict arib=%d' % v2)

    # merge_pmt_versions: main audio tag 0x10 moves from PID 0x101 to 0x102 mid-file
    audio2 = (0x0f, 0x102, [desc(0x52, bytes([0x10]))])
    for aribish, tag in ((True, 'ARIB'), (False, 'non-ARIB')):
        prg = [MARK['prg_c1'][1], MARK['prg_de'][1]] if aribish else []

        def pick(k, n, prg=prg):
            if k < n // 2:
                return pmt_section(1, 4, prg, [VIDEO, AUDIO])
            return pmt_section(1, 5, prg, [VIDEO, audio2])
        rewrite(base, os.path.join(WORK, 'merge.ts'), 1, pick, audio_move=(0.5, 0x102))
        out = inspect(os.path.join(WORK, 'merge.ts'), 100)
        m = re.search(r'^final: nb_streams=(\d+) packets_by_stream:(.*)$', out, re.M)
        nb, pk = int(m.group(1)), m.group(2)
        pids = re.findall(r'idx(\d+)/pid(0x[0-9a-f]+)=(\d+)', pk)
        if aribish:
            check(nb == 2 and len(pids) == 2 and any(p[1] == '0x102' for p in pids) and not any(p[1] == '0x101' for p in pids),
                  'merge (ARIB): PID move 0x101 -> 0x102 keeps one AVStream and reports the new PID (%s)' % pk.strip())
        else:
            check(nb == 3, 'merge (non-ARIB): PID move creates a new AVStream as before (nb_streams=%d)' % nb)

    for aribish, tag in ((True, 'ARIB'), (False, 'non-ARIB')):
        prg = [MARK['prg_c1'][1], MARK['prg_de'][1]] if aribish else []

        def pick_late(k, n, prg=prg):
            if k < int(n * 0.55):
                return pmt_section(1, 4, prg, [VIDEO, AUDIO])
            return pmt_section(1, 5, prg, [VIDEO, audio2])
        rewrite(base, os.path.join(WORK, 'merge2.ts'), 1, pick_late, audio_move=(0.5, 0x102))
        out = inspect(os.path.join(WORK, 'merge2.ts'), 100)
        m = re.search(r'^final: nb_streams=(\d+) packets_by_stream:(.*)$', out, re.M)
        nb, pk = int(m.group(1)), m.group(2)
        if aribish:
            check(nb == 2, 'merge (ARIB), new PID sends PES before the PMT lists it: still one AVStream (nb_streams=%d: %s)' % (nb, pk.strip()))
        else:
            check(nb == 3, 'merge (non-ARIB), PES before PMT: new AVStream as before (nb_streams=%d)' % nb)


def run_real():
    cases = [
        (os.path.join(ROOT, 'testmedia', 'arib', 'tbs-captions.ts'), '1', 'caption'),
    ]
    fuji = [os.path.join(r'Y:\2026-10-06', f) for f in os.listdir(r'Y:\2026-10-06') if f.startswith('フジ')][0]
    cases += [(fuji, '1', None)]
    z = os.path.join(r'Z:\recordings', '2025-08-17')
    cases += [(os.path.join(z, [f for f in os.listdir(z) if 'ゲーセン少女' in f][0]), '1', None)]
    for path, expect, _ in cases:
        out = inspect(path, 3)
        check(verdict(out) == expect, 'real: %s -> arib=%s' % (os.path.basename(path), verdict(out)))
    atsc = os.path.join(r'Z:\recordings', '(애국가SIGN-OFF).ts')
    out = inspect(atsc, 3)
    check(verdict(out) in (None, '0', '(unset)') or 'OPEN FAILED' in out, 'real: ATSC sign-off -> non-ARIB (arib=%s)' % verdict(out))


if __name__ == '__main__':
    run_synthetic()
    if '--real' in sys.argv:
        run_real()
    print('\n%d failure(s)' % len(FAILS))
    sys.exit(1 if FAILS else 0)
