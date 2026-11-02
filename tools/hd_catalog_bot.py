#!/usr/bin/env python3
"""Finds HD texture packs on n64textures.com and adds them to Harissa64's
catalogue (docs/texture-packs.json).

For each pack the site links to, the bot:
  1. downloads it (kept in a cache folder, never extracted, never run);
  2. measures its size and SHA-256;
  3. looks inside the zip: refused if it holds a program or a script, or no
     picture named the GLideN64/Rice way (NAME#CRC#FMT#SIZE...png);
  4. converts it with h64hdconv, the same code the console runs, to prove it
     works (the result is thrown away);
  5. finds the game code from the name in the pictures (tools/hd_games.json:
     the cartridge names and codes, made by the `names` command);
  6. adds or updates its entry in the catalogue.

Entries added by hand are never touched: the bot only changes entries it
made itself ("source": "n64textures.com"). Packs the console cannot use
(.rar, .7z, over 2 GB, Jabo or HTS format) are listed in the report only.

Usage:
  python tools/hd_catalog_bot.py scan     [--only TEXT]   list the site's packs (no download)
  python tools/hd_catalog_bot.py update   [--only TEXT]   download, check, update the catalogue
  python tools/hd_catalog_bot.py names    --roms DIR      rebuild tools/hd_games.json from ROMs

Options:
  --catalog FILE   the catalogue (default docs/texture-packs.json)
  --cache DIR      where downloads are kept (default: %TEMP%/harissa64-hdbot)
  --conv PATH      h64hdconv(.exe) (default: build-msvc, build-release-tools)
  --report FILE    also write the report (Markdown) to FILE
  --keep           keep the downloads (default: each is deleted once checked)

Only Python 3 is needed. The site is read politely: one request at a time,
a pause between packs.
"""
import argparse
import hashlib
import html
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
import zipfile

SITE = 'https://www.n64textures.com'
PAGES = [SITE + '/downloads/']
SOURCE = 'n64textures.com'
UA = 'Harissa64 texture catalogue bot (https://github.com/kernel64/Harissa64)'
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
GAMES = os.path.join(HERE, 'hd_games.json')
MAX_SIZE = 2 * 1024 * 1024 * 1024 - 1          # the console reads packs under 2 GB
RISKY = ('.exe', '.dll', '.com', '.scr', '.bat', '.cmd', '.ps1', '.vbs', '.vbe', '.js', '.jse', '.wsf',
         '.msi', '.lnk', '.jar', '.hta', '.sh', '.py', '.pif', '.reg', '.cpl', '.sys', '.app', '.dmg', '.apk')
KEY = re.compile(r'^(.+?)#([0-9A-Fa-f]{8}|\$)#(\d)#(\d)')


# ---------------------------------------------------------------- web --
def get(url, dest=None, progress=False):
    req = urllib.request.Request(url, headers={'User-Agent': UA})
    with urllib.request.urlopen(req, timeout=60) as r:
        if dest is None:
            return r.read()
        done, last = 0, 0
        with open(dest + '.part', 'wb') as f:
            while True:
                block = r.read(1 << 20)
                if not block:
                    break
                f.write(block)
                done += len(block)
                if done > MAX_SIZE:
                    raise ValueError('over 2 GB')
                if progress and done - last >= 50 << 20:
                    last = done
                    print('    %d MB' % (done >> 20), flush=True)
        os.replace(dest + '.part', dest)
        return done


def text_of(fragment):
    t = re.sub(r'<[^>]+>', ' ', fragment)
    return re.sub(r'\s+', ' ', html.unescape(t)).strip()


