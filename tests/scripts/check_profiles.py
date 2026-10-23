"""Which games a change affects through their per-game settings.

For every ROM in xbox64/Release/roms (and tests/roms/commercial), h64test
--profile-info prints the emulator options the per-game profile leaves
(core/system/h64_profile.cpp). They are compared with tests/expected/profiles.txt
(game codes and option values only, no ROM data): a game whose options changed
is listed with the options' names, and only those games need checking again
for a profile change.

    python tests/scripts/check_profiles.py            compare
    python tests/scripts/check_profiles.py --update   accept the current options

Exit code 1 when a game changed (or appeared, or disappeared).
"""
import argparse, os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
H64TEST = os.path.join(ROOT, 'build-msvc', 'h64test.exe')
ROMDIRS = [os.path.join(ROOT, 'xbox64', 'Release', 'roms'), os.path.join(ROOT, 'tests', 'roms', 'commercial')]
EXPECTED = os.path.join(ROOT, 'tests', 'expected', 'profiles.txt')
ROM_EXT = ('.z64', '.n64', '.v64', '.zip')


def option_names():
    """The H64Options fields in declaration order (core/system/h64_system.h)."""
    text = open(os.path.join(ROOT, 'core', 'system', 'h64_system.h'), encoding='utf-8').read()
    body = re.search(r'struct H64Options\s*\{(.*?)\n\};', text, re.S).group(1)
    names = []
    for line in body.splitlines():
        line = line.split('//')[0].strip()
        m = re.match(r'(?:int|u32)\s+(.*);$', line)
        if m:
            names += [n.strip() for n in m.group(1).split(',')]
    return names


def roms():
    out = []
    for d in ROMDIRS:
        if os.path.isdir(d):
            out += [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.lower().endswith(ROM_EXT)]
    return out


def profile(path):
    r = subprocess.run([H64TEST, '--rom', path, '--profile-info'], capture_output=True, text=True, errors='replace')
    for line in r.stdout.splitlines():
        if line.startswith('[profile-info] '):
            f = line.split()
            return f[1], f[2:]
    return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--update', action='store_true')
    a = ap.parse_args()
    if not os.path.isfile(H64TEST):
        sys.exit('build build-msvc/h64test.exe first')
    names = option_names()
    files = roms()
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 8) as ex:
        results = list(ex.map(profile, files))
    now = {}
    for path, (code, words) in zip(files, results):
        if code and code not in now:   # the profile depends on the game code only
            now[code] = (words, os.path.basename(path))
    if a.update:
        with open(EXPECTED, 'w', encoding='utf-8', newline='\n') as f:
            f.write('# Options left by the per-game profile, per game code (tests/scripts/check_profiles.py)\n')
            f.write('# fields: ' + ' '.join(names) + '\n')
            for code in sorted(now):
                f.write('%s|%s|%s\n' % (code, ' '.join(now[code][0]), now[code][1]))
        print('%d games written to %s' % (len(now), EXPECTED))
        return 0
    if not os.path.isfile(EXPECTED):
        sys.exit('no %s: run with --update first' % EXPECTED)
    # Compared by option name: adding an option to H64Options changes no game.
    old, oldNames = {}, names
    for line in open(EXPECTED, encoding='utf-8'):
        if line.startswith('# fields: '):
            oldNames = line[len('# fields: '):].split()
            continue
        if line.startswith('#') or not line.strip():
            continue
        code, words, rom = line.rstrip('\n').split('|', 2)
        old[code] = (words.split(), rom)
    changed = 0
    for code in sorted(set(old) | set(now)):
        if code not in now:
            print('%s  no longer found (%s)' % (code, old[code][1]))
            changed += 1
            continue
        if code not in old:
            print('%s  new game (%s)' % (code, now[code][1]))
            changed += 1
            continue
        o0 = dict(zip(oldNames, old[code][0]))
        o1 = dict(zip(names, now[code][0]))
        diffs = ['%s %s -> %s' % (n, o0.get(n, '0'), o1.get(n, '0'))
                 for n in names + [n for n in oldNames if n not in names] if o0.get(n, '0') != o1.get(n, '0')]
        if diffs:
            print('%s  %s: %s' % (code, now[code][1], ', '.join(diffs)))
            changed += 1
    print('RESULT: %s (%d games)' % ('no game affected' if not changed else '%d games affected' % changed, len(now)))
    return 1 if changed else 0


if __name__ == '__main__':
    sys.exit(main())
