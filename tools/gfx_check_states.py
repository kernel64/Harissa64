#!/usr/bin/env python3
"""Graphics HLE check over the save-state corpus (level B, docs-local/render_tests_study.md).

Each case of tests/states.txt runs FRAMES VI frames with its graphics tasks on
the LLE RSP; each task also runs through the graphics HLE on a copy and the
pictures are compared (h64test --hle-gfx-check). Prints one line per case and
the details of the cases with differences.

  python tools/gfx_check_states.py OUTDIR [--only NAME ...] [--frames 10] [--jobs 8]

PNGs (LLE | HLE | heat map) go to OUTDIR/<case>/: game pictures, keep OUTDIR
outside the repository.
"""
import argparse
import concurrent.futures
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
H64TEST = os.path.join(ROOT, 'build-msvc', 'h64test.exe')
ROMS = os.path.join(ROOT, 'xbox64', 'Release', 'roms')
STATES = os.path.join(ROOT, 'tests', 'console', 'data')


def cases():
    out = []
    for line in open(os.path.join(ROOT, 'tests', 'states.txt'), encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        p = line.split('|')
        out.append({'name': p[0], 'rom': p[1], 'state': p[2], 'input': p[4] if len(p) > 4 else ''})
    return out


def run(c, out, frames):
    d = os.path.join(out, c['name'])
    os.makedirs(d, exist_ok=True)
    cmd = [H64TEST, '--rom', os.path.join(ROMS, c['rom']), '--hle-audio', '--load-state', os.path.join(STATES, c['state']),
           '--more-frames', str(frames), '--hle-gfx-check', d]
    if c['input']:
        cmd += ['--input', c['input'], '--input-after-load']
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    lines = [l for l in r.stdout.splitlines() if l.startswith('[gfxcheck]') or l.startswith('  ') or l.startswith('    op')]
    with open(os.path.join(d, 'gfxcheck.txt'), 'w', encoding='utf-8') as f:
        f.write(r.stdout)
    return c['name'], r.returncode, lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--only', action='append', default=[])
    ap.add_argument('--frames', type=int, default=10)
    ap.add_argument('--jobs', type=int, default=8)
    a = ap.parse_args()
    todo = [c for c in cases() if not a.only or c['name'] in a.only]
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        results = list(ex.map(lambda c: run(c, a.out, a.frames), todo))
    for name, rc, lines in results:
        summary = [l for l in lines if 'graphics tasks compared' in l]
        print('%-16s %s' % (name, summary[0][11:] if summary else 'no summary (exit %d)' % rc))
    print()
    for name, rc, lines in results:
        detail = [l for l in lines if 'graphics tasks compared' not in l]
        if any('over' in l or 'steps apart' in l for l in detail):
            print('== ' + name)
            for l in detail[:14]:
                print(l)


if __name__ == '__main__':
    main()
