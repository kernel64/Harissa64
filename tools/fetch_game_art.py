#!/usr/bin/env python3
"""Download box art and game information for your N64 ROMs, on a PC.

Harissa64 downloads these itself when the console is online. This script is
for consoles without internet: run it on a PC, then copy the folder it fills
to the console, next to default.xex (Harissa64\\art\\games\\).

Usage:
  python fetch_game_art.py ROM_FOLDER [OUTPUT_FOLDER] [--no-descriptions] [--refresh]

  ROM_FOLDER     your .z64 / .n64 / .v64 / .zip files
  OUTPUT_FOLDER  where to write (default: ./art/games); it gets one KEY.png
                 (box art) and one KEY.txt (game information) per game, and
                 the databases in db/.

Only Python 3 is needed (no extra packages). Sources:
  - libretro-database (CC BY-SA 4.0): names, developers, publishers, years,
    genres, players.
  - GameDB-N64 (github.com/niemasd/GameDB-N64): titles, release dates.
  - libretro-thumbnails: box art pictures.
  - Wikipedia (CC BY-SA): a short description.
Nothing is uploaded; only your ROM headers are read (the first 64 bytes).

The matching follows core/cart/h64_gameinfo.cpp (keep both in step).
"""
import argparse
import json
import os
import re
import struct
import sys
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
import zipfile

UA = 'Harissa64 art script (https://github.com/kernel64/Harissa64)'
LIBRETRO = 'https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/'
N64 = 'Nintendo%20-%20Nintendo%2064.dat'
DB_FILES = [
    ('nointro.dat', LIBRETRO + 'no-intro/' + N64),
    ('developer.dat', LIBRETRO + 'developer/' + N64),
    ('publisher.dat', LIBRETRO + 'publisher/' + N64),
    ('releaseyear.dat', LIBRETRO + 'releaseyear/' + N64),
    ('genre.dat', LIBRETRO + 'genre/' + N64),
    ('maxusers.dat', LIBRETRO + 'maxusers/' + N64),
    ('gamedb-n64.json', 'https://github.com/niemasd/GameDB-N64/releases/latest/download/N64.data.json'),
]
THUMBS = 'https://raw.githubusercontent.com/libretro-thumbnails/Nintendo_-_Nintendo_64/master/Named_Boxarts/'
WIKI = 'https://en.wikipedia.org/api/rest_v1/page/summary/'
ROM_EXT = ('.z64', '.n64', '.v64', '.rom', '.u64')
REGIONS = {'E': 'USA', 'J': 'Japan', 'P': 'Europe', 'X': 'Europe', 'Y': 'Europe', 'Z': 'Europe', 'D': 'Germany',
           'F': 'France', 'I': 'Italy', 'S': 'Spain', 'U': 'Australia', 'A': 'Asia', 'B': 'Brazil', 'C': 'China',
           'K': 'Korea', 'H': 'Netherlands', 'W': 'Scandinavia', 'N': 'Canada', 'G': 'Gateway 64 (NTSC)',
           'L': 'Gateway 64 (PAL)'}
BAD_TAGS = ['Beta', 'Proto', 'Demo', 'Kiosk', 'LodgeNet', 'Debug', 'Pirate', 'Unl', 'Hack', 'Sample', 'Promo',
            'Virtual Console', 'Switch Online', 'Aftermarket', 'Wii', 'iQue', 'Competition', 'Alt', 'Program',
            'GameCube', 'Collection']
FIELDS = ['name', 'title', 'year', 'date', 'developer', 'publisher', 'genre', 'players', 'region', 'rating',
          'description']


def get(url, limit=8 << 20):
    """(status, bytes); status 0 when the server could not be reached."""
    req = urllib.request.Request(url, headers={'User-Agent': UA})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            data = r.read(limit + 1)
            return (r.status, data) if len(data) <= limit else (0, b'')
    except urllib.error.HTTPError as e:
        return e.code, b''
    except (urllib.error.URLError, OSError):
        return 0, b''


# ---- ROM headers ----
def to_z64(head):
    w = head[:4]
    if w == b'\x80\x37\x12\x40':
        return head
    if w == b'\x37\x80\x40\x12':   # v64: 16-bit byte-swapped
        return b''.join(head[i:i + 2][::-1] for i in range(0, len(head) - 1, 2))
    if w == b'\x40\x12\x37\x80':   # n64: 32-bit little-endian
        return b''.join(head[i:i + 4][::-1] for i in range(0, len(head) - 3, 4))
    return None


def rom_header(path):
    try:
        if path.lower().endswith('.zip'):
            with zipfile.ZipFile(path) as z:
                for info in z.infolist():
                    with z.open(info) as f:
                        head = f.read(64)
                    if info.filename.lower().endswith(ROM_EXT) or to_z64(head):
                        return to_z64(head)
            return None
        with open(path, 'rb') as f:
            return to_z64(f.read(64))
    except (OSError, zipfile.BadZipFile):
        return None


