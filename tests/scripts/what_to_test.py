"""Which checks a change needs, from the files it touches.

    python tests/scripts/what_to_test.py                 the working tree against HEAD
    python tests/scripts/what_to_test.py --since REF     everything since REF (e.g. origin/v2, a release commit)
    python tests/scripts/what_to_test.py --run           also run the PC checks it lists (build first)

The PC checks are cheap and run in order of cost: unit tests, per-game profiles
(check_profiles.py), save states (run_states.py, ~30 s), game scenarios
(run_games.py, ~10 min). Console checks are only listed: they need the shared
console and the user's go. For a profile change, the games whose options
changed are named, with the console cases that cover them.
"""
import argparse, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PY = sys.executable

CHECKS = {
    'unit': ('unit tests', [os.path.join(ROOT, 'build-msvc', 'h64test.exe'), '--unit']),
    'profiles': ('per-game profiles', [PY, 'tests/scripts/check_profiles.py']),
    'states': ('save states (~30 s)', [PY, 'tests/scripts/run_states.py']),
    'games': ('game scenarios (~10 min)', [PY, 'tests/scripts/run_games.py']),
    'suites': ('CPU/RSP/RDP test ROMs', [PY, 'tests/scripts/run_suites.py']),
}
ORDER = ['unit', 'profiles', 'states', 'games', 'suites']

# (path prefix, PC checks, console note). The first matching prefix wins.
RULES = [
    ('core/system/h64_profile', ['unit', 'profiles', 'states'], 'the games check_profiles.py names'),
    ('core/dynarec/', ['unit', 'states', 'games'],
     'the recompiler only runs on the Xbox (and QEMU: tests/scripts/run_wsl.sh, lockstep): console_regress.py cases.txt'),
    ('core/hle/', ['unit', 'states', 'games'], None),
    ('core/rdp/', ['unit', 'states', 'games', 'suites'], None),
    ('core/rsp/', ['unit', 'states', 'games', 'suites'], None),
    ('core/r4300/', ['unit', 'states', 'games', 'suites'], 'console_regress.py cases.txt (the recompiler calls the interpreter)'),
    ('core/', ['unit', 'states', 'games'], None),
    ('render/api.h', ['unit', 'states', 'games'], None),
    ('render/xenos/', ['unit'], 'Xbox renderer: console_regress.py cases.txt (and the cases of the games concerned)'),
    ('platform/xbox360/lang/', [], 'Xbox build (gen_lang.py --check runs in run_all.ps1)'),
    ('platform/xbox360/', [], 'Xbox front end: Xbox build, console_regress.py cases.txt if it touches emulation'),
    ('platform/host/', ['unit', 'states'], None),
    ('third_party/', ['unit', 'states', 'games'], 'Xbox build'),
    ('CMakeLists.txt', ['unit', 'states'], None),
    ('tests/unit/', ['unit'], None),
    ('tests/states.txt', ['states'], None),
    ('tests/expected/', [], None),
    ('tests/', [], None),
    ('docs', [], None),
    ('', [], None),   # README, THIRD_PARTY...: nothing
]


def changed_files(since):
    def git(*args):
        return subprocess.run(['git'] + list(args), cwd=ROOT, capture_output=True, text=True).stdout.split('\n')
    files = set()
    if since:
        files.update(git('diff', '--name-only', since + '...HEAD'))
    files.update(git('diff', '--name-only', 'HEAD'))
    files.update(git('ls-files', '--others', '--exclude-standard'))
    return sorted(f for f in files if f and not f.startswith('tests/console/'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--since')
    ap.add_argument('--run', action='store_true')
    a = ap.parse_args()
    files = changed_files(a.since)
    if not files:
        print('nothing changed')
        return 0
    need, notes = set(), []
    for f in files:
        for prefix, checks, note in RULES:
            if f.startswith(prefix):
                need.update(checks)
                if note and note not in notes:
                    notes.append(note)
                print('  %-50s -> %s' % (f, ', '.join(checks) or 'nothing'))
                break
    plan = [c for c in ORDER if c in need]
    print('\nPC checks: ' + (', '.join(CHECKS[c][0] for c in plan) if plan else 'none'))
    for n in notes:
        print('Console: ' + n)
    if not a.run:
        return 0
    failed = []
    for c in plan:
        print('\n== %s' % CHECKS[c][0])
        r = subprocess.run(CHECKS[c][1], cwd=ROOT)
        if r.returncode:
            failed.append(CHECKS[c][0])
            if c == 'profiles':
                print('(a profile change: check those games, then check_profiles.py --update)')
    print('\nRESULT: ' + ('ALL OK' if not failed else 'CHANGED or FAILED: ' + ', '.join(failed)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
