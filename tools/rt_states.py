#!/usr/bin/env python3
"""Render test scenes from the save-state corpus (prototype, docs-local/render_tests_study.md).

For each case of tests/states.txt (or --only NAME): record FRAMES VI frames
right after the state loads (h64test --hle --rdp-dump), replay them with the
software RDP (h64test --rt-play) and the oracles (rdp-oracle: ParaLLEl-RDP and
Angrylion), and compare every image drawn (tools/rt_compare.py).

  python tools/rt_states.py OUTDIR [--only NAME ...] [--frames 4] [--oracle EXE] [--png] [--keep]

Scenes and RDRAM files hold game data: keep OUTDIR outside the repository.
Without --keep, the RDRAM files are deleted after the comparison (12 MB each).
"""
import argparse
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
H64TEST = os.path.join(ROOT, 'build-msvc', 'h64test.exe')
ROMS = os.path.join(ROOT, 'xbox64', 'Release', 'roms')
STATES = os.path.join(ROOT, 'tests', 'console', 'data')
ORACLE = r'E:\IA-labs\Xenon64\build-oracle\rdp-oracle.exe'


def cases():
    out = []
    for line in open(os.path.join(ROOT, 'tests', 'states.txt'), encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        p = line.split('|')
        out.append({'name': p[0], 'rom': p[1], 'state': p[2], 'input': p[4] if len(p) > 4 else ''})
    return out


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    return r.returncode, r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--only', action='append', default=[])
    ap.add_argument('--frames', type=int, default=4)
    ap.add_argument('--oracle', default=ORACLE)
    ap.add_argument('--png', action='store_true')
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for c in cases():
        if a.only and c['name'] not in a.only:
            continue
        d = os.path.join(a.out, c['name'])
        os.makedirs(d, exist_ok=True)
        scene = os.path.join(d, 'scene.rdp')
        cmd = [H64TEST, '--rom', os.path.join(ROMS, c['rom']), '--hle', '--load-state', os.path.join(STATES, c['state']),
               '--more-frames', str(a.frames + 2), '--rdp-dump', '0:%d:%s' % (a.frames, scene)]
        if c['input']:
            cmd += ['--input', c['input'], '--input-after-load']
        rc, log = run(cmd)
        rec = [l for l in log.splitlines() if l.startswith('[rtdump]')]
        if rc or not os.path.exists(scene):
            print('%-16s record failed: %s' % (c['name'], log.strip().splitlines()[-1:]))
            continue
        rc1, log1 = run([H64TEST, '--rt-play', scene, d, '--frames', 'all'])
        rc2, log2 = run([a.oracle, scene, d, '--frames', 'all'])
        if rc1 or rc2:
            print('%-16s replay failed: soft %d oracle %d %s' % (c['name'], rc1, rc2, (log1 + log2).strip().splitlines()[-2:]))
            continue
        cmp = [sys.executable, os.path.join(ROOT, 'tools', 'rt_compare.py'), scene, d]
        if a.png:
            cmp.append('--png')
        rc3, log3 = run(cmp)
        print('== %s  %s' % (c['name'], rec[-1] if rec else ''))
        print(log3.rstrip())
        if not a.keep:
            for p in glob.glob(os.path.join(d, '*.rdram')) + glob.glob(os.path.join(d, '*.hidden')):
                os.remove(p)


if __name__ == '__main__':
    main()