def rom_key(head):
    code = head[0x3B:0x3F].decode('latin-1')
    crc1 = struct.unpack('>I', head[0x10:0x14])[0]
    if re.fullmatch('[A-Z0-9]{4}', code):
        return code, code
    return 'X%08X' % crc1, code


# ---- Text ----
SPECIAL = {0x2018: "'", 0x2019: "'", 0x201A: "'", 0x2032: "'", 0x201C: '"', 0x201D: '"', 0x201E: '"', 0x2033: '"',
           0x2010: '-', 0x2011: '-', 0x2012: '-', 0x2013: '-', 0x2014: '-', 0x2212: '-', 0x2026: '...', 0xA0: ' ',
           0x2009: ' ', 0x200A: ' ', 0x202F: ' ', 0xD7: 'x', 0x152: 'OE', 0x153: 'oe', 0xC6: 'AE', 0xE6: 'ae',
           0xDF: 'ss', 0xD0: 'D', 0xF0: 'd', 0xD8: 'O', 0xF8: 'o', 0xDE: 'T', 0xFE: 't', 0xF7: '/', 0x110: 'D',
           0x111: 'd', 0x141: 'L', 0x142: 'l', 0x131: 'i'}


def fold(text):
    """ASCII the console's font can show (as h64_ascii_fold)."""
    out = []
    for ch in text:
        c = ord(ch)
        if c < 0x80:
            out.append(' ' if ch in '\r\n\t' else ch)
        elif c in SPECIAL:
            out.append(SPECIAL[c])
        elif 0xC0 <= c <= 0x17F:
            d = ''.join(x for x in unicodedata.normalize('NFKD', ch) if ord(x) < 128 and x.isalnum())
            out.append(d[:1])
    return ''.join(out)


def display_title(src, dash_to_colon):
    t = src.split(' (')[0]
    if dash_to_colon and ' - ' in t:
        t = t.replace(' - ', ': ', 1)
    head, sep, tail = t.partition(': ')
    for art in ('The', 'A', 'An'):
        if head.endswith(', ' + art):
            head = art + ' ' + head[:-len(art) - 2]
            break
    return fold(head + sep + tail)


# ---- Databases ----
TOKEN = re.compile(r'"([^"]*)"|(\()|(\))|([^\s()"]+)')


def parse_dat(text):
    """Games of a clrmamepro dat: (fields, rom crcs, rom serial)."""
    toks = [(m.group(1) if m.group(1) is not None else m.group(4), m.group(2) or m.group(3)) for m in TOKEN.finditer(text)]
    i, games = 0, []
    while i < len(toks):
        word = toks[i][0]
        i += 2   # name and "("
        if word != 'game':
            depth = 1
            while i < len(toks) and depth:
                depth += toks[i][1] == '(' and 1 or toks[i][1] == ')' and -1 or 0
                i += 1
            continue
        fields, crcs, serial = {}, [], ''
        while i < len(toks) and toks[i][1] != ')':
            key = toks[i][0]
            if toks[i + 1][1] == '(':
                i += 2
                while i < len(toks) and toks[i][1] != ')':
                    if toks[i + 1][1] == '(':   # nested blocks are not used
                        depth, i = 1, i + 2
                        while depth:
                            depth += toks[i][1] == '(' and 1 or toks[i][1] == ')' and -1 or 0
                            i += 1
                        continue
                    k, v = toks[i][0], toks[i + 1][0]
                    if key == 'rom' and k == 'crc':
                        crcs.append(int(v, 16))
                    elif key == 'rom' and k == 'serial':
                        serial = v
                    i += 2
                i += 1
            else:
                fields[key] = toks[i + 1][0]
                i += 2
        i += 1
        games.append((fields, crcs, serial))
    return games


class GameDb:
    def __init__(self):
        self.games = {}     # name -> {'name', 'region', 'serial', 'crcs'}
        self.order = []
        self.meta = {}      # crc -> fields
        self.gdb = {}       # code -> fields

    def add_dat(self, text):
        for fields, crcs, serial in parse_dat(text):
            if 'name' in fields:
                g = self.games.get(fields['name'])
                if not g:
                    g = {'name': fields['name'], 'region': fields.get('region', ''),
                         'serial': fields.get('serial', serial), 'crcs': []}
                    self.games[g['name']] = g
                    self.order.append(g)
                g['crcs'] += crcs
            else:
                for c in crcs:
                    m = self.meta.setdefault(c, {})
                    for k in ('comment', 'developer', 'publisher', 'releaseyear', 'genre', 'users'):
                        if k in fields and not m.get(k):
                            m[k] = fields[k]

    def add_gamedb(self, data):
        for serial, f in data.items():
            if len(serial) >= 8 and serial[3] == '-':
                d = self.gdb.setdefault(serial[4:8], {})
                for k in ('title', 'developer', 'publisher', 'release_date', 'rating'):
                    if f.get(k) and not d.get(k):
                        d[k] = f[k]


