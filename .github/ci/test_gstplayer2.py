#!/usr/bin/env python3
"""
Headless tests for gstplayer2 (and gst-ifdsrc) on a CI runner.

gstplayer2 is started the way E2iPlayer / ServiceApp start it (getopt command
line, commands on stdin, JSON lines on stderr), with fakesink instead of the
box's dvb sinks. A local HTTP server serves progressive MP4, HLS and DASH
fixtures and records the request headers of every request.

usage: test_gstplayer2.py <gstplayer2 binary> <fixture dir> [<ifdsrc plugin dir>]
"""
import http.server
import json
import os
import shutil
import socketserver
import subprocess
import sys
import tempfile
import threading
import time

PLAYER = os.path.abspath(sys.argv[1])
MEDIA = os.path.abspath(sys.argv[2])
IFDSRC_DIR = os.path.abspath(sys.argv[3]) if len(sys.argv) > 3 else None

SINKS = ['-v', 'fakesink', '-a', 'fakesink']
HEADERS = {
    'Referer': 'http://referer.example/page',
    'Origin': 'http://origin.example',
    'X-Test': 'e2i-ci',
    'User-Agent': 'GstPlayer2CI/1.0',
    'Cookie': 'a=1; b=2',
}
HEADER_ARGS = []
for key, value in HEADERS.items():
    HEADER_ARGS += ['-H', '%s=%s' % (key, value)]

FAILED = []