def scan_page(url):
    """The packs of one downloads page: one per /download/ link, with the
    title, author and date found before it in the same entry."""
    page = get(url).decode('utf-8', 'replace')
    page = re.sub(r'<script.*?</script>', '', page, flags=re.S)
    packs = []
    links = list(re.finditer(r'<a([^>]+href="(/download/[^"]+)"[^>]*)>(.*?)</a>', page, re.S))
    prev = 0
    for m in links:
        chunk = page[prev:m.start()]
        prev = m.end()
        # The entry starts at its last anchor (the game's title).
        anchors = list(re.finditer(r'<a[^>]*\bname="([^"]+)"[^>]*>(.*?)</a>(.{0,200})', chunk, re.S))
        if not anchors:
            continue
        a = anchors[-1]
        body = text_of(chunk[a.start():])
        title = text_of(a.group(2) + ' ' + a.group(3).split('<')[0]) or html.unescape(a.group(1))
        author = re.search(r'Author\s*:?\s*(.+?)\s+File Size', body)
        date = re.search(r'Release Date\s*:?\s*(.+?)\s+Download', body)
        desc = re.search(r'Desc?ription\s*:?\s*(.+?)\s+Author', body)
        who = author.group(1).strip() if author else ''
        if not who or len(who) > 60:
            # Some entries swap their labels, or have none: the author in brackets in the title.
            who = desc.group(1).strip() if desc and len(desc.group(1)) <= 60 else ''
            if not who:
                br = re.findall(r'\(([^()]+)\)', title)
                who = br[-1].strip() if br else ''
        attr = re.search(r'title="([^"]*)"', m.group(1))
        packs.append({
            'title': re.sub(r'\s+', ' ', title).strip(),
            'anchor': html.unescape(a.group(1)),
            'author': who,
            'date': date.group(1).strip(' ,') if date else '',
            'about': desc.group(1).strip() if desc else '',
            'url': urllib.parse.urljoin(SITE, html.unescape(m.group(2))),
            'file': html.unescape(attr.group(1) if attr and attr.group(1) else m.group(2).rsplit('/', 1)[-1]),
            'page': url + '#' + urllib.parse.quote(html.unescape(a.group(1))),
        })
    return packs


# ---------------------------------------------------------------- checks --
def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def look_inside(path):
    """(rom name, pictures, problem): the name most pictures carry."""
    try:
        z = zipfile.ZipFile(path)
    except zipfile.BadZipFile:
        return None, 0, 'not a zip file'
    names = z.namelist()
    risky = [n for n in names if n.lower().endswith(RISKY)]
    if risky:
        return None, 0, 'holds programs or scripts (%s): refused' % ', '.join(risky[:3])
    counts = {}
    for n in names:
        base = n.replace('\\', '/').rsplit('/', 1)[-1]
        if not base.lower().endswith('.png'):
            continue
        m = KEY.match(base)
        if m:
            counts[m.group(1)] = counts.get(m.group(1), 0) + 1
    if not counts:
        hts = [n for n in names if n.lower().endswith(('.hts', '.htc'))]
        return None, 0, 'HTS/HTC file, not pictures' if hts else 'no picture with GLideN64/Rice names (Jabo format?)'
    rom = max(counts, key=counts.get)
    return rom, counts[rom], None


def find_conv(given):
    cands = [given] if given else []
    for d in ('build-release-tools', 'build-msvc', 'build-gcc'):
        cands += [os.path.join(REPO, d, n) for n in ('h64hdconv.exe', 'h64hdconv')]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    sys.exit('h64hdconv not found: build it (cmake --build build-msvc --target h64hdconv) or give --conv')


def convert_check(conv, path, rom):
    """Pictures converted by h64hdconv (the console's code), or an error."""
    out = os.path.join(tempfile.gettempdir(), 'harissa64-hdbot-check.h64hd')
    try:
        r = subprocess.run([conv, path, rom, out], capture_output=True, text=True, timeout=3600)
    finally:
        if os.path.exists(out):
            os.remove(out)
    m = re.search(r': (\d+) textures \((\d+) PNG names, (\d+) repeated, (\d+) unreadable\), ([\d.]+) MB', r.stdout)
    if r.returncode or not m:
        return None, (r.stdout + r.stderr).strip().splitlines()[-1:] or ['failed']
    return {'pictures': int(m.group(1)), 'unreadable': int(m.group(4)), 'mb': float(m.group(5))}, None


def load_games():
    if not os.path.exists(GAMES):
        return {}
    with open(GAMES, encoding='utf-8') as f:
        return json.load(f).get('names', {})


def codes_for(rom, games):
    key = rom.strip().upper()
    for name, codes in games.items():
        if name.strip().upper() == key:
            return codes
    return ''


