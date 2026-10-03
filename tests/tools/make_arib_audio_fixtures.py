#!/usr/bin/env python3
"""Generates the synthetic ARIB audio fixtures (testmedia/arib-audio/*.ts + *.json manifests).

Step 1: ffmpeg muxes testsrc2 + A1 (440 Hz) + A2 (880 Hz) + two alternative A1 streams into a base TS.
Step 2: this script rewrites PSI (PAT/PMT with versions, stream_identifier 0x52, ARIB markers) and drops audio
        PES units by PID and PTS to build each case.

Usage: python -I make_arib_audio_fixtures.py [--out DIR] [--ffmpeg PATH] [--only NAME ...] [--force] [--validate]
All manifest times are seconds since T0 = the smallest PTS in the file.
"""
import argparse, json, os, struct, subprocess, tempfile

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
DEF_FFMPEG = os.path.join(REPO, 'ffmpeg', 'dist', 'win64', 'ffmpeg.exe')
DEF_FFPROBE = os.path.join(REPO, 'ffmpeg', 'dist', 'win64', 'ffprobe.exe')
DEF_OUT = os.path.join(REPO, 'testmedia', 'arib-audio')

DUR = 16.0
RATE = 48000
PID_V, PID_A1, PID_A2, PID_A1B, PID_A151, PID_PMT = 0x111, 0x112, 0x113, 0x114, 0x115, 0x1F0
BASE_PIDS = {PID_V, PID_A1, PID_A2, PID_A1B, PID_A151}
T = 5.0
T2 = 9.0
PMT_VERSION0 = 3
SR_TABLE = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350]


def make_base(ffmpeg, path, a2_bitrate='96k'):
    fc = (f"sine=frequency=440:sample_rate={RATE}:duration={DUR},asplit=3[s1][s2][s3];"
          f"[s1]pan=stereo|c0=c0|c1=c0[a1];[s2]pan=stereo|c0=c0|c1=c0[a1b];[s3]pan=5.1|FL=c0|FR=c0[a151];"
          f"sine=frequency=880:sample_rate={RATE}:duration={DUR},pan=stereo|c0=c0|c1=c0[a2]")
    cmd = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-y',
           '-f', 'lavfi', '-i', f'testsrc2=size=640x360:rate=30000/1001:duration={DUR}',
           '-filter_complex', fc,
           '-map', '0:v', '-map', '[a1]', '-map', '[a2]', '-map', '[a1b]', '-map', '[a151]',
           '-c:v', 'mpeg2video', '-b:v', '2M', '-bf', '0', '-g', '15', '-pix_fmt', 'yuv420p',
           '-c:a', 'aac', '-b:a', '96k', '-b:a:1', a2_bitrate, '-b:a:3', '192k',
           '-streamid', f'0:{PID_V}', '-streamid', f'1:{PID_A1}', '-streamid', f'2:{PID_A2}',
           '-streamid', f'3:{PID_A1B}', '-streamid', f'4:{PID_A151}',
           '-f', 'mpegts', '-mpegts_pmt_start_pid', str(PID_PMT), '-pes_payload_size', '0', '-muxdelay', '0.1',
           path]
    subprocess.run(cmd, check=True)


