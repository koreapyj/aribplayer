#!/usr/bin/env python3
"""Silent audio capture through libffplay with an action schedule.

Audio goes to SDL's `disk` driver (SDL_AUDIODRIVER=disk, SDL_DISKAUDIOFILE=<pcm>): raw PCM, the device format
(libffplay opens s16, out_sample_rate / out_channels from ffp_get_debug_info "audio"; 48000 Hz stereo for the fixtures),
written by the audio callback and paced in real time by the driver (a 16 s file takes ~16 s). Nothing is audible.

Library use:
    import capture_arib_audio as cap
    r = cap.capture(media, pcm_path, actions=[...], open_options={...}, preference=None)
CLI:
    python -I capture_arib_audio.py MEDIA PCM_OUT [--result JSON] [--schedule JSON_OR_FILE] [--open k=v ...]
        [--af FILTER] [--start-paused] [--video] [--preference main|commentary] [--ss SEC] [--timeout SEC]

Schedule: a list of actions, each {"at": WHEN, "do": WHAT, ...}; executed in order.
  WHEN: {"pos": S}          media position (ffp_get_position) >= S
        {"wall": S}         S seconds after PREPARED
        {"after_prev": S}   S seconds after the previous action fired
  WHAT: "seek" (value=S absolute) | "seek_rel" (value=S) | "pause" | "play" | "toggle_pause"
        | "step" (value=+1/-1) | "af" (value="atempo=1.5") | "preference" (value="main"|"commentary")
        | "mute" (value=0/1) | "volume" (value=0..100)
  With start_paused the harness plays at once unless the schedule has its own "play"/"toggle_pause" action, in
  which case the open stays paused until that action (e.g. [{"at":{"wall":2},"do":"play"}]).
  The "preference" action calls ffp_set_audio_preference (PREFERENCE_API). The same preference can be applied before
  playback via capture(preference=...) / --preference, which is set on the instance before the open (so the first
  audio already follows it).
"""
import argparse, ctypes, json, os, sys, time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
DEF_DLL_DIR = os.path.join(REPO, 'ffmpeg', 'dist', 'win64')

EV_PREPARED, EV_POSITION, EV_STATE, EV_BUFFERING, EV_EOF, EV_ERROR = 1, 2, 3, 4, 5, 6
EV_STEP_DONE = 10

# Symbol and value mapping of the audio preference setter.
PREFERENCE_API = {'symbol': 'ffp_set_audio_preference', 'values': {'main': 0, 'commentary': 1}}

CB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p)
LOGCB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p)


class Host:
    """Hidden off-screen host window (like tests/Core.Tests) so that video is decoded and presented: needed to see
    the audio/video clock interplay. The default (nodisp) is audio only."""
    def __init__(self):
        from ctypes import wintypes
        u = ctypes.WinDLL('user32', use_last_error=True)
        self.u = u
        u.CreateWindowExW.restype = wintypes.HWND
        u.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD, ctypes.c_int, ctypes.c_int,
                                      ctypes.c_int, ctypes.c_int, wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, wintypes.LPVOID]
        u.DestroyWindow.argtypes = [wintypes.HWND]
        u.PeekMessageW.argtypes = [ctypes.c_void_p, wintypes.HWND, wintypes.UINT, wintypes.UINT, wintypes.UINT]
        u.DispatchMessageW.argtypes = [ctypes.c_void_p]
        self.msg = ctypes.create_string_buffer(64)
        # WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE, WS_POPUP; child: WS_CHILD|WS_VISIBLE
        self.parent = u.CreateWindowExW(0x80 | 0x08000000, 'STATIC', 'wvp-aribaudio-host', 0x80000000, -32000, -32000, 640, 360, None, None, None, None)
        self.child = u.CreateWindowExW(0, 'STATIC', 'video', 0x40000000 | 0x10000000, 0, 0, 640, 360, self.parent, None, None, None)

    def pump(self):
        while self.u.PeekMessageW(self.msg, None, 0, 0, 1):
            self.u.DispatchMessageW(self.msg)

    def close(self):
        self.u.DestroyWindow(self.parent)


class Lib:
    def __init__(self, dll_dir):
        os.add_dll_directory(dll_dir)
        os.environ['PATH'] = dll_dir + os.pathsep + os.environ.get('PATH', '')
        d = ctypes.CDLL(os.path.join(dll_dir, 'libffplay.dll'))
        self.d = d
        P, I, D, S = ctypes.c_void_p, ctypes.c_int, ctypes.c_double, ctypes.c_char_p
        for name, res, args in [
                ('ffp_create', P, [P, CB, P]), ('ffp_destroy', None, [P]), ('ffp_open', I, [P, S, S]),
                ('ffp_stop', I, [P]), ('ffp_play', I, [P]), ('ffp_pause', I, [P]), ('ffp_toggle_pause', I, [P]),
                ('ffp_seek', I, [P, D, I]), ('ffp_step_frame', I, [P, I]), ('ffp_set_volume', I, [P, I]),
                ('ffp_set_mute', I, [P, I]), ('ffp_get_position', D, [P]), ('ffp_get_duration', D, [P]),
                ('ffp_set_audio_filter', I, [P, S]), ('ffp_get_debug_info', P, [P]), ('ffp_get_media_info', P, [P]),
                ('ffp_free', None, [P]), ('ffp_set_log_level', None, [I]),
                ('ffp_set_log_callback', None, [LOGCB, P])]:
            f = getattr(d, name)
            f.restype, f.argtypes = res, args
        self.pref = getattr(d, PREFERENCE_API['symbol'])
        self.pref.restype, self.pref.argtypes = I, [P, I]

    def json_of(self, fn, p):
        ptr = fn(p)
        if not ptr:
            return None
        try:
            return json.loads(ctypes.string_at(ptr).decode('utf-8', 'replace'))
        finally:
            self.d.ffp_free(ptr)


