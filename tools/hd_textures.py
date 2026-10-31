#!/usr/bin/env python3
"""Hi-res texture packs for Harissa64, on a PC.

The console can download and convert the packs of the catalogue itself (RT in
the ROM list). This script is for everything else: packs hosted where the
console cannot download (Mega, Google Drive...), archives it cannot open
(.7z, .rar: extract them first), packs over 2 GB, or a console without
internet. It converts a pack with h64hdconv (the same conversion the console
does) into one file to copy to the console:

    <Harissa64 folder>\\hd_textures\\<GAME NAME>.h64hd

Then turn on "HD textures" in Settings > Graphics.

Usage:
  python hd_textures.py list
      the packs of the catalogue
  python hd_textures.py get PACK_ID [--out DIR]
      downloads a pack of the catalogue (resumed if stopped), checks it, converts it
  python hd_textures.py convert PACK ROM_NAME [--out DIR]
      converts a pack you have: its .zip, or its extracted folder. ROM_NAME is
      the name its PNG files start with ("SUPER MARIO 64#...png" -> "SUPER MARIO 64").

Options:
  --out DIR        where hd_textures\\ is made (default: the current folder)
  --catalog FILE   a catalogue file or https:// address (default: the one on GitHub)
  --conv PATH      h64hdconv(.exe) (default: next to this script, then the build folders)
  --max N          textures larger than N pixels are halved (default 1024)

Only Python 3 is needed. The packs stay on their authors' pages: nothing is
uploaded, and Harissa64 hosts no pack.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request

CATALOG = 'https://raw.githubusercontent.com/kernel64/Harissa64/master/docs/texture-packs.json'
UA = 'Harissa64 texture script (https://github.com/kernel64/Harissa64)'
HERE = os.path.dirname(os.path.abspath(__file__))


def file_name(rom):
    """The file name the console looks for: the cartridge name the way save
    folders are named (core/cart/h64_save.cpp h64_save_folder_name, without
    the game code): letters, digits, '-' and '_' kept, anything else one space."""
    out, space = '', False
    for c in rom:
        if c.isascii() and (c.isalnum() or c in '-_'):
            if space and out:
                out += ' '
            out += c
            space = False
        else:
            space = True
        if len(out) >= 36:
            break
    return out[:36] or 'pack'


def read_catalog(where):
    if where.startswith('https://'):
        req = urllib.request.Request(where, headers={'User-Agent': UA})
        with urllib.request.urlopen(req, timeout=60) as r:
            data = r.read()
    else:
        with open(where, 'rb') as f:
            data = f.read()
    cat = json.loads(data.decode('utf-8'))
    if cat.get('format') != '1':
        sys.exit('not a texture pack catalogue: %s' % where)
    return {k: v for k, v in cat.items() if isinstance(v, dict)}


def find_conv(given):
    names = ['h64hdconv.exe', 'h64hdconv']
    cands = [given] if given else []
    for d in (HERE, os.path.join(HERE, '..', 'build-msvc'), os.path.join(HERE, '..', 'build-gcc')):
        cands += [os.path.join(d, n) for n in names]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    found = shutil.which('h64hdconv')
    if found:
        return found
    sys.exit('h64hdconv not found: give its path with --conv (it comes with Harissa64, or build it with CMake)')


def download(url, path, size):
    have = os.path.getsize(path) if os.path.exists(path) else 0
    if size and have > size:
        os.remove(path)
        have = 0
    if size and have == size:
        return
    headers = {'User-Agent': UA}
    if have:
        headers['Range'] = 'bytes=%d-' % have
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=60) as r:
        mode = 'ab' if have and r.status == 206 else 'wb'
        done = have if mode == 'ab' else 0
        with open(path, mode) as f:
            while True:
                block = r.read(1 << 20)
                if not block:
                    break
                f.write(block)
                done += len(block)
                if size:
                    print('\r  %d / %d MB' % (done >> 20, size >> 20), end='', flush=True)
    print()


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def convert(conv, pack, rom, out_dir, max_side):
    dest = os.path.join(out_dir, 'hd_textures')
    os.makedirs(dest, exist_ok=True)
    target = os.path.join(dest, file_name(rom) + '.h64hd')
    r = subprocess.run([conv, pack, rom, target, '--max', str(max_side)])
    if r.returncode:
        sys.exit('conversion failed')
    print('copy %s to the console: <Harissa64 folder>\\hd_textures\\' % target)


def main():
    ap = argparse.ArgumentParser(description='Hi-res texture packs for Harissa64 (see the top of this file).')
    ap.add_argument('command', choices=['list', 'get', 'convert'])
    ap.add_argument('args', nargs='*')
    ap.add_argument('--out', default='.')
    ap.add_argument('--catalog', default=CATALOG)
    ap.add_argument('--conv')
    ap.add_argument('--max', type=int, default=1024)
    a = ap.parse_args()

    if a.command == 'list':
        for k, p in read_catalog(a.catalog).items():
            print('%-16s %-20s %-8s %6d MB  by %s' % (k, p.get('name', ''), p.get('version', ''), int(p.get('size', '0')) >> 20,
                                                     p.get('author', '')))
            print('%16s games %s, page %s' % ('', p.get('games', ''), p.get('page', '')))
        return
    if a.command == 'get':
        if len(a.args) != 1:
            sys.exit('usage: hd_textures.py get PACK_ID')
        cat = read_catalog(a.catalog)
        p = cat.get(a.args[0])
        if not p:
            sys.exit('no pack "%s" in the catalogue (see: hd_textures.py list)' % a.args[0])
        conv = find_conv(a.conv)
        size = int(p.get('size', '0'))
        zip_path = os.path.join(a.out, re.sub(r'[^A-Za-z0-9._-]', '_', p['url'].rsplit('/', 1)[-1]))
        print('%s %s by %s (%s)' % (p['name'], p.get('version', ''), p.get('author', ''), p.get('page', '')))
        download(p['url'], zip_path, size)
        if size and os.path.getsize(zip_path) != size:
            sys.exit('the download is incomplete: run the same command again to go on')
        if p.get('sha256') and sha256(zip_path) != p['sha256'].lower():
            os.remove(zip_path)
            sys.exit('the download is damaged (SHA-256): run the same command again')
        convert(conv, zip_path, p['rom'], a.out, a.max)
        return
    if len(a.args) != 2:
        sys.exit('usage: hd_textures.py convert PACK ROM_NAME')
    convert(find_conv(a.conv), a.args[0], a.args[1], a.out, a.max)


if __name__ == '__main__':
    main()
