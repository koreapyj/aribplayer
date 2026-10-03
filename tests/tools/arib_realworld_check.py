#!/usr/bin/env python3
"""Read-only real-recording checks of the ARIB audio model (silent: SDL disk driver; the media is only opened for reading).

    python -I arib_realworld_check.py fuji   [--seconds 60]
    python -I arib_realworld_check.py audio2 [--seconds 90] [--ss 0]      (Noitamina recording: audio 2 present at the start,
                                                                         gone 2.4-62.4 s, back after that; preference = audio 2)
    python -I arib_realworld_check.py FILE.ts --pref commentary --seconds 30 --video

Prints the timeline of the audio model (playing source, audio 2 present / starved) once per second, the speed of the
video clock against the wall clock, the silent fraction of the captured audio, and a PASS/FAIL line per check. The
capture lands in artifacts/scratch/aribaudio/realworld-<name>.{pcm,json}, the log in artifacts/logs/."""
import argparse, json, math, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import capture_arib_audio as cap

def _find(folder, prefix, contains):
    """The recording by a stable part of its name (the real names carry full-width spaces and symbol characters)."""
    for n in os.listdir(folder):
        if n.startswith(prefix) and contains in n:
            return os.path.join(folder, n)
    raise FileNotFoundError(folder + ' ' + prefix + contains)


FUJI = _find('Y:/2026-10-06', 'フジテレビ - ', '火アニバル')
NOITAMINA = _find('Y:/2026-08-28', 'フジテレビ - ', '逃げ上手の若君第二期')


def rms_windows(pcm, rate, channels, win_s=0.1):
    import array
    a = array.array('h')
    with open(pcm, 'rb') as f:
        data = f.read()
    a.frombytes(data[:len(data) // 2 * 2])
    n = len(a) // channels
    w = int(win_s * rate)
    out = []
    for i in range(0, n - w + 1, w):
        seg = a[i * channels:(i + w) * channels]
        out.append(math.sqrt(sum(v * v for v in seg) / len(seg)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('which')
    ap.add_argument('--seconds', type=float, default=60)
    ap.add_argument('--pref', choices=['main', 'commentary'])
    ap.add_argument('--ss', type=float)
    ap.add_argument('--video', action='store_true')
    a = ap.parse_args()
    media = {'fuji': FUJI, 'audio2': NOITAMINA}.get(a.which, a.which)
    name = a.which if a.which in ('fuji', 'audio2') else os.path.basename(a.which)
    pref = a.pref or ('commentary' if a.which == 'audio2' else 'main')
    out = os.path.join(REPO, 'artifacts', 'scratch', 'aribaudio')
    pcm = os.path.join(out, f'realworld-{name}.pcm')
    # stop after N seconds of playing: a seek-free schedule ends the run through the timeout (the capture reports it)
    r = cap.capture(media, pcm, [], {'ss': str(a.ss)} if a.ss is not None else {}, None, False, pref,
                    timeout=a.seconds, log_path=os.path.join(REPO, 'artifacts', 'logs', f'aribaudio-realworld-{name}.log'),
                    video=a.video or a.which == 'fuji')
    json.dump(r, open(os.path.join(out, f'realworld-{name}.json'), 'w'), indent=1)
    tr = r['trace']
    print('media', name)
    print('error', r['error'], 'wall', r.get('wall_total'), 'pcm_bytes', r['pcm_bytes'])
    last = -1
    for t in tr:
        s = int(t['wall'])
        if s != last:
            last = s
            print(f"  t={t['wall']:7.2f} pos={t['pos']} aclk={t['audio_clock']} vclk={t['video_clock']} playing={t.get('arib_playing')} "
                  f"a2_present={t.get('a2_present')} a2_starved={t.get('a2_starved')} sw={t.get('arib_switches')} sil={t.get('arib_silence_ms')} "
                  f"drops={t['drops_early']}/{t['drops_late']}")
    ok = True

    def check(label, cond, detail=''):
        nonlocal ok
        ok = ok and bool(cond)
        print(('PASS ' if cond else 'FAIL ') + label + (' : ' + detail if detail else ''))

    ps = [t for t in tr if t['pos'] is not None and t['wall'] > 3]
    if len(ps) > 4:
        dp, dw = ps[-1]['pos'] - ps[0]['pos'], ps[-1]['wall'] - ps[0]['wall']
        check('position runs at wall speed', 0.95 <= dp / dw <= 1.05, f'{dp:.2f} media s in {dw:.2f} wall s ({dp / dw:.3f}x)')
    acl = [t['audio_clock'] for t in tr if t['audio_clock'] is not None]
    check('audio clock monotonic', all(b >= a_ - 0.15 for a_, b in zip(acl, acl[1:])))
    if len(acl) > 4 and len(ps) > 4:        # the audio clock is the stream's own timestamp (start time included)
        a0 = next(t for t in ps if t['audio_clock'] is not None)
        da, dp = ps[-1]['audio_clock'] - a0['audio_clock'], ps[-1]['pos'] - a0['pos']
        check('audio clock advances with the position', abs(da - dp) < 1.0, f'audio +{da:.2f} s, position +{dp:.2f} s')
    rate, ch = r['pcm_format']['sample_rate'], r['pcm_format']['channels']
    w = rms_windows(pcm, rate, ch)
    sil = sum(1 for v in w if v < 30) / max(len(w), 1)
    print(f'captured {len(w) / 10:.1f} s of audio, {sil * 100:.1f}% of the 100 ms windows silent')
    # the longest silent run (real programmes have short silences; a dead pipeline has seconds)
    longest = run = 0
    runs = []
    for i, v in enumerate(w):
        run = run + 1 if v < 30 else 0
        if run == 1:
            runs.append([i * 0.1, 0.0])
        if run:
            runs[-1][1] = run * 0.1
        longest = max(longest, run)
    print('silent runs >= 0.3 s (pcm start, length):', [(round(a_, 1), round(b_, 1)) for a_, b_ in runs if b_ >= 0.3])
    check('no long silence', longest * 0.1 < 3.0, f'longest silent run {longest * 0.1:.1f} s')
    check('video drops stay low', (tr[-1]['drops_late'] or 0) + (tr[-1]['drops_early'] or 0) < 30 if tr else False)
    if a.which == 'fuji':
        check('audio 1 plays (0x112 = 274)', all(t.get('arib_playing') in ('audio1', None) for t in tr) and
              any(t.get('a1_pid') == 274 for t in tr), 'pid ' + str({t.get('a1_pid') for t in tr}))
        check('arib model active', any(t.get('arib_playing') for t in tr))
        check('no silence synthesised', (tr[-1].get('arib_silence_ms') or 0) < 50)
    if a.which == 'audio2':
        seq = [(t['wall'], t['pos'], t.get('arib_playing')) for t in tr if t['pos'] is not None]
        print('segments (wall, pos, playing) at changes:')
        prev = None
        for wll, pos, pl in seq:
            if pl != prev:
                print(f'   wall {wll:7.2f} pos {pos:7.2f} -> {pl}')
                prev = pl
    print('OVERALL', 'PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