def capture(media, pcm_path, actions=(), open_options=None, af=None, start_paused=False, preference=None,
            dll_dir=DEF_DLL_DIR, timeout=None, log_path=None, trace_hz=20, tail_s=0.7, video=False):
    """Plays `media` silently, writes PCM to pcm_path, returns a result dict (JSON-serialisable)."""
    pcm_path = os.path.abspath(pcm_path)
    if os.path.exists(pcm_path):
        os.remove(pcm_path)
    os.environ['SDL_AUDIODRIVER'] = 'disk'
    os.environ['SDL_DISKAUDIOFILE'] = pcm_path
    os.environ.pop('SDL_DISKAUDIODELAY', None)
    lib = Lib(dll_dir)
    events, logs = [], []
    t_start = time.monotonic()

    def on_event(user, ev, js):
        events.append({'t': round(time.monotonic() - t_start, 4), 'event': ev, 'json': js.decode('utf-8', 'replace') if js else None})

    def on_log(user, level, line):
        if line:
            logs.append(line.decode('utf-8', 'replace'))

    cb, lcb = CB(on_event), LOGCB(on_log)
    lib.d.ffp_set_log_callback(lcb, None)
    lib.d.ffp_set_log_level(32)
    host = Host() if video else None
    if host:
        import threading
        box = []
        th = threading.Thread(target=lambda: box.append(lib.d.ffp_create(host.child, cb, None)), daemon=True)
        th.start()
        while th.is_alive():
            host.pump()
            time.sleep(0.005)
        p = box[0]
    else:
        p = lib.d.ffp_create(None, cb, None)
    if not p:
        raise RuntimeError('ffp_create failed')
    opts = {} if video else {'nodisp': '1'}
    opts.update(open_options or {})
    if af:
        opts['af'] = af
    if start_paused:
        opts['start_paused'] = '1'
    kv = '\n'.join(f'{k}={v}' for k, v in opts.items())
    acts = [dict(a) for a in actions]
    if preference:
        # set before the open: the preference lives on the instance, so the very first audio already follows it
        lib.pref(p, PREFERENCE_API['values'][preference])
    res = {'media': os.path.abspath(media), 'pcm': pcm_path, 'open_options': opts, 'actions': [], 'trace': [],
           'events': events, 'error': None, 'start_paused': start_paused, 'preference': preference}
    try:
        rc = lib.d.ffp_open(p, os.path.abspath(media).encode('utf-8'), kv.encode('utf-8'))
        if rc != 0:
            raise RuntimeError(f'ffp_open rc={rc}')
        if not _wait(lambda: any(e['event'] in (EV_PREPARED, EV_ERROR) for e in events), 20, host):
            raise RuntimeError('no PREPARED')
        if any(e['event'] == EV_ERROR for e in events):
            raise RuntimeError('open error: ' + str([e for e in events if e['event'] == EV_ERROR]))
        dur = lib.d.ffp_get_duration(p)
        res['duration'] = dur
        res['media_info'] = lib.json_of(lib.d.ffp_get_media_info, p)
        if start_paused and not any(a['do'] in ('play', 'toggle_pause') for a in acts):
            lib.d.ffp_play(p)
        t_play = time.monotonic()
        speed = _speed_of(opts.get('af'))
        limit = timeout or ((dur if dur > 0 else 60) / speed + 15 + sum(a.get('hold', 0) for a in acts))
        pending, last_fire, last_trace, final_info = list(acts), time.monotonic(), 0.0, None
        eof_seen = None
        while True:
            now = time.monotonic()
            if now - t_play > limit:
                res['error'] = 'timeout'
                break
            pos = lib.d.ffp_get_position(p)
            while pending and _due(pending[0]['at'], now, t_play, last_fire, pos):
                a = pending.pop(0)
                _do(lib, p, a)
                last_fire = time.monotonic()
                res['actions'].append({'do': a['do'], 'value': a.get('value'), 'wall': round(last_fire - t_play, 4),
                                       'pos': None if pos != pos else round(pos, 4),
                                       'pcm_bytes': _size(pcm_path)})
            if now - last_trace >= 1.0 / trace_hz:
                last_trace = now
                di = lib.json_of(lib.d.ffp_get_debug_info, p)
                if di:
                    final_info = di
                    sy = di.get('sync') or {}
                    ar = di.get('arib_audio') or {}
                    roles = {r.get('role'): r for r in ar.get('roles', [])}
                    res['trace'].append({'arib_playing': ar.get('playing'), 'arib_switches': ar.get('switches'),
                                         'arib_silence_ms': ar.get('silence_ms'),
                                         'a1_present': (roles.get('audio1') or {}).get('present'),
                                         'a2_present': (roles.get('audio2') or {}).get('present'),
                                         'a2_starved': (roles.get('audio2') or {}).get('starved'),
                                         'a1_pid': (roles.get('audio1') or {}).get('pid'),
                                         'wall': round(now - t_play, 4), 'pos': None if pos != pos else round(pos, 4),
                                         'audio_clock': sy.get('audio_clock'), 'video_clock': sy.get('video_clock'),
                                         'drops_early': sy.get('frame_drops_early'), 'drops_late': sy.get('frame_drops_late'),
                                         'pcm_bytes': _size(pcm_path)})
            if eof_seen is None and any(e['event'] == EV_EOF for e in events):
                eof_seen = now
            if eof_seen is not None and now - eof_seen > tail_s and not pending:
                break
            if host:
                host.pump()
            time.sleep(0.005)
        res['audio'] = (final_info or {}).get('audio')
        res['debug_final'] = final_info
        res['wall_total'] = round(time.monotonic() - t_play, 3)
    except Exception as ex:
        res['error'] = repr(ex)
    finally:
        lib.d.ffp_stop(p)
        lib.d.ffp_destroy(p)
        lib.d.ffp_set_log_callback(LOGCB(), None)
        if host:
            host.close()
    res['pcm_bytes'] = _size(pcm_path)
    a = res.get('audio') or {}
    res['pcm_format'] = {'sample_fmt': 's16le', 'sample_rate': a.get('out_sample_rate') or 48000, 'channels': a.get('out_channels') or 2}
    res['log'] = logs
    if log_path:
        with open(log_path, 'a', encoding='utf-8') as f:
            f.write(f'--- {res["media"]}\n' + ''.join(logs))
    return res