# ---------------------------------------------------------------- catalogue --
def tidy(pack):
    '''The name without the author in brackets (the console shows "by ..."),
    and the date without stray spaces.'''
    name = re.sub(r'\(\s+', '(', re.sub(r'\s+\)', ')', pack['title']))
    who = pack['author'].strip().lower()
    br = re.search(r'\s*\(([^()]*)\)\s*$', name)
    if br and who and br.group(1).strip().lower().replace(' ', '') in (who.replace(' ', ''), who.replace(' ', '') + 's'):
        name = name[:br.start()]
    date = re.sub(r'\s+,', ',', pack['date']).strip()
    return name.strip(), date


def slug(text):
    s = re.sub(r'\.(zip|rar|7z)$', '', text, flags=re.I)
    s = re.sub(r'[^A-Za-z0-9]+', '-', s).strip('-').lower()
    # 36 characters at most: the console's file names (FATX: 42) end in .h64hd.
    return 'n64tex-' + s[:29].strip('-')


def load_catalog(path):
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def save_catalog(path, cat):
    text = json.dumps(cat, indent=2, ensure_ascii=False) + '\n'
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


# ---------------------------------------------------------------- commands --
def cmd_scan(a):
    packs = []
    for p in PAGES:
        packs += scan_page(p)
    for p in packs:
        if a.only and a.only.lower() not in (p['title'] + p['file']).lower():
            continue
        print('%-50s %-22s %s' % (p['title'][:50], p['author'][:22], p['file']))
    print('%d packs' % len(packs))
    return packs


def cmd_update(a):
    conv = find_conv(a.conv)
    games = load_games()
    cat = load_catalog(a.catalog)
    os.makedirs(a.cache, exist_ok=True)
    packs = []
    for page in PAGES:
        packs += scan_page(page)
    report = {'added': [], 'updated': [], 'same': [], 'skipped': [], 'gone': []}
    seen = set()
    for p in packs:
        if a.only and a.only.lower() not in (p['title'] + p['file']).lower():
            continue
        pid = slug(p['file'])
        seen.add(pid)
        label = '%s (%s)' % (p['title'], p['author'] or '?')
        if not p['file'].lower().endswith('.zip'):
            report['skipped'].append((label, 'a %s archive: the console reads only .zip (PC: extract it, then tools/hd_textures.py convert)'
                                       % p['file'].rsplit('.', 1)[-1]))
            continue
        old = cat.get(pid)
        if old is not None and old.get('source') != SOURCE:
            report['skipped'].append((label, 'id %s is an entry added by hand' % pid))
            continue
        local = os.path.join(a.cache, re.sub(r'[^A-Za-z0-9._-]', '_', p['file']))
        print('%s\n  %s' % (label, p['url']), flush=True)
        try:
            if not os.path.exists(local):
                get(p['url'], local, progress=True)
                time.sleep(2)
            size = os.path.getsize(local)
            digest = sha256(local)
            name, date = tidy(p)
            if old and old.get('sha256') == digest and old.get('url') == p['url']:
                # The same file: only the page's text may have changed.
                meta = {'name': name, 'author': p['author'] or 'unknown', 'version': date or SOURCE, 'page': p['page']}
                if any(old.get(k) != v for k, v in meta.items()):
                    old.update(meta)
                    save_catalog(a.catalog, cat)
                    report['updated'].append((label, 'name, author, date or page'))
                else:
                    report['same'].append((label, pid))
                continue
            rom, pictures, problem = look_inside(local)
            if problem:
                report['skipped'].append((label, problem))
                continue
            codes = codes_for(rom, games)
            if not codes:
                report['skipped'].append((label, 'pictures named "%s": no cartridge of that name in tools/hd_games.json' % rom))
                continue
            res, err = convert_check(conv, local, rom)
            if err:
                report['skipped'].append((label, 'h64hdconv: %s' % err[0]))
                continue
            if not res['pictures']:
                report['skipped'].append((label, 'h64hdconv converted no picture'))
                continue
            entry = {
                'name': name,
                'games': codes,
                'rom': rom,
                'author': p['author'] or 'unknown',
                'version': date or SOURCE,
                'page': p['page'],
                'url': p['url'],
                'size': str(size),
                'sha256': digest,
                'source': SOURCE,
            }
            note = '%s, %d pictures, %.0f MB on the console%s' % (codes, res['pictures'], res['mb'],
                                                                   ', %d unreadable' % res['unreadable'] if res['unreadable'] else '')
            report['updated' if old else 'added'].append((label, note))
            cat[pid] = entry
            save_catalog(a.catalog, cat)
        except Exception as e:   # one pack's failure must not stop the others
            report['skipped'].append((label, 'error: %s' % e))
        finally:
            if not a.keep and os.path.exists(local):
                os.remove(local)
    if not a.only:
        for pid in [k for k, v in cat.items() if isinstance(v, dict) and v.get('source') == SOURCE and k not in seen]:
            report['gone'].append((cat[pid].get('name', pid), 'no longer on the site: removed'))
            del cat[pid]
        save_catalog(a.catalog, cat)
    lines = ['# HD texture packs from %s (%s)' % (SOURCE, time.strftime('%Y-%m-%d')), '']
    for key, head in (('added', 'Added'), ('updated', 'Updated'), ('same', 'Unchanged'), ('skipped', 'Not added'), ('gone', 'Removed')):
        if report[key]:
            lines.append('## %s (%d)' % (head, len(report[key])))
            lines += ['- %s: %s' % item for item in report[key]]
            lines.append('')
    text = '\n'.join(lines)
    print(text)
    if a.report:
        with open(a.report, 'w', encoding='utf-8') as f:
            f.write(text)


