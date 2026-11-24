#!/usr/bin/env python3
"""End-to-end test of the Linux build against a fake GameVault server.

usage: python3 tests/integration.py [path/to/build/gamevault]
Starts a small GameVault stand-in (with HTTP range support), runs the service
with a fake console file system, and checks downloads, resume, PS4 installs,
deletes and path safety.
"""
import hashlib
import http.server
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else 'build/gamevault')
WORK = tempfile.mkdtemp(prefix='gv-test-')
ROOT = os.path.join(WORK, 'console')
FILES = os.path.join(WORK, 'nas')
os.makedirs(os.path.join(ROOT, 'data'), exist_ok=True)
os.makedirs(os.path.join(ROOT, 'user', 'app'), exist_ok=True)
os.makedirs(FILES, exist_ok=True)
# An old 0.2 tile folder, which GameVault should remove.
os.makedirs(os.path.join(ROOT, 'data/homebrew/PPSA99687/sce_sys'), exist_ok=True)
open(os.path.join(ROOT, 'data/homebrew/PPSA99687/.gv-app-id'), 'w').write('old')


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


# ---- fake NAS content -----------------------------------------------------------------
def exfat_image(path, size):
    with open(path, 'wb') as f:
        boot = bytearray(512)
        boot[0:3] = b'\xeb\x76\x90'
        boot[3:11] = b'EXFAT   '
        f.write(boot)
        block = os.urandom(1 << 20)
        written = 512
        while written < size:
            n = min(len(block), size - written)
            f.write(block[:n])
            written += n


IMAGE = 'PPSA12345-Test Game (v01.000.002).exfat'
exfat_image(os.path.join(FILES, IMAGE), 70 * 1024 * 1024 + 123)  # three 32 MB chunks
with open(os.path.join(FILES, 'bad.exfat'), 'wb') as f:
    f.write(os.urandom(4 * 1024 * 1024))
os.makedirs(os.path.join(FILES, 'folder', 'sce_sys'), exist_ok=True)
with open(os.path.join(FILES, 'folder', 'sce_sys', 'param.json'), 'w') as f:
    json.dump({'titleId': 'PPSA54321', 'contentVersion': '01.000.000',
               'localizedParameters': {'en-US': {'titleName': 'Folder Game'}}}, f)
with open(os.path.join(FILES, 'folder', 'eboot.bin'), 'wb') as f:
    f.write(os.urandom(3 * 1024 * 1024))
with open(os.path.join(FILES, 'game.pkg'), 'wb') as f:
    f.write(os.urandom(1024))

GAMES = {
    1: {'game': {'id': 1, 'title': 'Test Game', 'platform': 'PS5', 'titleId': 'PPSA12345', 'version': '01.000.002', 'path': '/games/PS5/' + IMAGE},
        'files': {'kind': 'ps5-exfat', 'files': [{'path': IMAGE, 'size': os.path.getsize(os.path.join(FILES, IMAGE)), 'disk': IMAGE}]}},
    2: {'game': {'id': 2, 'title': 'Folder Game', 'platform': 'PS5', 'titleId': 'PPSA54321', 'version': '01.000.000', 'path': '/games/PS5/Folder Game'},
        'files': {'kind': 'ps5-folder', 'files': [
            {'path': 'sce_sys/param.json', 'size': os.path.getsize(os.path.join(FILES, 'folder/sce_sys/param.json')), 'disk': 'folder/sce_sys/param.json'},
            {'path': 'eboot.bin', 'size': os.path.getsize(os.path.join(FILES, 'folder/eboot.bin')), 'disk': 'folder/eboot.bin'}]}},
    3: {'game': {'id': 3, 'title': 'Hades', 'platform': 'PS4', 'titleId': 'CUSA00002', 'version': '01.10', 'contentId': 'EP9000-CUSA00002_00-0000000000000000', 'path': '/games/PS4/Hades'},
        'files': {'kind': 'ps4-pkg', 'files': [{'path': 'Hades.pkg', 'size': 1024, 'category': 'gd', 'disk': 'game.pkg'}]}},
    4: {'game': {'id': 4, 'title': 'Evil', 'platform': 'PS5', 'titleId': 'PPSA66666', 'version': '1', 'path': '/games/PS5/Evil'},
        'files': {'kind': 'ps5-folder', 'files': [{'path': '../../escape.txt', 'size': 10, 'disk': 'game.pkg'}]}},
    5: {'game': {'id': 5, 'title': 'Broken', 'platform': 'PS5', 'titleId': 'PPSA77777', 'version': '1', 'path': '/games/PS5/x'},
        'files': {'kind': 'ps5-exfat', 'files': [{'path': 'PPSA77777-Broken.exfat', 'size': 4 * 1024 * 1024, 'disk': 'bad.exfat'}]}},
}
SLOW = {'on': False}