# --------------------------------------------------------------- http server
REQUESTS = []
REQUESTS_LOCK = threading.Lock()


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=MEDIA, **kwargs)

    def do_GET(self):
        with REQUESTS_LOCK:
            REQUESTS.append((self.path, dict(self.headers.items())))
        return super().do_GET()

    def log_message(self, fmt, *args):
        pass


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def start_server():
    server = Server(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, 'http://127.0.0.1:%d' % server.server_address[1]


# --------------------------------------------------------------- player
class Run(object):
    def __init__(self, args, env=None, timeout=90, commands=None):
        self.args = args
        self.lines = []
        self.json = []
        self.bad_json = []
        full_env = dict(os.environ)
        full_env.update(env or {})
        start = time.time()
        proc = subprocess.Popen([PLAYER] + args, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                stderr=subprocess.PIPE, env=full_env)
        reader = threading.Thread(target=self._read, args=(proc.stderr,), daemon=True)
        reader.start()
        try:
            for delay, cmd in (commands or []):
                time.sleep(delay)
                if proc.poll() is not None:
                    break
                proc.stdin.write(cmd.encode() + b'\n')
                proc.stdin.flush()
            proc.wait(timeout=max(1, timeout - (time.time() - start)))
            self.timed_out = False
        except subprocess.TimeoutExpired:
            self.timed_out = True
            proc.kill()
            proc.wait()
        except BrokenPipeError:
            proc.wait(timeout=10)
            self.timed_out = False
        reader.join(5)
        self.returncode = proc.returncode

    def _read(self, pipe):
        for raw in iter(pipe.readline, b''):
            line = raw.decode('utf-8', 'replace').rstrip('\n')
            self.lines.append(line)
            if line.startswith('{'):
                try:
                    self.json.append(json.loads(line))
                except ValueError:
                    self.bad_json.append(line)

    def events(self, key):
        return [obj[key] for obj in self.json if key in obj]

    def dump(self):
        return '\n'.join('    | ' + line for line in self.lines[-40:])


def check(name, cond, detail=''):
    print('  [%s] %s%s' % ('ok' if cond else 'FAIL', name, (' - ' + detail) if (detail and not cond) else ''))
    if not cond:
        FAILED.append(name)
    return cond


def common_checks(tag, run, expect_eos=True):
    check('%s: no invalid JSON lines' % tag, not run.bad_json, repr(run.bad_json[:3]))
    check('%s: version 10022' % tag, [e.get('version') for e in run.events('GSTPLAYER_EXTENDED')] == [10022],
          repr(run.events('GSTPLAYER_EXTENDED')))
    glib = [l for l in run.lines if 'CRITICAL' in l or 'GLib-GObject-WARNING' in l]
    check('%s: no GLib CRITICAL lines' % tag, not glib, repr(glib[:2]))
    play = run.events('PLAYBACK_PLAY')
    check('%s: PLAYBACK_PLAY sts 0' % tag, bool(play) and play[0].get('sts') == 0, repr(play))
    if expect_eos:
        check('%s: ends by itself' % tag, not run.timed_out)
        check('%s: exit code 0' % tag, run.returncode == 0, 'rc=%r\n%s' % (run.returncode, run.dump()))
        check('%s: no GST_ERROR' % tag, not run.events('GST_ERROR'), repr(run.events('GST_ERROR')))


def header_checks(tag, requests, path_filter, what):
    matched = [(path, hdrs) for path, hdrs in requests if path_filter(path)]
    if not check('%s: %s requested' % (tag, what), bool(matched), 'requests: %r' % [p for p, _ in requests]):
        return
    for path, hdrs in matched:
        missing = []
        for key, value in HEADERS.items():
            got = hdrs.get(key)
            if key == 'Cookie':
                ok = got is not None and 'a=1' in got and 'b=2' in got
            else:
                ok = got == value
            if not ok:
                missing.append('%s=%r' % (key, got))
        check('%s: all headers on %s' % (tag, path), not missing, ', '.join(missing))


# --------------------------------------------------------------- tests
def test_local_file(tmp):
    print('local MP4 file')
    run = Run([os.path.join(MEDIA, 'a.mp4')] + SINKS)
    common_checks('local', run)
    lengths = [e.get('length', 0) for e in run.events('PLAYBACK_LENGTH')]
    check('local: duration reported', any(3.5 < l < 4.5 for l in lengths), repr(lengths))
    check('local: audio track list', bool(run.events('a_l')), run.dump())


def test_escaped_file_name(tmp):
    print('file name with quote and backslash (JSON escaping)')
    weird = os.path.join(tmp, 'we"ird\\name.mp4')
    shutil.copy(os.path.join(MEDIA, 'a.mp4'), weird)
    run = Run([weird] + SINKS)
    common_checks('escape', run)
    files = [e.get('file') for e in run.events('PLAYBACK_PLAY')]
    check('escape: file name round-trips', files == [weird], repr(files))


def test_ifd_missing_error(tmp):
    print('ifd:// without gst-ifdsrc (escaped GST_ERROR)')
    env = {'GST_PLUGIN_PATH': '', 'GST_REGISTRY': os.path.join(tmp, 'reg-noifd.bin')}
    run = Run([os.path.join(MEDIA, 'a.mp4'), '-t', '1000', '-l', '0'] + SINKS, env=env, timeout=30)
    check('ifd-missing: no invalid JSON lines', not run.bad_json, repr(run.bad_json[:3]))
    msgs = [e.get('msg', '') for e in run.events('GST_ERROR')] + [e.get('msg', '') for e in run.events('GST_MISSING_PLUGIN')]
    check('ifd-missing: error mentions "ifd" with quotes intact', any('"ifd"' in m or 'IFD' in m for m in msgs), repr(msgs) + '\n' + run.dump())


def test_ifd_growing_file(tmp):
    print('growing file through ifd:// (gst-ifdsrc, -t/-l)')
    if not IFDSRC_DIR:
        check('ifd-growing: plugin dir given', False)
        return
    src = os.path.join(MEDIA, 'long.ts')
    data = open(src, 'rb').read()
    dst = os.path.join(tmp, 'growing.ts')
    head = len(data) // 4
    with open(dst, 'wb') as f:
        f.write(data[:head])

    def writer():
        pos = head
        step = max(1, (len(data) - head) // 10)
        while pos < len(data):
            time.sleep(0.4)
            with open(dst, 'ab') as f:
                f.write(data[pos:pos + step])
            pos += step

    env = {'GST_PLUGIN_PATH': IFDSRC_DIR, 'GST_REGISTRY': os.path.join(tmp, 'reg-ifd.bin')}
    t = threading.Thread(target=writer, daemon=True)
    t.start()
    # poll the position like E2iPlayer does, then "t0" once the download is
    # complete (E2iPlayer's setDownloadFileTimeout(0)) so ifdsrc ends at EOF
    commands = [(0.5, 'j')] * 12 + [(0.5, 't0')] + [(0.5, 'j')] * 6
    # -l 0 must not swallow -t (the old getopt handling ignored the value)
    run = Run([dst, '-l', '0', '-t', '3000'] + SINKS, env=env, timeout=60, commands=commands)
    t.join()
    common_checks('ifd-growing', run)
    positions = [e.get('ms', 0) for e in run.events('J')]
    # the file is 8 s long and starts with its first quarter; without ifdsrc
    # (or without the timeout) playback ends at about 2 s
    check('ifd-growing: played past the initial part', bool(positions) and max(positions) > 5000,
          'positions %r\n%s' % (positions, run.dump()))


def test_http(tmp, base, name, path, prefix, segment_ext):
    print('%s over HTTP with -H headers' % name)
    with REQUESTS_LOCK:
        del REQUESTS[:]
    run = Run(['%s/%s' % (base, path)] + SINKS + HEADER_ARGS, timeout=90)
    common_checks(name, run)
    with REQUESTS_LOCK:
        requests = list(REQUESTS)
    header_checks(name, requests, lambda p: p == '/' + path, 'manifest/file')
    if segment_ext:
        header_checks(name, requests, lambda p: p.startswith('/' + prefix) and p.endswith(segment_ext), 'segments')


def segments(requests, prefix):
    """representation ids of the media segments fetched below prefix"""
    ids = set()
    for path, _ in requests:
        name = path.rsplit('/', 1)[-1]
        if path.startswith('/' + prefix) and name.startswith('chunk-'):
            ids.add(name.split('-')[1])
    return ids


def test_dash_variants(tmp, base):
    for name in ('vonly', 'aonly', 'hevc', 'eac3'):
        print('DASH %s' % name)
        run = Run(['%s/dash-%s/manifest.mpd' % (base, name)] + SINKS, timeout=60)
        common_checks('dash-' + name, run)
        lengths = [e.get('length', 0) for e in run.events('PLAYBACK_LENGTH')]
        check('dash-%s: prerolled (length reported)' % name, any(l > 10 for l in lengths), repr(lengths) + '\n' + run.dump())


def test_dash_multi(tmp, base):
    url = '%s/dash-multi/manifest.mpd' % base

    print('DASH 720p + 1080p, 2 audio languages (default)')
    with REQUESTS_LOCK:
        del REQUESTS[:]
    run = Run([url] + SINKS, timeout=60, commands=[(3, 'al')])
    common_checks('dash-multi', run)
    with REQUESTS_LOCK:
        segs = segments(REQUESTS, 'dash-multi/')
    check('dash-multi: 1080p representation used', '1' in segs, repr(sorted(segs)))
    check('dash-multi: first audio language played, second not downloaded', '2' in segs and '3' not in segs, repr(sorted(segs)))
    lists = run.events('a_l')
    names = [t.get('n') for t in lists[-1]] if lists else []
    check('dash-multi: audio track list has both languages', names == ['deu', 'eng'], repr(lists))

    print('DASH -M 1280x720')
    with REQUESTS_LOCK:
        del REQUESTS[:]
    run = Run([url, '-M', '1280x720'] + SINKS, timeout=60)
    common_checks('dash-multi-M', run)
    with REQUESTS_LOCK:
        segs = segments(REQUESTS, 'dash-multi/')
    check('dash-multi-M: only the 720p representation', '0' in segs and '1' not in segs, repr(sorted(segs)))

    print('DASH -i 1 (second audio language)')
    with REQUESTS_LOCK:
        del REQUESTS[:]
    run = Run([url, '-i', '1'] + SINKS, timeout=60)
    common_checks('dash-multi-i1', run)
    with REQUESTS_LOCK:
        segs = segments(REQUESTS, 'dash-multi/')
    check('dash-multi-i1: second language played', '3' in segs and '2' not in segs, repr(sorted(segs)))


def test_mpd_in_query(tmp, base):
    print('HLS URL with ".mpd" in the query (must not use the DASH pipeline)')
    run = Run(['%s/hls/index.m3u8?src=file.mpd' % base] + SINKS, timeout=60)
    common_checks('hls-mpd-query', run)


def main():
    tmp = tempfile.mkdtemp(prefix='gst2ci-')
    server, base = start_server()
    print('player: %s\nserver: %s\n' % (PLAYER, base))
    test_local_file(tmp)
    test_escaped_file_name(tmp)
    test_ifd_missing_error(tmp)
    test_ifd_growing_file(tmp)
    test_http(tmp, base, 'progressive', 'a.mp4', '', None)
    test_http(tmp, base, 'hls', 'hls/index.m3u8', 'hls/', '.ts')
    test_http(tmp, base, 'dash', 'dash/manifest.mpd', 'dash/', '.m4s')
    test_dash_variants(tmp, base)
    test_dash_multi(tmp, base)
    test_mpd_in_query(tmp, base)
    server.shutdown()
    print('\n%d failed check(s)' % len(FAILED))
    for name in FAILED:
        print('  - ' + name)
    sys.exit(1 if FAILED else 0)


if __name__ == '__main__':
    main()