def name_score(name, version, region):
    score, rev = 100, 0
    for tag in re.findall(r'\(([^)]*)\)?', name):
        if tag.lower().startswith('rev '):
            r = tag[4:]
            rev = ord(r[0]) - ord('A') + 1 if r[:1].isupper() and r[:1].isalpha() else int(re.match(r'\d*', r).group() or 0)
        if any(b.lower() in tag.lower() for b in BAD_TAGS):
            score -= 50
    score += 20 if rev == version else -5 * abs(rev - version)
    if region and region.lower() in name.lower():
        score += 10
    return score * 1000 - len(name)


def base_key(name):
    return ''.join(c.lower() for c in re.split(r'[(\[]', name)[0] if c.isascii() and c.isalnum())


def lookup(db, code, version, stem):
    """(info dict, box art names to try) as h64_gamedb_lookup."""
    region = REGIONS.get(code[3], '') if len(code) == 4 else ''
    info = {'found': 0}
    cands = [g for g in db.order if g['serial'] == code] if len(code) == 4 else []
    if not cands and stem:
        k = base_key(stem)
        if len(k) >= 3:
            cands = [g for g in db.order if base_key(g['name']) == k]
    cands.sort(key=lambda g: -name_score(g['name'], version, region))
    covers = [g['name'] for g in cands]
    if cands:
        e = cands[0]
        info.update(found=1, name=e['name'], region=fold(e['region']))
        m = None
        for g in cands:
            for c in g['crcs']:
                if c in db.meta:
                    m = db.meta[c]
                    break
            if m:
                break
        if m:
            info.update(developer=fold(m.get('developer', '')), publisher=fold(m.get('publisher', '')),
                        year=fold(m.get('releaseyear', '')), genre=fold(m.get('genre', '')))
            u = int(m.get('users', '0') or 0)
            if u:
                info['players'] = '1-%d' % u if u > 1 else '1'
        info['title'] = display_title(m['comment'], False) if m and m.get('comment') else display_title(e['name'], True)
    g = db.gdb.get(code) if len(code) == 4 else None
    if g:
        info['found'] = 1
        if g.get('title'):
            info['title'] = display_title(g['title'], False)
        for k in ('developer', 'publisher'):
            if not info.get(k) and g.get(k):
                info[k] = fold(g[k])
        if len(g.get('release_date', '')) >= 4:
            info['date'] = g['release_date']
            info['year'] = g['release_date'][:4]   # this region's release: wins over libretro's year
        if g.get('rating'):
            info['rating'] = fold(g['rating'])
        if not info.get('region'):
            info['region'] = region
    return info, covers


# ---- Cache files (h64_gameinfo_format / parse) ----
def write_info(path, info):
    lines = ['# Harissa64 game information (sources: libretro-database CC BY-SA 4.0, GameDB-N64, Wikipedia CC BY-SA)',
             'found=%d' % (1 if info.get('found') else 0),
             'cover=' + {1: 'yes', 0: 'none'}.get(info.get('cover', -1), 'unknown')]
    if info.get('wiki', -1) >= 0:
        lines.append('wiki=' + ('yes' if info['wiki'] else 'none'))
    for k in FIELDS:
        v = str(info.get(k, '') or '').replace('\r', ' ').replace('\n', ' ')
        if v:
            lines.append('%s=%s' % (k, v))
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write('\r\n'.join(lines) + '\r\n')


def read_info(path):
    info = {}
    try:
        for line in open(path, encoding='utf-8', errors='replace'):
            if '=' in line and not line.startswith('#'):
                k, v = line.rstrip('\r\n').split('=', 1)
                info[k] = v
    except OSError:
        return None
    if 'found' not in info:
        return None
    info['found'] = int(info['found'] or 0)
    info['cover'] = {'yes': 1, 'none': 0}.get(info.get('cover'), -1)
    info['wiki'] = {'yes': 1, 'none': 0}.get(info.get('wiki'), -1)
    return info


def thumb_name(name):
    return ''.join('_' if c in '&*/:`<>?\\|"' else c for c in name)


