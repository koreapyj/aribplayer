#!/usr/bin/env python3
"""Runs capture + analysis for fixtures: python -I run_arib_audio_cases.py [--cases NAME ...] [--pref main|commentary|both]
[--schedule JSON] [--af F] [--start-paused] [--video] [--tag LABEL] [--switch-tol S] [--hold-ms MS].
Writes artifacts/scratch/aribaudio/<tag>/<case>.{pcm,capture.json,<pref>.json} and appends to
artifacts/logs/aribaudio-fixtures-<tag>.log. Prints a table; with --pref both the capture is analysed against both expectations."""
import argparse, glob, json, os, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import analyze_arib_audio as an


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cases', nargs='*')
    ap.add_argument('--pref', default='both', choices=['main', 'commentary', 'both'])
    ap.add_argument('--schedule')
    ap.add_argument('--af')
    ap.add_argument('--start-paused', action='store_true')
    ap.add_argument('--video', action='store_true', help='decode+present video in a hidden off-screen host window (clock/drops checks)')
    ap.add_argument('--tag', default='baseline')
    ap.add_argument('--switch-tol', type=float, default=0.030)
    ap.add_argument('--hold-ms', type=float, default=0.0)
    ap.add_argument('--set-preference', choices=['main', 'commentary'], help='passes --preference to the capture (sets the audio preference before the open)')
    ap.add_argument('--fixtures', default=os.path.join(REPO, 'testmedia', 'arib-audio'))
    a = ap.parse_args()
    out = os.path.join(REPO, 'artifacts', 'scratch', 'aribaudio', a.tag)
    os.makedirs(out, exist_ok=True)
    log = os.path.join(REPO, 'artifacts', 'logs', f'aribaudio-fixtures-{a.tag}.log')
    names = a.cases or sorted(os.path.basename(p)[:-5] for p in glob.glob(os.path.join(a.fixtures, '*.json')))
    prefs = ['main', 'commentary'] if a.pref == 'both' else [a.pref]
    summary = []
    for n in names:
        man = os.path.join(a.fixtures, n + '.json')
        ts = os.path.join(a.fixtures, n + '.ts')
        pcm, cap = os.path.join(out, n + '.pcm'), os.path.join(out, n + '.capture.json')
        cmd = [sys.executable, '-I', os.path.join(HERE, 'capture_arib_audio.py'), ts, pcm, '--result', cap, '--log', log]
        if a.schedule: cmd += ['--schedule', a.schedule]
        if a.af: cmd += ['--af', a.af]
        if a.start_paused: cmd.append('--start-paused')
        if a.video: cmd.append('--video')
        if a.set_preference: cmd += ['--preference', a.set_preference]
        t0 = time.time()
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            summary.append({'case': n, 'error': (r.stdout + r.stderr)[-400:]})
            continue
        res = json.load(open(cap))
        speed = an._atempo(a.af) if a.af else 1.0
        row = {'case': n, 'capture_s': round(time.time() - t0, 1), 'error': res.get('error')}
        for p in prefs:
            rr = an.analyse(pcm, man, p, res['pcm_format']['sample_rate'], res['pcm_format']['channels'], speed, 0.0, res,
                            a.switch_tol, 0.25, a.hold_ms)
            json.dump(rr, open(os.path.join(out, f'{n}.{p}.json'), 'w'), indent=1)
            row[p] = {'ok': rr['ok'], 'failed': [k for k, c in rr['checks'].items() if not c['ok']],
                      'observed': [(g['class'], g['start_media_s'], g['end_media_s']) for g in rr['data']['observed_segments']],
                      'details': {k: c['detail'] for k, c in rr['checks'].items()}}
        summary.append(row)
        print(n, {p: (row[p]['ok'], row[p]['failed']) for p in prefs}, flush=True)
    json.dump(summary, open(os.path.join(out, 'summary.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()