class Vault(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *a):
        pass

    def send_json(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parts = self.path.split('?')[0].strip('/').split('/')
        if parts == ['api', 'v1', 'health']:
            return self.send_json({'ok': True, 'version': 'fake', 'games': len(GAMES)})
        if len(parts) >= 4 and parts[:3] == ['api', 'v1', 'games'] and int(parts[3]) in GAMES:
            g = GAMES[int(parts[3])]
            if len(parts) == 4:
                return self.send_json(g['game'])
            if len(parts) == 5 and parts[4] == 'files':
                return self.send_json({**g['files'], 'files': [{k: v for k, v in f.items() if k != 'disk'} for f in g['files']['files']]})
            if parts[4] == 'files':
                want = urllib.request.unquote('/'.join(parts[5:]))
                entry = next((f for f in g['files']['files'] if f['path'] == want), None)
                if entry:
                    return self.send_file(os.path.join(FILES, entry['disk']))
        self.send_json({'error': 'not found'}, 404)

    def send_file(self, path):
        size = os.path.getsize(path)
        start, end = 0, size - 1
        rng = self.headers.get('Range')
        if rng:
            a, b = rng.replace('bytes=', '').split('-')
            start = int(a)
            end = int(b) if b else size - 1
            self.send_response(206)
            self.send_header('Content-Range', f'bytes {start}-{end}/{size}')
        else:
            self.send_response(200)
        self.send_header('Content-Length', str(end - start + 1))
        self.send_header('Accept-Ranges', 'bytes')
        self.end_headers()
        with open(path, 'rb') as f:
            f.seek(start)
            left = end - start + 1
            while left:
                chunk = f.read(min(left, 256 * 1024))
                try:
                    self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    return
                left -= len(chunk)
                if SLOW['on']:
                    time.sleep(0.02)


vault_port = free_port()
vault = http.server.ThreadingHTTPServer(('127.0.0.1', vault_port), Vault)
threading.Thread(target=vault.serve_forever, daemon=True).start()

port = free_port()
env = {**os.environ, 'GV_ROOT': ROOT, 'GV_PORT': str(port), 'GV_NO_BROWSER': '1'}
proc = None


def start():
    global proc
    proc = subprocess.Popen([BIN], env=env, stdout=open(os.path.join(WORK, 'log.txt'), 'a'), stderr=subprocess.STDOUT)
    for _ in range(50):
        try:
            call('GET', '/api/state')
            return
        except Exception:
            time.sleep(0.1)
    raise SystemExit('service did not start')


def stop():
    proc.terminate()
    proc.wait(5)


def call(method, path, body=None, timeout=10):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(f'http://127.0.0.1:{port}{path}', data=data, method=method,
                                 headers={'Content-Type': 'application/json'} if data else {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read() or b'null')
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b'null')


def job(jid):
    return next(j for j in call('GET', '/api/state')[1]['downloads'] if j['id'] == jid)


def wait(jid, states, timeout=60):
    t = time.time()
    while time.time() - t < timeout:
        j = job(jid)
        if j['state'] in states:
            return j
        time.sleep(0.2)
    raise AssertionError(f'{jid} stuck in {job(jid)["state"]}')


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


passed = 0


def check(name, cond, detail=''):
    global passed
    if not cond:
        print(f'FAIL {name} {detail}')
        print(open(os.path.join(WORK, 'log.txt')).read()[-3000:])
        stop()
        sys.exit(1)
    passed += 1
    print(f'ok   {name}')


try:
    start()
    s, st = call('PUT', '/api/settings', {'server': f'http://127.0.0.1:{vault_port}/', 'connections': 3})
    check('settings saved, trailing slash trimmed', s == 200 and st['server'] == f'http://127.0.0.1:{vault_port}')
    s, st = call('PUT', '/api/settings', {'ps5Drive': 'nope'})
    check('bad drive refused', s == 400)
    s, t = call('POST', '/api/test-connection', {})
    check('test connection', t['ok'] and t['games'] == len(GAMES))

    # Full image download, several connections.
    s, r = call('POST', '/api/downloads', {'gameId': 1})
    check('queue image', s == 201)
    jid = r['id']
    s, r2 = call('POST', '/api/downloads', {'gameId': 1})
    check('duplicate refused', s == 409)
    j = wait(jid, ['done', 'failed'])
    dest = os.path.join(ROOT, 'data/homebrew', IMAGE)
    check('image downloaded', j['state'] == 'done', j.get('error'))
    check('image identical', sha(dest) == sha(os.path.join(FILES, IMAGE)))
    state = call('GET', '/api/state')[1]
    check('image listed as installed', any(i['titleId'] == 'PPSA12345' and i['version'] == '01.000.002' for i in state['installed']))

    # Pause, restart the service, resume.
    s, _ = call('DELETE', '/api/installed?path=' + urllib.request.quote('/data/homebrew/' + IMAGE))
    check('delete installed', s == 200 and not os.path.exists(dest))
    SLOW['on'] = True
    jid = call('POST', '/api/downloads', {'gameId': 1})[1]['id']
    time.sleep(1.5)
    call('POST', f'/api/downloads/{jid}/pause', {})
    j = wait(jid, ['paused'])
    check('paused part kept', os.path.exists(dest + '.part') and os.path.exists(dest + '.part.state'))
    stop()
    SLOW['on'] = False
    start()
    check('still paused after restart', job(jid)['state'] == 'paused')
    call('POST', f'/api/downloads/{jid}/resume', {})
    j = wait(jid, ['done', 'failed'])
    check('resumed download complete', j['state'] == 'done' and sha(dest) == sha(os.path.join(FILES, IMAGE)), j.get('error'))
    check('resume files cleaned up', not os.path.exists(dest + '.part') and not os.path.exists(dest + '.part.state'))

    # Cancel removes partial data.
    os.remove(dest)
    call('POST', '/api/rescan', {})
    SLOW['on'] = True
    jid = call('POST', '/api/downloads', {'gameId': 1})[1]['id']
    time.sleep(1)
    call('POST', f'/api/downloads/{jid}/cancel', {})
    j = wait(jid, ['cancelled'])
    SLOW['on'] = False
    check('cancel deletes partial data', not os.path.exists(dest + '.part') and not os.path.exists(dest + '.part.state'))
    s, _ = call('DELETE', f'/api/downloads/{jid}')
    check('remove cancelled job', s == 200 and all(d['id'] != jid for d in call('GET', '/api/state')[1]['downloads']))

    # Folder dump.
    jid = call('POST', '/api/downloads', {'gameId': 2})[1]['id']
    j = wait(jid, ['done', 'failed'])
    check('folder downloaded', j['state'] == 'done' and sha(os.path.join(ROOT, 'data/homebrew/Folder Game/eboot.bin')) == sha(os.path.join(FILES, 'folder/eboot.bin')))

    # PS4 install hands the URL to the installer.
    jid = call('POST', '/api/downloads', {'gameId': 3})[1]['id']
    j = wait(jid, ['done', 'failed'])
    check('PS4 sent to installer', j['state'] == 'done' and os.path.isdir(os.path.join(ROOT, 'user/app/CUSA00002')))

    # Safety: a path escaping the game folder, and deleting outside the scan.
    s, r = call('POST', '/api/downloads', {'gameId': 4})
    check('unsafe path refused', s == 409 and 'unsafe' in r['error'])
    s, _ = call('DELETE', '/api/installed?path=/etc/passwd')
    check('delete outside games refused', s == 409)

    # A file that isn't an exFAT image fails the check.
    jid = call('POST', '/api/downloads', {'gameId': 5})[1]['id']
    j = wait(jid, ['done', 'failed'])
    check('bad image rejected', j['state'] == 'failed' and 'exFAT' in j['error'])

    # Missing drive.
    s, r = call('POST', '/api/downloads', {'gameId': 1, 'drive': 'usb3'})
    check('missing drive refused', s == 409)

    # Drives: an empty /mnt/usb0 folder is not a connected drive.
    os.makedirs(os.path.join(ROOT, 'mnt/usb0'), exist_ok=True)
    state = call('GET', '/api/state')[1]
    check('unplugged USB folder not shown', [d['id'] for d in state['drives']] == ['internal'])
    open(os.path.join(ROOT, 'mnt/usb0/.mounted'), 'w').close()
    state = call('GET', '/api/state')[1]
    check('connected USB drive shown', 'usb0' in [d['id'] for d in state['drives']])

    # The old tile's folder (PPSA99687) is removed: it is checked at start-up,
    # so it was planted before this run began (see the top of the file).
    check('old tile folder removed', not os.path.exists(os.path.join(ROOT, 'data/homebrew/PPSA99687')))

    # Home-screen app folder, kept out of the games list.
    tile = os.path.join(ROOT, 'data/homebrew/PPSA99688')
    for _ in range(50):
        if os.path.exists(os.path.join(tile, 'sce_sys/param.json')):
            break
        time.sleep(0.1)
    check('tile folder written', all(os.path.exists(os.path.join(tile, f)) for f in ('eboot.bin', 'sce_sys/param.json', 'sce_sys/icon0.png')))
    call('POST', '/api/rescan', {})
    state = call('GET', '/api/state')[1]
    check('tile not listed as a game', all(i['titleId'] != 'PPSA99688' for i in state['installed']) and state['tile']['installed'])
    s, _ = call('POST', '/api/open', {})
    check('open request accepted', s == 200)
    s, r = call('GET', '/api/resolve?host=localhost')
    check('resolve a name for the app', s == 200 and r['ip'] == '127.0.0.1', r)
    s, r = call('GET', '/api/resolve?host=192.168.0.9')
    check('resolve an address as itself', s == 200 and r['ip'] == '192.168.0.9', r)

    # Speed test (network only).
    s, r = call('POST', '/api/speed-test', {'gameId': 1}, timeout=60)
    check('speed test', s == 200 and r['best'] in (1, 2, 4, 6) and len(r['results']) == 4, r)
    check('speed test remembered', call('GET', '/api/settings')[1]['measuredConnections'] == r['best'])

    # Static interface.
    with urllib.request.urlopen(f'http://127.0.0.1:{port}/') as r:
        check('interface served', r.status == 200 and 'text/html' in r.headers['Content-Type'])

    print(f'\n{passed} checks passed')
finally:
    if proc and proc.poll() is None:
        stop()
    vault.shutdown()
    shutil.rmtree(WORK, ignore_errors=True)