def wiki_extract(data, limit=640):
    """A few whole sentences when the page is about a video game (h64_gameinfo_wiki_extract)."""
    try:
        d = json.loads(data)
    except ValueError:
        return ''
    desc = (d.get('description') or '').lower()
    if d.get('type') == 'disambiguation' or 'game' not in desc or 'series' in desc or 'franchise' in desc:
        return ''   # not a game's page ("Mario Party" is the series)
    text = fold(d.get('extract') or '')
    cut, pos, n = 0, 0, 0
    while n < 4:
        dot = text.find('. ', pos)
        end = dot + 1 if dot >= 0 else (len(text) if text.endswith('.') else -1)
        if end < 0 or end >= limit:
            break
        cut, n = end, n + 1
        if dot < 0:
            break
        pos = dot + 2
    return text[:cut]


def wiki_pages(info):
    t = info.get('title', '').replace(' ', '_')
    if not t:
        return []
    pages = [t, t + '_(video_game)']
    if info.get('year'):
        pages.append('%s_(%s_video_game)' % (t, info['year']))
    return [urllib.parse.quote(p, safe='-._~') for p in pages]


def main():
    ap = argparse.ArgumentParser(description='Box art and game information for Harissa64.')
    ap.add_argument('roms', help='folder with your ROMs')
    ap.add_argument('out', nargs='?', default=os.path.join('art', 'games'), help='output folder (default art/games)')
    ap.add_argument('--no-descriptions', action='store_true', help='skip the Wikipedia descriptions')
    ap.add_argument('--refresh', action='store_true', help='download the databases again and retry what was missing')
    a = ap.parse_args()
    os.makedirs(os.path.join(a.out, 'db'), exist_ok=True)

    db = GameDb()
    for name, url in DB_FILES:
        path = os.path.join(a.out, 'db', name)
        if a.refresh or not os.path.exists(path) or time.time() - os.path.getmtime(path) > 30 * 86400:
            st, data = get(url)
            if st == 200:
                open(path, 'wb').write(data)
                print('downloaded %s (%d KB)' % (name, len(data) // 1024))
            else:
                print('could not download %s (%s)' % (name, st or 'no connection'))
        if os.path.exists(path):
            raw = open(path, 'rb').read().decode('utf-8', errors='replace')
            if name.endswith('.json'):
                db.add_gamedb(json.loads(raw))
            else:
                db.add_dat(raw)
    if not db.order:
        sys.exit('No game list: check the internet connection.')

    files = sorted(f for f in os.listdir(a.roms) if f.lower().endswith(ROM_EXT + ('.zip',)))
    found = covers = 0
    for i, f in enumerate(files, 1):
        head = rom_header(os.path.join(a.roms, f))
        if not head:
            print('[%d/%d] %s: not an N64 ROM' % (i, len(files), f))
            continue
        key, code = rom_key(head)
        stem = os.path.splitext(f)[0]
        txt, png = os.path.join(a.out, key + '.txt'), os.path.join(a.out, key + '.png')
        info = read_info(txt)
        fresh, names = lookup(db, code, head[0x3F], stem)
        if not info or not info['found'] or a.refresh:
            if info and info.get('wiki') == 1:
                fresh['description'], fresh['wiki'] = info.get('description', ''), 1
            info = fresh
            info['cover'] = 1 if os.path.exists(png) else -1
            info.setdefault('wiki', -1)
        if info['found'] and not os.path.exists(png):
            for n in names or [info.get('name', '')]:
                st, data = get(THUMBS + urllib.parse.quote(thumb_name(n) + '.png', safe='-._~'))
                if st == 200 and len(data) < 256 and data.endswith(b'.png') and b'/' not in data:
                    # A git symbolic link (aliases in libretro-thumbnails): its text names the real file.
                    st, data = get(THUMBS + urllib.parse.quote(data.decode('utf-8', 'replace'), safe='-._~'))
                if st == 200 and data.startswith(b'\x89PNG'):
                    open(png, 'wb').write(data)
                    break
            info['cover'] = 1 if os.path.exists(png) else 0
        if info['found'] and info.get('wiki', -1) < 0 and not a.no_descriptions:
            info['wiki'] = 0
            for p in wiki_pages(info):
                st, data = get(WIKI + p, 256 << 10)
                if st == 200:
                    d = wiki_extract(data)
                    if d:
                        info['description'], info['wiki'] = d, 1
                        break
                elif st == 0:
                    info['wiki'] = -1
                    break
        write_info(txt, info)
        found += bool(info['found'])
        covers += os.path.exists(png)
        print('[%d/%d] %s -> %s: %s%s' % (i, len(files), f, key, info.get('title', 'not found'),
                                        '' if os.path.exists(png) else ' (no box art)'))
    print('\n%d ROMs, %d identified, %d with box art. Copy "%s" to the console as Harissa64\\art\\games.'
          % (len(files), found, covers, a.out))


if __name__ == '__main__':
    main()
