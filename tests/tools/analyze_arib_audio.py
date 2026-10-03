#!/usr/bin/env python3
"""Analyses a PCM capture (capture_arib_audio.py) against a fixture manifest (make_arib_audio_fixtures.py).

Usage: python -I analyze_arib_audio.py PCM MANIFEST --pref main|commentary [--result CAPTURE.json] [--json OUT.json]
         [--speed S] [--media-offset SEC] [--rate 48000] [--channels 2] [--ignore a,b ...] [--hold-ms MS]
Exit code 0 when every check passes, 1 otherwise. The JSON output has "checks" (name -> {ok, detail}), "ok", and the
measured data (observed segments, switches with deltas, silence spans, zero runs, click metrics, clock trace).

Time model: pcm time 0 is the device start; the first non-silent sample (lead, ~45 ms of device
startup latency) is taken as media_offset. Media time m = seg.media_s + (pcm_s - seg.pcm_s) * seg.speed. Without --result there is one segment
{pcm 0 -> media --media-offset, speed --speed}; with --result the capture's actions are applied: seek restarts the
mapping, an "af" with atempo changes the speed, pause..play spans are ignored, and every action excludes +-guard
seconds (default 0.25) from the tone/zero/click checks because the pcm position of an action is only known to
one device buffer (~43 ms) plus pipeline latency.

Checks and default thresholds (all overridable):
  content_end    last non-silent sample vs manifest media end            tol 0.10 s (inserted or lost samples shift it)
  tones          hop-wise tone class (20 ms Goertzel window, 5 ms hop)   >= 99.0 % of hops outside the +-switch_tol of
                 vs the expected tone per time                           every expected boundary and the guards
  switches       observed tone/silence boundaries vs expected ones       each within --switch-tol (0.030 s), none extra
  zero_runs      runs of |x| <= 2 lasting >= 1 ms outside expected       none
                 silence spans (+-switch_tol)
  clicks         max |first difference| within +-3 ms of each tone switch  <= 1.6 x the steady max first difference of the
                 and of each source boundary (PMT events, A1/A2 coverage  tone(s) on either side (measured away from
                 edges that stay non-silent)                              switches; 440 ~0.058 A, 880 ~0.115 A). Catches dropouts,
                                                                          steps and phase jumps; it cannot tell a clean hard cut
                                                                          from a crossfade
  clock          capture trace: audio_clock/pos non-decreasing (except   no regressions > 150 ms before the audio ends and away from seeks,
                 across seeks), frame drops                              drops_early/late must not grow
"""
import argparse, array, json, math, os, sys

SIL_RMS = 30.0          # int16 units; a 440/880 Hz tone of the fixtures has rms ~2900
WIN_S, HOP_S = 0.020, 0.005
TONES = (440, 880)