def crc32_mpeg(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def psi_packet(pid, sec, cc):
    pl = bytes([0]) + sec
    pl += b'\xff' * (184 - len(pl))
    return bytes([0x47, 0x40 | (pid >> 8), pid & 0xFF, 0x10 | (cc & 15)]) + pl


def section(table_id, id_field, version, body):
    inner = struct.pack('>HBBB', id_field, 0xC1 | (version << 1), 0, 0) + body
    ln = len(inner) + 4
    s = bytes([table_id, 0xB0 | (ln >> 8), ln & 0xFF]) + inner
    return s + struct.pack('>I', crc32_mpeg(s))


def pat_section():
    return section(0x00, 1, 0, struct.pack('>HH', 1, 0xE000 | PID_PMT))


def desc(tag, payload):
    return bytes([tag, len(payload)]) + payload


def pmt_section(version, streams, arib):
    """streams: list of (pid, stream_type, tag_or_None). ARIB markers: program-level 0xC1 and video ES-level 0xC8."""
    prog_desc = desc(0xC1, b'\x00') if arib else b''
    body = struct.pack('>HH', 0xE000 | PID_V, 0xF000 | len(prog_desc)) + prog_desc
    for pid, st, tag in streams:
        d = b''
        if tag is not None:
            d += desc(0x52, bytes([tag]))
        if arib and pid == PID_V:
            d += desc(0xC8, b'\x07')
        body += struct.pack('>BHH', st, 0xE000 | pid, 0xF000 | len(d)) + d
    return section(0x02, 1, version, body)


class Unit:
    __slots__ = ('pid', 'pts', 'idx', 'payload', 'dur', 'keep', 'channels')

    def __init__(self, pid, pts, idx):
        self.pid, self.pts, self.idx = pid, pts, idx
        self.payload = bytearray()
        self.dur = 0.0
        self.keep = True
        self.channels = 0


def parse_ts(path):
    data = open(path, 'rb').read()
    pkts = [data[i:i + 188] for i in range(0, len(data) - 187, 188)]
    units, unit_of, cur = [], [None] * len(pkts), {}
    pcr = [None] * len(pkts)
    for i, p in enumerate(pkts):
        assert p[0] == 0x47
        pid = ((p[1] & 0x1F) << 8) | p[2]
        afc = (p[3] >> 4) & 3
        off = 4
        if afc & 2:
            if p[4] >= 7 and p[5] & 0x10:
                b = struct.unpack('>I', p[6:10])[0]
                pcr[i] = (b << 1 | (p[10] >> 7)) / 90000.0
            off += 1 + p[4]
        if pid not in BASE_PIDS:
            continue
        pl = p[off:] if afc & 1 else b''
        if p[1] & 0x40:
            assert pl[:3] == b'\x00\x00\x01'
            pts = None
            if pl[7] & 0x80:
                b = pl[9:14]
                pts = (((b[0] >> 1) & 7) << 30 | b[1] << 22 | (b[2] >> 1) << 15 | b[3] << 7 | b[4] >> 1) / 90000.0
            u = Unit(pid, pts, i)
            u.payload += pl[9 + pl[8]:]
            units.append(u)
            cur[pid] = u
        else:
            cur[pid].payload += pl
        unit_of[i] = cur[pid]
    for u in units:
        if u.pid == PID_V:
            continue
        pos, n, p = 0, 0, u.payload
        sr = 0
        while pos + 7 <= len(p) and p[pos] == 0xFF and (p[pos + 1] & 0xF0) == 0xF0:
            fl = ((p[pos + 3] & 3) << 11) | (p[pos + 4] << 3) | (p[pos + 5] >> 5)
            sr = SR_TABLE[(p[pos + 2] >> 2) & 15]
            u.channels = ((p[pos + 2] & 1) << 2) | (p[pos + 3] >> 6)
            n += 1
            pos += fl
        assert n > 0 and sr == RATE, (u.pid, n)
        u.dur = n * 1024.0 / sr
    return pkts, pcr, unit_of, units


def merge(spans, eps=1e-4):
    out = []
    for a, b in sorted(spans):
        if out and a - out[-1][1] <= eps:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return out


def r3(x):
    return round(x, 4)


def subtract(a, b):
    out = []
    for x0, x1 in a:
        segs = [[x0, x1]]
        for y0, y1 in b:
            nxt = []
            for s0, s1 in segs:
                if y1 <= s0 or y0 >= s1:
                    nxt.append([s0, s1])
                else:
                    if y0 > s0:
                        nxt.append([s0, y0])
                    if y1 < s1:
                        nxt.append([y1, s1])
            segs = nxt
        out += segs
    return merge([s for s in out if s[1] - s[0] > 1e-6])


def build_case(case, pkts, pcr, unit_of, units, t0, out_path):
    arib = case['arib']
    tags = (lambda t: t) if arib else (lambda t: None)
    A1 = (PID_A1, 0x0F, tags(0x10))
    A2 = (PID_A2, 0x0F, tags(0x11))
    V = (PID_V, 0x02, None)
    epochs = case['epochs'](A1, A2, V, tags)  # [(anchor_time or None, streams)]
    for u in units:
        if u.pid != PID_V:
            u.keep = case['keep'](u.pid, u.pts - t0)
    anchors = {}
    for ei, (at, _) in enumerate(epochs):
        if at is not None:
            a = next(u for u in units if u.pid == PID_A1 and u.pts - t0 >= at - 1e-9)
            anchors[a.idx] = ei
    out, cc = [], {}
    state = {'ver': PMT_VERSION0, 'streams': epochs[0][1]}
    pmt_events = [{'t': 0.0, 'version': PMT_VERSION0, 'pids': [s[0] for s in epochs[0][1]]}]

    def emit_psi():
        out.append(psi_packet(0, pat_section(), cc.get(0, 0))); cc[0] = (cc.get(0, 0) + 1) & 15
        out.append(psi_packet(PID_PMT, pmt_section(state['ver'], state['streams'], arib), cc.get(PID_PMT, 0)))
        cc[PID_PMT] = (cc.get(PID_PMT, 0) + 1) & 15

    emit_psi()
    next_psi = 0.0
    for i, p in enumerate(pkts):
        pid = ((p[1] & 0x1F) << 8) | p[2]
        if pid not in BASE_PIDS:
            continue
        if i in anchors:
            state['ver'] = (state['ver'] + 1) & 31
            state['streams'] = epochs[anchors[i]][1]
            pmt_events.append({'t': r3(unit_of[i].pts - t0), 'version': state['ver'], 'pids': [s[0] for s in state['streams']]})
            emit_psi()
        if pcr[i] is not None and pcr[i] >= next_psi:
            emit_psi()
            next_psi = pcr[i] + 0.1
        if pid != PID_V and not unit_of[i].keep:
            continue
        b = bytearray(p)
        if (p[3] >> 4) & 1:
            b[3] = (p[3] & 0xF0) | cc.get(pid, 0)
            cc[pid] = (cc.get(pid, 0) + 1) & 15
        out.append(bytes(b))
    open(out_path, 'wb').write(b''.join(out))

    def cov(pids):
        return merge([[u.pts - t0, u.pts - t0 + u.dur] for u in units if u.pid in pids and u.keep])
    a1c, a2c = cov({PID_A1, PID_A1B, PID_A151}), cov({PID_A2})
    return {
        'a1': a1c, 'a2': a2c, 'pmt_events': pmt_events, 'a1_first_s': r3(a1c[0][0]), 'a1_end_s': r3(a1c[-1][1]),
        'a1_channels': {hex(pid): next(u.channels for u in units if u.pid == pid) for pid in (PID_A1, PID_A1B, PID_A151)},
    }


def timeline(a1, a2, end):
    """Expected tone spans for both preferences and the silence spans (ideal coverage, no hold)."""
    main = [{'start': r3(s), 'end': r3(e), 'tone': 440} for s, e in a1]
    com = [(s, e, 880) for s, e in a2] + [(s, e, 440) for s, e in subtract(a1, a2)]
    com.sort()
    com = [{'start': r3(s), 'end': r3(e), 'tone': t} for s, e, t in com]
    covered = [[x['start'], x['end']] for x in com]
    switches = [c['start'] for k, c in enumerate(com) if k and c['tone'] != com[k - 1]['tone']
                and abs(c['start'] - com[k - 1]['end']) < 1e-3]
    return main, com, subtract([[0.0, end]], a1), subtract([[0.0, end]], merge(covered)), switches


def cases():
    def e_static(A1, A2, V, tags): return [(None, [V, A1, A2])]
    no_alt = lambda pid: pid not in (PID_A1B, PID_A151)
    cs = {}
    f1_keep = lambda pid, t: no_alt(pid) and not (pid == PID_A2 and t >= T)
    f1_epochs = lambda A1, A2, V, tags: [(None, [V, A1, A2]), (T, [V, A1])]
    cs['f1-pmt-removes-a2'] = dict(desc='F1: PMT v+1 at T removes A2 (A2 packets also stop)', arib=True, epochs=f1_epochs, keep=f1_keep)
    cs['f1-nonarib'] = dict(desc='F1 without ARIB markers and without stream_identifier tags', arib=False, epochs=f1_epochs, keep=f1_keep)
    cs['f1b-a2-preferred-by-ffmpeg'] = dict(
        desc='F1 with A2 at a much higher bitrate, so FFmpeg picks A2 by default (the Fuji failure): PMT v+1 at T removes A2',
        arib=True, epochs=f1_epochs, keep=f1_keep, a2_bitrate='256k')
    cs['f2a-a2-stops-resumes'] = dict(
        desc='F2a: PMT unchanged, A2 packets stop at T and resume at T2', arib=True, epochs=e_static,
        keep=lambda pid, t: no_alt(pid) and not (pid == PID_A2 and T <= t < T2))
    cs['f2b-a2-stops'] = dict(
        desc='F2b: PMT unchanged, A2 packets stop at T and never resume', arib=True, epochs=e_static,
        keep=lambda pid, t: no_alt(pid) and not (pid == PID_A2 and t >= T))
    cs['f3a-a1-new-pid'] = dict(
        desc='F3a: A1 moves from 0x112 to 0x114 (same tag 0x10) at T via a new PMT', arib=True,
        epochs=lambda A1, A2, V, tags: [(None, [V, A1, A2]), (T, [V, (PID_A1B, 0x0F, tags(0x10)), A2])],
        keep=lambda pid, t: (pid == PID_A1 and t < T) or (pid == PID_A1B and t >= T) or pid == PID_A2)
    cs['f3b-a1-new-pid-51'] = dict(
        desc='F3b: A1 moves from 0x112 (2ch) to 0x115 (5.1) at T via a new PMT', arib=True,
        epochs=lambda A1, A2, V, tags: [(None, [V, A1, A2]), (T, [V, (PID_A151, 0x0F, tags(0x10)), A2])],
        keep=lambda pid, t: (pid == PID_A1 and t < T) or (pid == PID_A151 and t >= T) or pid == PID_A2)
    cs['f4-a2-added'] = dict(
        desc='F4: A2 absent at the start, added by a PMT version at T', arib=True,
        epochs=lambda A1, A2, V, tags: [(None, [V, A1]), (T, [V, A1, A2])],
        keep=lambda pid, t: pid == PID_A1 or (pid == PID_A2 and t >= T))
    G = [(4.0, 0.05), (7.0, 0.15), (10.0, 0.4)]
    cs['f5-a2-gaps'] = dict(
        desc='F5: A2 gaps of nominal 50/150/400 ms at 4/7/10 s (quantised to AAC frames)', arib=True, epochs=e_static,
        keep=lambda pid, t: pid == PID_A1 or (pid == PID_A2 and not any(g <= t < g + d for g, d in G)))
    cs['f6-both-missing'] = dict(
        desc='F6: A1 and A2 both missing for 2 s from T', arib=True, epochs=e_static,
        keep=lambda pid, t: pid in (PID_A1, PID_A2) and not (T <= t < T + 2.0))
    return cs


def scan_pmts(path):
    """Independent PSI scan (CRC-checked): one entry per PMT version change."""
    data = open(path, 'rb').read()
    res, last, t = [], None, 0.0
    for i in range(0, len(data) - 187, 188):
        p = data[i:i + 188]
        pid = ((p[1] & 0x1F) << 8) | p[2]
        if pid == PID_V and (p[3] >> 4) & 2 and p[4] >= 7 and p[5] & 0x10:
            b = struct.unpack('>I', p[6:10])[0]
            t = (b << 1 | (p[10] >> 7)) / 90000.0
        if pid == PID_PMT and p[1] & 0x40:
            s = p[5:]
            sl = ((s[1] & 15) << 8) | s[2]
            assert crc32_mpeg(s[:3 + sl]) == 0, 'bad PMT CRC'
            ver = (s[5] >> 1) & 31
            if ver != last:
                last = ver
                pil = ((s[10] & 15) << 8) | s[11]
                pos, end, es, markers, q = 12 + pil, 3 + sl - 4, [], [], 12
                while q < 12 + pil:
                    markers.append(hex(s[q])); q += 2 + s[q + 1]
                while pos < end:
                    st, epid, il = s[pos], ((s[pos + 1] & 31) << 8) | s[pos + 2], ((s[pos + 3] & 15) << 8) | s[pos + 4]
                    q, tag, dl = pos + 5, None, []
                    while q < pos + 5 + il:
                        dl.append(hex(s[q]))
                        if s[q] == 0x52:
                            tag = hex(s[q + 2])
                        q += 2 + s[q + 1]
                    es.append((hex(epid), hex(st), tag, dl))
                    pos += 5 + il
                res.append({'pcr_s': round(t, 3), 'version': ver, 'program_descriptors': markers, 'streams': es})
    return res


def validate(path, ffprobe):
    print('==', os.path.basename(path))
    for e in scan_pmts(path):
        print('  PMT v%d at pcr %.3fs prog_desc=%s' % (e['version'], e['pcr_s'], e['program_descriptors']))
        for s in e['streams']:
            print('     pid', s[0], 'type', s[1], 'tag', s[2], 'desc', s[3])
    r = subprocess.run([ffprobe, '-v', 'error', '-of', 'json', '-show_format', '-show_streams', '-show_packets',
                        '-show_entries', 'format=start_time,duration:stream=index,id,codec_name,channels:packet=stream_index,pts_time',
                        path], capture_output=True, text=True, check=True)
    j = json.loads(r.stdout)
    print('  format start/duration:', j['format']['start_time'], j['format']['duration'])
    rng = {}
    for pk in j['packets']:
        if 'pts_time' not in pk:
            continue
        x = rng.setdefault(pk['stream_index'], [1e9, -1e9, 0])
        t = float(pk['pts_time']); x[0] = min(x[0], t); x[1] = max(x[1], t); x[2] += 1
    for s in j['streams']:
        x = rng.get(s['index'])
        print('  stream %d id=%s %s ch=%s packets=%s' % (s['index'], s.get('id'), s['codec_name'], s.get('channels'),
              ('%d pts %.3f..%.3f' % (x[2], x[0], x[1])) if x else 'none'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=DEF_OUT)
    ap.add_argument('--ffmpeg', default=DEF_FFMPEG)
    ap.add_argument('--ffprobe', default=DEF_FFPROBE)
    ap.add_argument('--only', nargs='*')
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--validate', action='store_true', help='print PSI + ffprobe validation of existing fixtures')
    a = ap.parse_args()
    cs = cases()
    names = a.only or list(cs)
    os.makedirs(a.out, exist_ok=True)
    if a.validate:
        for n in names:
            validate(os.path.join(a.out, n + '.ts'), a.ffprobe)
        return
    todo = [n for n in names if a.force or not os.path.exists(os.path.join(a.out, n + '.ts'))]
    if not todo:
        print('up to date')
        return
    with tempfile.TemporaryDirectory() as td:
        parsed = {}
        for n in todo:
            c = cs[n]
            br = c.get('a2_bitrate', '96k')
            if br not in parsed:
                base = os.path.join(td, 'base-%s.ts' % br)
                make_base(a.ffmpeg, base, br)
                parsed[br] = parse_ts(base)
            pkts, pcr, unit_of, units = parsed[br]
            t0 = min(u.pts for u in units if u.pts is not None)
            r = build_case(c, pkts, pcr, unit_of, units, t0, os.path.join(a.out, n + '.ts'))
            end = r['a1_end_s']
            main_t, com_t, sil_m, sil_c, sw = timeline(r['a1'], r['a2'], end)
            sp = lambda L: [[r3(x), r3(y)] for x, y in L]
            man = {
                'name': n, 'file': n + '.ts', 'description': c['desc'], 'arib': c['arib'], 'sample_rate': RATE,
                'time_origin': 'seconds since the smallest PTS in the file (T0)',
                'media_duration_s': end, 'a1_first_pts_s': r['a1_first_s'],
                'tones_hz': {'a1': 440, 'a2': 880},
                'pids': {'video': PID_V, 'a1': PID_A1, 'a2': PID_A2, 'a1_alt': PID_A1B, 'a1_alt_51': PID_A151, 'pmt': PID_PMT},
                'a1_channels_by_pid': r['a1_channels'],
                'boundary_T_s': T, 'resume_T2_s': T2 if n == 'f2a-a2-stops-resumes' else None,
                'pmt_events': r['pmt_events'], 'a1_coverage': sp(r['a1']), 'a2_coverage': sp(r['a2']),
                'expected': {'main': main_t, 'commentary': com_t,
                             'silence_main': sp(sil_m), 'silence_commentary': sp(sil_c),
                             'commentary_switch_times_s': sw},
                'notes': 'Commentary spans are the ideal coverage rule (no hold-back); a consumer with a hold time may stay on 440 up to hold_ms after an A2 gap.',
            }
            json.dump(man, open(os.path.join(a.out, n + '.json'), 'w'), indent=1)
            print('made', n)


if __name__ == '__main__':
    main()
