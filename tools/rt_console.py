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
import time

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


def run_console(a):
    """Uploads OUT over FTP (files whose size differs), launches default.xex with the XDK's
    xbreboot, waits for the return to the dashboard, then fetches every xenos_f*.img into BACK.
    The caller holds the console lock (xbox_lock) and the debugger is detached."""
    sys.path.insert(0, os.path.join(ROOT, 'tests', 'scripts'))
    import console_regress as cr
    import ftplib
    cfg = cr.load_settings()
    cfg = dict(cfg, dir=cfg['dir'].rsplit('/', 1)[0] + '/Harissa64v2_rtest',
               xdir=cfg['xdir'].rsplit('\\', 1)[0] + '\\Harissa64v2_rtest')
    con = cr.Console(cfg)
    if not con.wait_ftp(limit=60):
        sys.exit('console FTP not answering')

    def remote_sizes():
        g = con.ftp()
        out = {}

        def walk(path, rel):
            g.cwd(path)
            lines = []
            g.retrlines('LIST', lines.append)
            for l in lines:
                p = l.split(None, 8)
                if len(p) < 9:
                    continue
                if p[0].startswith('d'):
                    walk(path + '/' + p[8], rel + p[8] + '/')
                else:
                    out[rel + p[8]] = int(p[4])
        try:
            walk(cfg['dir'], '')
        except ftplib.error_perm:
            pass
        g.quit()
        return out
    sizes = remote_sizes()
    sent = 0
    for d, _, files in os.walk(a.out):
        for fn in files:
            p = os.path.join(d, fn)
            rel = os.path.relpath(p, a.out).replace(os.sep, '/')
            # The xex and the ini always (two builds can have the same size), scenes when their size differs.
            if '/' not in rel or sizes.get(rel) != os.path.getsize(p):
                con.put(open(p, 'rb').read(), rel)
                sent += 1
    for rel in sizes:   # results of an earlier run
        if os.path.basename(rel).startswith('xenos_f') or rel == 'harissa64v2.log':
            con.delete(rel)
    print('uploaded %d files' % sent, flush=True)
    con.xb('xbreboot.exe', cfg['xdir'] + '\\default.xex')
    time.sleep(20)
    t0 = time.time()
    log = b''
    while time.time() - t0 < a.timeout:
        if con.wait_ftp(limit=10):
            try:
                log = con.get('harissa64v2.log') or b''
            except Exception:
                log = b''
            if b'[rtest] done' in log:
                break
        time.sleep(5)
    if b'[rtest] done' not in log:
        sys.exit('no "[rtest] done" in the log after %d s' % a.timeout)
    time.sleep(3)
    os.makedirs(a.back, exist_ok=True)
    open(os.path.join(a.back, 'harissa64v2.log'), 'wb').write(log)
    n = 0
    for rel in remote_sizes():
        if os.path.basename(rel).startswith('xenos_f'):
            p = os.path.join(a.back, rel.replace('/', os.sep))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'wb').write(con.get(rel))
            n += 1
    print('fetched %d images; %s' % (n, [l for l in log.decode('utf-8', 'replace').splitlines() if 'done' in l][-1:]), flush=True)


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
    r = sub.add_parser('run', help='FTP upload of OUT, xbreboot launch, wait, fetch the images into BACK')
    r.add_argument('out')
    r.add_argument('back')
    r.add_argument('--timeout', type=int, default=600)
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
    if a.cmd == 'run':
        run_console(a)
        return 0
    return collect(a)


if __name__ == '__main__':
    sys.exit(main())