def load_pcm(path, rate, channels):
    a = array.array('h')
    with open(path, 'rb') as f:
        data = f.read()
    a.frombytes(data[:len(data) // 2 * 2])
    if sys.byteorder == 'big':
        a.byteswap()
    n = len(a) // channels
    if channels == 1:
        return [float(x) for x in a], n
    mono = [0.0] * n
    for c in range(channels):
        mono = [m + x for m, x in zip(mono, a[c::channels])]
    return [m / channels for m in mono], n


def goertzel_pow(x, start, n, f, rate):
    w = 2 * math.pi * f / rate
    coeff = 2 * math.cos(w)
    s1 = s2 = 0.0
    for i in range(start, start + n):
        s = x[i] + coeff * s1 - s2
        s2, s1 = s1, s
    return (s1 * s1 + s2 * s2 - coeff * s1 * s2) / (n * n) * 4.0   # ~ amplitude^2 of the component


def classify(x, rate, nsamp):
    win, hop = int(WIN_S * rate), int(HOP_S * rate)
    out = []
    for st in range(0, nsamp - win + 1, hop):
        seg = x[st:st + win]
        rms = math.sqrt(sum(v * v for v in seg) / win)
        if rms < SIL_RMS:
            c = 'S'
        else:
            p4, p8 = goertzel_pow(x, st, win, 440, rate), goertzel_pow(x, st, win, 880, rate)
            tot = rms * rms * 2.0
            if p4 > 0.8 * tot and p4 > 8 * p8:
                c = '440'
            elif p8 > 0.8 * tot and p8 > 8 * p4:
                c = '880'
            else:
                c = 'M'
        out.append((st / rate + WIN_S / 2, c, rms))
    return out


def spans_to_fn(spans):
    def f(t):
        for s, e, tone in spans:
            if s <= t < e:
                return str(tone)
        return 'S'
    return f


def _median(v):
    v = sorted(v)
    return v[len(v) // 2] if v else None


def _seek_landing(res, rate, lead, media_offset, speed, pcm_s, until_pcm, eps):
    """The media time where the audio restarts after a seek: libffplay lands on a keyframe (or, without video, on an audio
    packet position), not on the requested time. Taken from the capture's audio clock trace: audio_clock - eps - the pcm
    time since the seek, over the settled samples after it (eps = audio clock minus media time, from before the first
    action). None when the trace has too few samples."""
    bpf = res['pcm_format']['channels'] * 2
    vals = []
    for t in res.get('trace', []):
        ac = t.get('audio_clock')
        if ac is None:
            continue
        pt = t['pcm_bytes'] / bpf / rate
        if pcm_s + 0.35 <= pt <= until_pcm - 0.15:
            vals.append(ac - eps - (pt - pcm_s) * speed)
    return _median(vals) if len(vals) >= 3 else None


def segments_from_result(res, rate, speed0, media_offset, guard, lead=0.0):
    bpf = res['pcm_format']['channels'] * 2
    segs = [{'pcm_s': 0.0, 'media_s': media_offset, 'speed': speed0}]
    ignore, guards, speed = [], [], speed0
    pause_start = None
    acts = res.get('actions', [])
    pcms = [a['pcm_bytes'] / bpf / rate for a in acts]
    eps = None
    if res.get('trace') and acts:
        v = [t['audio_clock'] - ((t['pcm_bytes'] / bpf / rate - lead) * speed0 + media_offset) for t in res['trace']
             if t.get('audio_clock') is not None and lead + 0.3 <= t['pcm_bytes'] / bpf / rate <= pcms[0] - 0.1]
        eps = _median(v) if len(v) >= 3 else None
    for ai, a in enumerate(acts):
        pcm_s = pcms[ai]
        guards.append([pcm_s - guard, pcm_s + guard])
        d = a['do']
        if d in ('seek', 'seek_rel'):
            m = a['value'] if d == 'seek' else (a['pos'] or 0) + a['value']
            if eps is not None:
                land = _seek_landing(res, rate, lead, media_offset, speed, pcm_s,
                                     pcms[ai + 1] if ai + 1 < len(acts) else float('inf'), eps)
                if land is not None:
                    m = round(land, 3)
            segs.append({'pcm_s': pcm_s, 'media_s': m, 'speed': speed})
        elif d == 'af':
            speed = _atempo(a['value'])
            cur = media_at(segs, pcm_s)
            segs.append({'pcm_s': pcm_s, 'media_s': cur, 'speed': speed})
        elif d in ('pause', 'toggle_pause') and pause_start is None:
            pause_start = (pcm_s, media_at(segs, pcm_s))
        elif d in ('play', 'toggle_pause') and pause_start is not None:
            ignore.append([pause_start[0], pcm_s + guard])
            m = pause_start[1]
            if eps is not None:     # the audio that was already queued when the pause took effect continues after the resume
                land = _seek_landing(res, rate, lead, media_offset, speed, pcm_s,
                                     pcms[ai + 1] if ai + 1 < len(acts) else float('inf'), eps)
                if land is not None:
                    m = round(land, 3)
            segs.append({'pcm_s': pcm_s, 'media_s': m, 'speed': speed})
            pause_start = None
        elif d == 'step':
            ignore.append([pcm_s, pcm_s + 5 * guard])
    return segs, ignore, guards


def _atempo(af):
    try:
        return float(af.split('atempo=')[1].split(',')[0])
    except Exception:
        return 1.0


def media_at(segs, pcm_s):
    s = max((g for g in segs if g['pcm_s'] <= pcm_s), key=lambda g: g['pcm_s'], default=segs[0])
    return s['media_s'] + (pcm_s - s['pcm_s']) * s['speed']


def analyse(pcm, manifest, pref, rate=48000, channels=2, speed=1.0, media_offset=0.0, result=None, switch_tol=0.030,
            guard=0.25, hold_ms=0.0, extra_ignore=(), min_tone_frac=0.99, click_ratio=1.6, end_tol=0.10):
    x, nsamp = load_pcm(pcm, rate, channels)
    man = json.load(open(manifest)) if isinstance(manifest, str) else manifest
    segs, ignore, guards = [{'pcm_s': 0.0, 'media_s': media_offset, 'speed': speed}], [], []
    lead = next((i for i in range(nsamp) if abs(x[i]) > 3), 0) / rate
    if result:
        segs, ignore, guards = segments_from_result(result, rate, speed, media_offset, guard, lead)
    ignore = ignore + [list(p) for p in extra_ignore]
    segs[0]['pcm_s'] = lead
    exp = man['expected'][pref]
    exp_fn = spans_to_fn([(s['start'], s['end'], s['tone']) for s in exp])
    exp_sil = man['expected']['silence_main' if pref == 'main' else 'silence_commentary']
    media_end = man['media_duration_s']
    seeked = len(segs) > 1 or bool(ignore)

    def in_any(t, spans, pad=0.0):
        return any(a - pad <= t <= b + pad for a, b in spans)

    bounds = []
    segs_e = sorted([(s['start'], s['end'], str(s['tone'])) for s in exp] + [(a, b, 'S') for a, b in exp_sil])
    for i in range(1, len(segs_e)):
        if abs(segs_e[i][0] - segs_e[i - 1][1]) < 1e-3 and segs_e[i][2] != segs_e[i - 1][2]:
            bounds.append((segs_e[i][0], segs_e[i - 1][2], segs_e[i][2]))
    hold_s = hold_ms / 1000.0
    cls = classify(x, rate, nsamp)

    last = next((i for i in range(nsamp - 1, -1, -1) if abs(x[i]) > 3), 0)
    content_end_pcm = (last + 1) / rate
    exp_end_pcm = None
    if not seeked:
        exp_end_pcm = lead + (media_end - media_offset) / speed
    checks, data = {}, {}
    if exp_end_pcm is not None:
        d = content_end_pcm - exp_end_pcm
        checks['content_end'] = {'ok': abs(d) <= end_tol, 'detail': f'content ends {content_end_pcm:.3f}s, expected {exp_end_pcm:.3f}s (delta {d:+.3f}, tol {end_tol})'}
    else:
        checks['content_end'] = {'ok': True, 'detail': 'skipped (seek/pause/step in schedule)'}
    data['pcm_total_s'] = nsamp / rate
    data['lead_s'] = lead
    data['content_end_pcm_s'] = content_end_pcm

    bad, total, bad_spans = 0, 0, []
    guard_media = []
    for t, c, rms in cls:
        m = media_at(segs, t)
        if in_any(t, ignore) or in_any(t, guards):
            continue
        if m < man['a1_first_pts_s'] + 0.06 or m > media_end - 0.06:
            continue
        if any(abs(m - b[0]) <= switch_tol + WIN_S for b in bounds):
            continue
        if hold_s and any(b[0] <= m <= b[0] + hold_s for b in bounds if b[2] == '880'):
            continue
        total += 1
        e = exp_fn(m)
        if c != e:
            bad += 1
            if len(bad_spans) < 40:
                bad_spans.append({'pcm_s': round(t, 3), 'media_s': round(m, 3), 'got': c, 'want': e})
    frac = 1.0 - bad / max(total, 1)
    checks['tones'] = {'ok': frac >= min_tone_frac, 'detail': f'{frac * 100:.2f}% of {total} hops match the expected tone ({bad} off)'}
    data['tone_mismatches'] = bad_spans

    steady = [(t, c) for t, c, _ in cls if c != 'M']
    obs_sw = []
    for i in range(1, len(steady)):
        (t0, c0), (t1, c1) = steady[i - 1], steady[i]
        if c0 != c1:
            obs_sw.append({'pcm_s': (t0 + t1) / 2, 'from': c0, 'to': c1})
    all_sw_pcm = [s['pcm_s'] for s in obs_sw]
    for s in obs_sw:
        s['media_s'] = media_at(segs, s['pcm_s'])
    obs_sw = [s for s in obs_sw if man['a1_first_pts_s'] + 0.1 < s['media_s'] < media_end - 0.1 and not in_any(s['pcm_s'], ignore) and not in_any(s['pcm_s'], guards)]
    matched, extra, deltas = set(), [], []
    for s in obs_sw:
        cand = [(abs(s['media_s'] - b[0]), k) for k, b in enumerate(bounds) if b[1] == s['from'] and b[2] == s['to'] and (seeked or k not in matched)]
        if cand:
            dlt, k = min(cand)
            if dlt <= switch_tol + (hold_s if s['to'] == '880' else 0):
                matched.add(k)
                deltas.append({'expected_s': bounds[k][0], 'observed_s': round(s['media_s'], 4), 'delta_s': round(s['media_s'] - bounds[k][0], 4),
                               'from': s['from'], 'to': s['to']})
                continue
        extra.append({k: (round(v, 4) if isinstance(v, float) else v) for k, v in s.items()})
    missing = []
    for k, b in enumerate(bounds):
        p = _pcm_of(segs, b[0])
        hold_hidden = hold_s and b[2] == '880' and (in_any(p + hold_s, ignore) or in_any(p + hold_s, guards))
        if k not in matched and p is not None and not in_any(p, ignore) and not in_any(p, guards) and not hold_hidden:
            missing.append({'at_s': b[0], 'from': b[1], 'to': b[2]})
    checks['switches'] = {'ok': not extra and not missing,
                          'detail': f'{len(deltas)} matched within {switch_tol}s (max |delta| {max([abs(d["delta_s"]) for d in deltas] or [0]):.3f}), {len(extra)} unexpected, {len(missing)} missing'}
    data.update({'expected_boundaries': [{'t': b[0], 'from': b[1], 'to': b[2]} for b in bounds], 'switch_deltas': deltas,
                 'unexpected_switches': extra, 'missing_switches': missing})

    obs_segs = []
    for t, c in steady:
        if obs_segs and obs_segs[-1]['class'] == c:
            obs_segs[-1]['end_pcm_s'] = t
        else:
            obs_segs.append({'class': c, 'start_pcm_s': t, 'end_pcm_s': t})
    data['observed_segments'] = [{'class': g['class'], 'start_media_s': round(media_at(segs, g['start_pcm_s']), 3),
                                  'end_media_s': round(media_at(segs, g['end_pcm_s']), 3)} for g in obs_segs
                                 if g['end_pcm_s'] - g['start_pcm_s'] >= 0.04]

    runs, i = [], 0
    while i < nsamp:
        if abs(x[i]) <= 2:
            j = i
            while j < nsamp and abs(x[j]) <= 2:
                j += 1
            if (j - i) >= rate // 1000:
                runs.append((i / rate, j / rate))
            i = j
        else:
            i += 1
    bad_runs = []
    for a, b in runs:
        ma, mb = media_at(segs, a), media_at(segs, b)
        if mb > media_end - 0.02:
            continue
        if in_any(a, ignore) or in_any(b, ignore) or in_any(a, guards) or in_any(b, guards):
            continue
        if any(s - switch_tol <= ma and mb <= e + switch_tol for s, e in exp_sil):
            continue
        if b <= lead + 0.001 or (exp_end_pcm is not None and a >= exp_end_pcm - 0.01):
            continue
        bad_runs.append({'pcm_s': round(a, 4), 'len_ms': round((b - a) * 1000, 2), 'media_s': round(ma, 4)})
    checks['zero_runs'] = {'ok': not bad_runs, 'detail': f'{len(runs)} zero runs >= 1 ms, {len(bad_runs)} outside expected silence'}
    data['unexpected_zero_runs'] = bad_runs[:40]

    amp = max((abs(v) for v in x[: int(min(nsamp, 3 * rate))]), default=1.0) or 1.0
    probes = {round(e['t'], 3) for e in man.get('pmt_events', []) if e['t'] > 0}
    for key in ('a1_coverage', 'a2_coverage'):
        for a_, b_ in man.get(key, []):
            probes.update((round(a_, 3), round(b_, 3)))
    excl_pcm = all_sw_pcm + [q for q in (_pcm_of(segs, m_) for m_ in probes) if q is not None]
    excl_pcm += [g['pcm_s'] for g in segs[1:]]
    steady_max = {}
    for t, c, _ in cls:
        if c in ('440', '880') and not any(abs(t - w) < 0.05 for w in excl_pcm):
            st = int((t - WIN_S / 2) * rate)
            steady_max[c] = max(steady_max.get(c, 0.0), max(abs(x[k + 1] - x[k]) for k in range(st, st + int(WIN_S * rate) - 1)))
    A = max(abs(v) for v in x[: nsamp])
    A = max(A, 1.0)
    click = []
    for s in obs_sw:
        c = int(s['pcm_s'] * rate)
        lo, hi = max(c - int(0.003 * rate), 1), min(c + int(0.003 * rate), nsamp - 1)
        md = max((abs(x[k] - x[k - 1]) for k in range(lo, hi)), default=0.0)
        click.append({'media_s': round(s['media_s'], 4), 'from': s['from'], 'to': s['to'], 'max_first_diff': round(md, 1), 'rel_A': round(md / A, 4)})
    for mt in sorted(probes):
        if not (man['a1_first_pts_s'] + 0.1 < mt < media_end - 0.1) or exp_fn(mt - 0.006) == 'S' or exp_fn(mt + 0.006) == 'S':
            continue
        pc = _pcm_of(segs, mt)
        if pc is None or in_any(pc, ignore) or in_any(pc, guards) or any(abs(pc - o['pcm_s']) < 0.004 for o in obs_sw):
            continue
        c = int(pc * rate)
        lo, hi = max(c - int(0.003 * rate), 1), min(c + int(0.003 * rate), nsamp - 1)
        md = max((abs(x[k] - x[k - 1]) for k in range(lo, hi)), default=0.0)
        click.append({'media_s': mt, 'from': exp_fn(mt - 0.006), 'to': exp_fn(mt + 0.006), 'source_boundary': True,
                      'max_first_diff': round(md, 1), 'rel_A': round(md / A, 4)})
    if '440' in steady_max and '880' not in steady_max:
        steady_max['880'] = 2 * steady_max['440']
    if '880' in steady_max and '440' not in steady_max:
        steady_max['440'] = steady_max['880'] / 2
    bad_clicks = []
    for k in click:
        ref = max(steady_max.get(k['from'], 0.0), steady_max.get(k['to'], 0.0)) or A * 0.115
        k['limit'] = round(click_ratio * ref, 1)
        k['ratio_to_steady'] = round(k['max_first_diff'] / ref, 2)
        if 'S' not in (k['from'], k['to']) and k['max_first_diff'] > k['limit']:
            bad_clicks.append(k)
    worst = max([k['ratio_to_steady'] for k in click if 'S' not in (k['from'], k['to'])] or [0.0])
    checks['clicks'] = {'ok': not bad_clicks, 'detail': f'{len(click)} probes (tone switches + source boundaries: PMT events, A1/A2 coverage edges), worst {worst:.2f}x the steady max first-difference of the tone(s) (limit {click_ratio}x); steady max diff 440={steady_max.get("440", 0) / A:.3f}A 880={steady_max.get("880", 0) / A:.3f}A, A={A:.0f}'}
    data['click_metrics'] = click

    if result and result.get('trace'):
        regress, prev = [], None
        for tr in result['trace']:
            ac = tr.get('audio_clock')
            if ac is None:
                continue
            if prev is not None and ac < prev[1] - 0.15 and tr['wall'] < content_end_pcm - 0.3 and not any(abs(tr['wall'] - a['wall']) < 1.0 for a in result.get('actions', []) if a['do'].startswith('seek')):
                regress.append({'wall': tr['wall'], 'audio_clock': ac, 'prev': prev[1]})
            prev = (tr['wall'], ac)
        d0 = result['trace'][0]
        d1 = result['trace'][-1]
        drops = {'early': (d1.get('drops_early') or 0) - (d0.get('drops_early') or 0), 'late': (d1.get('drops_late') or 0) - (d0.get('drops_late') or 0)}
        checks['clock'] = {'ok': not regress and drops['early'] == 0 and drops['late'] == 0,
                           'detail': f'{len(regress)} audio-clock regressions, frame drops early={drops["early"]} late={drops["late"]}'}
        data['clock_regressions'] = regress
    data['segments'] = segs
    data['ignore_spans'] = ignore
    ok = all(c['ok'] for c in checks.values())
    return {'ok': ok, 'name': man['name'], 'preference': pref, 'checks': checks, 'data': data}


def _pcm_of(segs, media_s):
    for g in reversed(segs):
        if g['speed']:
            p = g['pcm_s'] + (media_s - g['media_s']) / g['speed']
            if p >= g['pcm_s']:
                return p
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pcm')
    ap.add_argument('manifest')
    ap.add_argument('--pref', choices=['main', 'commentary'], default='main')
    ap.add_argument('--result')
    ap.add_argument('--json')
    ap.add_argument('--rate', type=int)
    ap.add_argument('--channels', type=int)
    ap.add_argument('--speed', type=float, default=1.0)
    ap.add_argument('--media-offset', type=float, default=0.0)
    ap.add_argument('--switch-tol', type=float, default=0.030)
    ap.add_argument('--guard', type=float, default=0.25)
    ap.add_argument('--hold-ms', type=float, default=0.0)
    ap.add_argument('--ignore', nargs='*', default=[], help='pcm-time spans a,b to exclude')
    a = ap.parse_args()
    res = json.load(open(a.result)) if a.result else None
    fmt = (res or {}).get('pcm_format', {})
    rate = a.rate or fmt.get('sample_rate') or 48000
    ch = a.channels or fmt.get('channels') or 2
    ign = [tuple(float(v) for v in s.split(',')) for s in a.ignore]
    r = analyse(a.pcm, a.manifest, a.pref, rate, ch, a.speed, a.media_offset, res, a.switch_tol, a.guard, a.hold_ms, ign)
    if a.json:
        json.dump(r, open(a.json, 'w'), indent=1)
    print(('PASS ' if r['ok'] else 'FAIL ') + r['name'] + ' [' + a.pref + ']')
    for k, c in r['checks'].items():
        print(f'  {"ok  " if c["ok"] else "FAIL"} {k}: {c["detail"]}')
    sys.exit(0 if r['ok'] else 1)


if __name__ == '__main__':
    main()