def rom_header(data):
    """(name, 3-letter code) from a ROM's first 0x40 bytes, any byte order."""
    magic = data[:4]
    if magic == b'\x37\x80\x40\x12':
        data = bytes(b for i in range(0, 0x40, 2) for b in (data[i + 1], data[i]))
    elif magic == b'\x40\x12\x37\x80':
        data = bytes(b for i in range(0, 0x40, 4) for b in data[i:i + 4][::-1])
    elif magic != b'\x80\x37\x12\x40':
        return None
    name = data[0x20:0x34].split(b'\0')[0].decode('latin-1').strip()
    code = data[0x3B:0x3E].decode('latin-1')
    if not name or not re.match(r'^[A-Z0-9][A-Z0-9]{2}$', code):
        return None
    return name, code


def cmd_names(a):
    if not a.roms:
        sys.exit('names needs --roms DIR')
    names = {}
    for k, v in load_games().items():   # kept: codes added by hand, cartridges not in this folder
        names[k] = [c for c in v.split(',') if c]
    for fn in sorted(os.listdir(a.roms)):
        path = os.path.join(a.roms, fn)
        head = None
        try:
            if fn.lower().endswith('.zip'):
                with zipfile.ZipFile(path) as z:
                    for n in z.namelist():
                        if n.lower().endswith(('.z64', '.n64', '.v64')):
                            with z.open(n) as f:
                                head = f.read(0x40)
                            break
            elif fn.lower().endswith(('.z64', '.n64', '.v64')):
                with open(path, 'rb') as f:
                    head = f.read(0x40)
        except (OSError, zipfile.BadZipFile):
            continue
        h = rom_header(head) if head and len(head) == 0x40 else None
        if not h:
            continue
        codes = names.setdefault(h[0], [])
        if h[1] not in codes:
            codes.append(h[1])
    out = {'about': 'Cartridge names (as in the ROM header, the start of GLideN64/Rice picture names) and their '
                    'game codes, for tools/hd_catalog_bot.py. Made by its `names` command.',
           'names': {k: ','.join(sorted(v)) for k, v in sorted(names.items())}}
    with open(GAMES, 'w', encoding='utf-8', newline='\n') as f:
        f.write(json.dumps(out, indent=2, ensure_ascii=False) + '\n')
    print('%d cartridge names -> %s' % (len(names), GAMES))


def main():
    ap = argparse.ArgumentParser(description='Finds HD texture packs on n64textures.com for the catalogue (see the top of this file).')
    ap.add_argument('command', choices=['scan', 'update', 'names'])
    ap.add_argument('--only')
    ap.add_argument('--catalog', default=os.path.join(REPO, 'docs', 'texture-packs.json'))
    ap.add_argument('--cache', default=os.path.join(tempfile.gettempdir(), 'harissa64-hdbot'))
    ap.add_argument('--conv')
    ap.add_argument('--report')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--roms')
    a = ap.parse_args()
    {'scan': cmd_scan, 'update': cmd_update, 'names': cmd_names}[a.command](a)


if __name__ == '__main__':
    main()
