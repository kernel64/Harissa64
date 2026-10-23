"""Save-state suite: replays the states of tests/states.txt on the PC.

Each state runs for a few seconds (graphics and audio HLE, as on the Xbox) with
h64test --render-hash: a hash of every RDP command and HLE triangle a GPU
renderer receives, plus the CPU and RDRAM hashes at the end. They are compared
with tests/expected/states.txt. When a game's hashes do not change and the Xbox
renderer (render/xenos) was not touched, the console draws that scene as before.
The states run in parallel (one h64test per core): about a minute for 30 states.

    python tests/scripts/run_states.py                 compare
    python tests/scripts/run_states.py --only NAME     one case (repeatable)
    python tests/scripts/run_states.py --update        accept the current hashes

States and ROMs are local (tests/console/data, xbox64/Release/roms: never
committed); a case whose files are missing is skipped. Exit code 1 on a change.
"""
import argparse, os, re, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
H64TEST = os.path.join(ROOT, 'build-msvc', 'h64test.exe')
ROMDIR = os.path.join(ROOT, 'xbox64', 'Release', 'roms')
DATA = os.path.join(ROOT, 'tests', 'console', 'data')
CASES = os.path.join(ROOT, 'tests', 'states.txt')
EXPECTED = os.path.join(ROOT, 'tests', 'expected', 'states.txt')


def load_cases():
    cases = []
    for line in open(CASES, encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        f = line.split('|')
        if len(f) != 5:
            sys.exit('tests/states.txt: 5 fields expected: ' + line)
        cases.append({'name': f[0], 'rom': f[1], 'state': f[2], 'frames': int(f[3]), 'input': f[4]})
    return cases


def run(c):
    rom = os.path.join(ROMDIR, c['rom'])
    st = os.path.join(DATA, c['state'])
    if not os.path.isfile(rom) or not os.path.isfile(st):
        return c['name'], None, 'missing ' + ('ROM' if not os.path.isfile(rom) else 'state')
    cmd = [H64TEST, '--rom', rom, '--hle', '--render-hash', '--load-state', st, '--more-frames', str(c['frames'])]
    if c['input']:
        cmd += ['--input', c['input'], '--input-after-load']
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    out = r.stdout
    m1 = re.search(r'state hash cpu=([0-9A-F]+) ram=([0-9A-F]+)', out)
    m2 = re.search(r'render hash ([0-9A-F]+) \((\d+) commands, (\d+) triangles\)', out)
    if r.returncode or not m1 or not m2:
        tail = (out + r.stderr).strip().splitlines()[-3:]
        return c['name'], None, 'h64test failed: ' + ' / '.join(tail)
    return c['name'], 'render=%s cpu=%s ram=%s' % (m2.group(1), m1.group(1), m1.group(2)), \
        '%s commands, %s triangles, %.1f s' % (m2.group(2), m2.group(3), time.time() - t0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--update', action='store_true')
    ap.add_argument('--only', action='append', default=[])
    a = ap.parse_args()
    if not os.path.isfile(H64TEST):
        sys.exit('build build-msvc/h64test.exe first')
    cases = [c for c in load_cases() if not a.only or c['name'] in a.only]
    expected = {}
    if os.path.isfile(EXPECTED):
        for line in open(EXPECTED, encoding='utf-8'):
            if line.strip() and not line.startswith('#'):
                n, h = line.rstrip('\n').split('|', 1)
                expected[n] = h
    t0 = time.time()
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 8) as ex:
        results = list(ex.map(run, cases))
    failed, skipped = [], 0
    for name, h, info in results:
        if h is None:
            print('%-16s SKIP  %s' % (name, info) if info.startswith('missing') else '%-16s FAIL  %s' % (name, info))
            if info.startswith('missing'):
                skipped += 1
            else:
                failed.append(name)
            continue
        if a.update:
            expected[name] = h
            print('%-16s SET   %s (%s)' % (name, h, info))
        elif name not in expected:
            print('%-16s NEW   %s (%s): accept with --update' % (name, h, info))
        elif expected[name] != h:
            print('%-16s DIFF  %s, was %s (%s)' % (name, h, expected[name], info))
            failed.append(name)
        else:
            print('%-16s OK    (%s)' % (name, info))
    if a.update:
        with open(EXPECTED, 'w', encoding='utf-8', newline='\n') as f:
            f.write('# Hashes of tests/states.txt cases (tests/scripts/run_states.py --update)\n')
            for n in sorted(expected):
                f.write('%s|%s\n' % (n, expected[n]))
    print('RESULT: %s (%d cases, %d skipped, %.0f s)' % (
        'ALL OK' if not failed else 'CHANGED: ' + ', '.join(failed), len(results), skipped, time.time() - t0))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
