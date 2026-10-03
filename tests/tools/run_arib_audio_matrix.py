#!/usr/bin/env python3
"""Every ARIB audio fixture x {main, commentary} x variants through the real libffplay (silent SDL disk driver).

    python -I run_arib_audio_matrix.py [--tag after] [--variants plain seek ...] [--cases NAME ...] [--pref main|commentary|both]

Variants (schedules are in media time; T of the fixtures is ~5 s):
  plain        no actions
  video        plain with the video decoded and presented in a hidden window (clock / drop checks)
  seek         playing: at 2 s seek forward to 6 s (across T), at 8 s seek back to 3 s (across T again)
  pause_step   (video) pause at 4.9 s, two frame steps, play: the pause and the steps sit on T
  start_paused open paused (start_paused=1), play after 2 s
  atempo       af=atempo=1.5
The preference is set on the instance before the open (ffp_set_audio_preference). Results: artifacts/scratch/aribaudio/<tag>-<variant>-<pref>/,
the table: artifacts/scratch/aribaudio/<tag>-matrix.md, logs: artifacts/logs/aribaudio-fixtures-<tag>-<variant>-<pref>.log.
A non-ARIB fixture has no preference, so only its main expectation is a verdict (commentary is reported as n/a)."""
import argparse, json, os, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
SCHED = {
    'seek': [{'at': {'pos': 2.0}, 'do': 'seek', 'value': 6.0}, {'at': {'pos': 8.0}, 'do': 'seek', 'value': 3.0}],
    'pause_step': [{'at': {'pos': 4.9}, 'do': 'pause'}, {'at': {'after_prev': 1.0}, 'do': 'step', 'value': 1},
                   {'at': {'after_prev': 0.7}, 'do': 'step', 'value': 1}, {'at': {'after_prev': 1.0}, 'do': 'play'}],
    'start_paused': [{'at': {'wall': 2.0}, 'do': 'play'}],
}
EXTRA = {
    'plain': [], 'video': ['--video'], 'seek': [], 'pause_step': ['--video'], 'start_paused': ['--start-paused'],
    'atempo': ['--af', 'atempo=1.5'],
}
ALL = list(EXTRA)


def run(tag, variant, pref, cases):
    t = f'{tag}-{variant}-{pref}'
    cmd = [sys.executable, '-I', os.path.join(HERE, 'run_arib_audio_cases.py'), '--pref', pref, '--set-preference', pref,
           '--tag', t, '--hold-ms', '1000' if pref == 'commentary' else '0'] + EXTRA[variant]
    if variant in SCHED:
        cmd += ['--schedule', json.dumps(SCHED[variant])]
    if cases:
        cmd += ['--cases'] + cases
    subprocess.run(cmd, capture_output=True, text=True)
    p = os.path.join(REPO, 'artifacts', 'scratch', 'aribaudio', t, 'summary.json')
    return json.load(open(p)) if os.path.exists(p) else []


def cell(row, pref):
    if 'error' in row and pref not in row:
        return 'ERR'
    r = row.get(pref)
    if r is None:
        return 'ERR'
    if row['case'] == 'f1-nonarib' and pref == 'commentary':
        return 'n/a'
    return 'ok' if r['ok'] else 'FAIL(' + ','.join(r['failed']) + ')'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tag', default='after')
    ap.add_argument('--variants', nargs='*', default=ALL)
    ap.add_argument('--cases', nargs='*')
    ap.add_argument('--pref', default='both', choices=['main', 'commentary', 'both'])
    a = ap.parse_args()
    prefs = ['main', 'commentary'] if a.pref == 'both' else [a.pref]
    out = {}
    with ThreadPoolExecutor(max_workers=len(prefs)) as ex:
        for v in a.variants:
            futs = {p: ex.submit(run, a.tag, v, p, a.cases) for p in prefs}
            for p, f in futs.items():
                out[(v, p)] = {r['case']: r for r in f.result()}
            print('done', v, flush=True)
    cases = sorted({c for d in out.values() for c in d})
    lines = ['| fixture | ' + ' | '.join(f'{v} {p}' for v in a.variants for p in prefs) + ' |',
             '|---|' + '---|' * (len(a.variants) * len(prefs))]
    for c in cases:
        lines.append(f'| {c} | ' + ' | '.join(cell(out[(v, p)].get(c, {'case': c}), p) for v in a.variants for p in prefs) + ' |')
    table = '\n'.join(lines)
    path = os.path.join(REPO, 'artifacts', 'scratch', 'aribaudio', f'{a.tag}-matrix.md')
    open(path, 'w', encoding='utf-8').write(table + '\n')
    print(table)


if __name__ == '__main__':
    main()
