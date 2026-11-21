#!/usr/bin/env python3
"""Render tests on the console: prepare the test folder, then score what the
console wrote (docs-local/render_tests_study.md, platform/xbox360/xb_rtest.h).

  python tools/rt_console.py prepare CORPUS OUT [--only NAME ...] [--scale 0]
      OUT gets default.xex, harissa64v2.ini (rendertest=game:\\cases) and
      cases\\<name>\\scene.rdp for each scene of CORPUS (tools/rt_states.py's
      folder). Send OUT to its own console folder (never the user's install),
      launch default.xex; it returns to the dashboard when done.

  python tools/rt_console.py collect CORPUS BACK [--only NAME ...] [--png] [--baseline SUMMARY]
      BACK holds cases\\<name>\\xenos_f<N>.img fetched from the console. For each
      case: the references are replayed (h64test --rt-play, rdp-oracle), the
      console's images become xenos_f<N>.rdram (+ .cover: the ranges Xenos read
      back), tools/rt_compare.py scores every image and tools/rt_report.py sums
      up into BACK\\summary.json.
"""
import argparse
import glob
import json
import os
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
H64TEST = os.path.join(ROOT, 'build-msvc', 'h64test.exe')
XEX = os.path.join(ROOT, 'platform', 'xbox360', 'Release', 'harissa64v2.xex')
ORACLE = r'E:\IA-labs\Xenon64\build-oracle\rdp-oracle.exe'
RDRAM = 8 << 20


def scenes(corpus, only):
    out = []
    for d in sorted(os.listdir(corpus)):
        p = os.path.join(corpus, d, 'scene.rdp')
        if os.path.isfile(p) and (not only or d in only):
            out.append((d, p))
    return out


def prepare(a):
    os.makedirs(os.path.join(a.out, 'cases'), exist_ok=True)
    shutil.copyfile(XEX, os.path.join(a.out, 'default.xex'))
    with open(os.path.join(a.out, 'harissa64v2.ini'), 'w', newline='\r\n') as f:
        f.write('uilang=en\nupdatecheck=0\nrendertest=game:\\cases\nrendertestscale=%d\n' % a.scale)
    n = 0
    for name, p in scenes(a.corpus, a.only):
        d = os.path.join(a.out, 'cases', name)
        os.makedirs(d, exist_ok=True)
        shutil.copyfile(p, os.path.join(d, 'scene.rdp'))
        n += 1
    print('%d cases in %s' % (n, a.out))


def img_to_rdram(img, out_rdram, out_cover):
    ram = bytearray(RDRAM)
    cover = []
    data = open(img, 'rb').read()
    pos = 0
    while pos + 8 <= len(data):
        addr, size = struct.unpack_from('>II', data, pos)
        pos += 8
        ram[addr:addr + size] = data[pos:pos + size]
        cover.append([addr, size])
        pos += size
    open(out_rdram, 'wb').write(ram)
    json.dump(cover, open(out_cover, 'w'))


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    return r.returncode, r.stdout + r.stderr


def collect(a):
    for name, p in scenes(a.corpus, a.only):
        d = os.path.join(a.back, 'cases', name)
        if not os.path.isdir(d):
            continue   # not sent to the console
        imgs = glob.glob(os.path.join(d, 'xenos_f*.img'))
        if not imgs:
            print('%-16s no images from the console' % name)
            continue
        for img in imgs:
            base = img[:-4]
            img_to_rdram(img, base + '.rdram', base + '.cover')
        rc1, l1 = run([H64TEST, '--rt-play', p, d, '--frames', 'all'])
        rc2, l2 = run([ORACLE, p, d, '--frames', 'all'])
        if rc1 or rc2:
            print('%-16s replay failed %s' % (name, (l1 + l2).strip().splitlines()[-2:]))
            continue
        cmp = [sys.executable, os.path.join(ROOT, 'tools', 'rt_compare.py'), p, d]
        if a.png:
            cmp.append('--png')
        rc3, l3 = run(cmp)
        print('== ' + name)
        print(l3.rstrip())
        if not a.keep:
            for f in glob.glob(os.path.join(d, '*.rdram')) + glob.glob(os.path.join(d, '*.hidden')):
                os.remove(f)
    rep = [sys.executable, os.path.join(ROOT, 'tools', 'rt_report.py'), os.path.join(a.back, 'cases')]
    if a.baseline:
        rep += ['--baseline', a.baseline]
    return subprocess.run(rep).returncode


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('prepare')
    p.add_argument('corpus')
    p.add_argument('out')
    p.add_argument('--only', action='append', default=[])
    p.add_argument('--scale', type=int, default=0, help='0: automatic (from the widest image)')
    c = sub.add_parser('collect')
    c.add_argument('corpus')
    c.add_argument('back')
    c.add_argument('--only', action='append', default=[])
    c.add_argument('--png', action='store_true')
    c.add_argument('--keep', action='store_true')
    c.add_argument('--baseline')
    a = ap.parse_args()
    if a.cmd == 'prepare':
        prepare(a)
        return 0
    return collect(a)


if __name__ == '__main__':
    sys.exit(main())