def _wait(cond, secs, host=None):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if cond():
            return True
        if host:
            host.pump()
        time.sleep(0.01)
    return cond()


def _size(path):
    try:
        return os.path.getsize(path)
    except OSError:
        return 0


def _speed_of(af):
    if af and 'atempo=' in af:
        try:
            return float(af.split('atempo=')[1].split(',')[0])
        except ValueError:
            pass
    return 1.0


def _due(at, now, t_play, last_fire, pos):
    if 'pos' in at:
        return pos == pos and pos >= at['pos']
    if 'wall' in at:
        return now - t_play >= at['wall']
    return now - last_fire >= at.get('after_prev', 0)


def _do(lib, p, a):
    d, v = a['do'], a.get('value')
    if d == 'seek':
        lib.d.ffp_seek(p, float(v), 0)
    elif d == 'seek_rel':
        lib.d.ffp_seek(p, float(v), 1)
    elif d == 'pause':
        lib.d.ffp_pause(p)
    elif d == 'play':
        lib.d.ffp_play(p)
    elif d == 'toggle_pause':
        lib.d.ffp_toggle_pause(p)
    elif d == 'step':
        lib.d.ffp_step_frame(p, int(v))
    elif d == 'af':
        lib.d.ffp_set_audio_filter(p, (v or '').encode())
    elif d == 'mute':
        lib.d.ffp_set_mute(p, int(v))
    elif d == 'volume':
        lib.d.ffp_set_volume(p, int(v))
    elif d == 'preference':
        lib.pref(p, PREFERENCE_API['values'][v])
    else:
        raise ValueError('unknown action ' + d)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('media')
    ap.add_argument('pcm')
    ap.add_argument('--result')
    ap.add_argument('--schedule', help='JSON text or path to a JSON file')
    ap.add_argument('--open', action='append', default=[], help='extra ffp_open option k=v')
    ap.add_argument('--af')
    ap.add_argument('--start-paused', action='store_true')
    ap.add_argument('--preference', choices=['main', 'commentary'])
    ap.add_argument('--ss', type=float)
    ap.add_argument('--timeout', type=float)
    ap.add_argument('--dll-dir', default=DEF_DLL_DIR)
    ap.add_argument('--log')
    ap.add_argument('--video', action='store_true', help='decode+present video in a hidden off-screen host window')
    a = ap.parse_args()
    sched = []
    if a.schedule:
        sched = json.load(open(a.schedule)) if os.path.exists(a.schedule) else json.loads(a.schedule)
    oo = dict(k.split('=', 1) for k in a.open)
    if a.ss is not None:
        oo['ss'] = str(a.ss)
    r = capture(a.media, a.pcm, sched, oo, a.af, a.start_paused, a.preference, a.dll_dir, a.timeout, a.log, video=a.video)
    out = json.dumps(r, indent=1)
    if a.result:
        open(a.result, 'w').write(out)
    print(json.dumps({k: r.get(k) for k in ('error', 'duration', 'wall_total', 'pcm_bytes', 'pcm_format', 'actions')}, indent=1))
    sys.stdout.flush()
    os._exit(1 if r['error'] else 0)


if __name__ == '__main__':
    main()
