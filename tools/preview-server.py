#!/usr/bin/env python3
"""A stand-in GameVault server for rendering the PS5 app's screens on a PC.

usage: python3 tools/preview-server.py PORT
Serves a few dozen made-up games with generated covers, key art and
screenshots (plain colours and shapes, no real artwork), so
`tools/build-app.sh snapshots` has something to draw.
"""
import http.server
import io
import json
import random
import sys

from PIL import Image, ImageDraw

PORT = int(sys.argv[1])
NAMES = ['Aurora Drift', 'Brass Lantern', 'Cinder Peak', 'Delta Signal', 'Echo Harbour', 'Fable Engine',
         'Granite Sky', 'Hollow Tide', 'Iron Meadow', 'Juniper Run', 'Kestrel Point', 'Lumen Vale',
         'Marrow Coast', 'Nimbus Yard', 'Orchid Line', 'Pale Comet', 'Quartz Hollow', 'Rook and Ruin',
         'Saffron Gate', 'Tundra Bloom', 'Umbra Works', 'Velvet Orbit']
GAMES = []
for i, name in enumerate(NAMES, 1):
    GAMES.append({
        'id': i, 'title': name, 'platform': 'PS4' if i % 5 == 0 else 'PS5',
        'titleId': f'PPSA{10000 + i}', 'version': '01.002.000', 'cover': f'/media/cover-{i}.png',
        'sizeBytes': (i * 7919 % 90 + 4) * 1_000_000_000, 'addedAt': f'2026-11-{i % 28 + 1:02d}',
        'releaseDate': f'20{18 + i % 8}-0{i % 9 + 1}-1{i % 9}', 'requiredFirmware': None,
        'favourite': i in (2, 5), 'year': 2018 + i % 8,
    })


def colour(seed, k=0):
    r = random.Random(seed * 31 + k)
    return tuple(r.randint(30, 200) for _ in range(3))


def png(w, h, seed, kind):
    img = Image.new('RGB', (w, h), colour(seed))
    d = ImageDraw.Draw(img)
    r = random.Random(seed)
    for k in range(9):
        x, y = r.randint(0, w), r.randint(0, h)
        s = r.randint(w // 10, w // 3)
        d.ellipse([x - s, y - s, x + s, y + s], fill=colour(seed, k + 1))
    d.rectangle([0, int(h * 0.82), w, h], fill=colour(seed, 20))
    out = io.BytesIO()
    img.save(out, 'PNG')
    return out.getvalue()


def detail(g):
    i = g['id']
    return {**g, 'summary': 'A made-up game for the PC preview. It has a summary long enough to wrap onto a second '
                            'line, so the page can be checked with a realistic amount of text in it.',
            'developer': 'Preview Studio', 'publisher': 'Stand-in Games', 'genres': ['Action', 'Adventure'],
            'modes': ['Single player'], 'criticScore': 70 + i % 25, 'userScore': 60 + i % 30,
            'backdrop': f'/media/backdrop-{i}.png', 'kind': 'ps4-pkg' if g['platform'] == 'PS4' else 'ps5-exfat',
            'fileCount': 1, 'screenshots': [f'/media/shot-{i}-{k}.png' for k in range(5)],
            'videos': [{'id': 'preview', 'name': 'Launch trailer'}]}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, body, kind='application/json'):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        p = self.path.split('?')[0]
        if p == '/api/v1/health':
            return self.send({'ok': True})
        if p == '/api/v1/games':
            return self.send({'games': GAMES})
        if p.startswith('/api/v1/games/'):
            parts = p.split('/')
            g = next((x for x in GAMES if str(x['id']) == parts[4]), None)
            if not g:
                self.send_response(404)
                self.end_headers()
                return
            if len(parts) > 5 and parts[5] == 'files':
                return self.send({'kind': detail(g)['kind'], 'files': []})
            return self.send(detail(g))
        if p.startswith('/media/'):
            name = p[7:-4]
            bits = name.split('-')
            seed = int(bits[1]) * 10 + (int(bits[2]) if len(bits) > 2 else 0)
            size = {'cover': (264, 352), 'backdrop': (1920, 1080), 'shot': (1280, 720)}[bits[0]]
            return self.send(png(*size, seed, bits[0]), 'image/png')
        self.send_response(404)
        self.end_headers()


http.server.ThreadingHTTPServer(('127.0.0.1', PORT), Handler).serve_forever()
