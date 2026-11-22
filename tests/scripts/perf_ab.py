#!/usr/bin/env python3
"""Speed A/B on the Xbox 360: the same save states with two xex files, the
front end's [prof] and [perf] lines compared (ms per frame, VI/s).

  python tests/scripts/perf_ab.py OLD.xex NEW.xex [--only NAME ...] [--seconds 14] [--runs 2]

Uses tests/console/console.local and the save states of tests/console/data,
as console_regress.py does: the console's saves and harissa64v2.ini are
backed up first and put back afterwards (nothing runs without a complete
backup); the xex in the emulator's folder is put back as it was at the end.
Take the console lock first (and send "debugger disconnect": an attached
debugger froze the console during such runs).
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import console_regress as cr  # noqa: E402

CASES = [   # name, ROM, state (tests/console/data); the release speed checks' five states
    ('dk64', 'Donkey Kong 64 (U).zip', 'dk64_user1.h64s'),
    ('oot', 'Legend of Zelda The - Ocarina of Time.zip', 'oot_kokiri.h64s'),
    ('pm', 'Paper Mario (U).zip', 'pm_house.h64s'),
    ('re2', 'Resident Evil 2 (U) (V1.1).zip', 're2_street.h64s'),
    ('yoshi', "Yoshi's Story (U) (M2).zip", 'yoshi_level.h64s'),
]
PROF = re.compile(r'\[prof\] ms/frame: run ([\d.]+) \(cpu ([\d.]+), gfx parse ([\d.]+), gfx hle ([\d.]+) incl. render ([\d.]+)')
PERF = re.compile(r'\[perf\] vi/s=([\d.]+)')
KEEP_LOGS = None
SKIP = 1


def one(con, xex_name, case, seconds, extra=()):
    name, rom, state = case
    folder = cr.save_folder(rom)
    con.put(open(os.path.join(cr.DATA, state), 'rb').read(), 'saves/%s/state%d.h64s' % (folder, cr.SLOT))
    frames = int(seconds * 60)
    ini = ['mode=play', 'cpu=dynarec', 'hle=1', 'rom=game:\\roms\\' + rom, 'stateslot=%d' % cr.SLOT, 'loadstate=1',
           'exitafter=%d' % frames] + list(extra)
    con.put(('\r\n'.join(ini) + '\r\n').encode(), 'harissa64v2.ini')
    con.delete('harissa64v2.log')
    con.xb('xbreboot.exe', con.cfg['xdir'] + '\\' + xex_name)
    time.sleep(10 + seconds)
    con.wait_ftp(limit=120)
    time.sleep(3)
    raw = con.get('harissa64v2.log') or b''
    if KEEP_LOGS:   # --keep-logs DIR: each run's log, for the [xprof]/[perf] breakdown
        n = len([f for f in os.listdir(KEEP_LOGS) if f.startswith('%s_%s' % (name, xex_name))])
        open(os.path.join(KEEP_LOGS, '%s_%s_%d.log' % (name, xex_name, n + 1)), 'wb').write(raw)
    log = raw.decode('utf-8', 'replace')
    prof = [tuple(float(x) for x in m.groups()) for m in PROF.finditer(log)]
    vis = [float(m.group(1)) for m in PERF.finditer(log)]
    # the first period holds the state load; --skip more to leave out the first
    # shader compilations (steady speed) and see them apart in 'warm'
    warm = prof[1][0] if len(prof) > 1 else 0.0
    prof, vis = prof[SKIP:], vis[SKIP:]
    if not prof:
        return None
    avg = [sum(p[i] for p in prof) / len(prof) for i in range(5)]
    return {'run': avg[0], 'cpu': avg[1], 'parse': avg[2], 'gfx': avg[3], 'render': avg[4],
            'vis': sum(vis) / len(vis) if vis else 0.0, 'periods': len(prof), 'warm': warm}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('old')
    ap.add_argument('new')
    ap.add_argument('--only', action='append', default=[])
    ap.add_argument('--seconds', type=float, default=14)
    ap.add_argument('--runs', type=int, default=2)
    ap.add_argument('--keep-logs', help='folder for each run\'s log')
    ap.add_argument('--old-ini', action='append', default=[], help='extra ini key for the old side (key=value)')
    ap.add_argument('--new-ini', action='append', default=[], help='extra ini key for the new side (key=value)')
    ap.add_argument('--case', action='append', default=[], help='extra case NAME|ROM|STATE (tests/console/data)')
    ap.add_argument('--skip', type=int, default=1, help='2-s periods left out at the start (default 1: the state load)')
    a = ap.parse_args()
    global KEEP_LOGS, SKIP
    SKIP = max(1, a.skip)
    if a.keep_logs:
        os.makedirs(a.keep_logs, exist_ok=True)
        KEEP_LOGS = a.keep_logs
    cases = CASES + [tuple(c.split('|')) for c in a.case]
    cfg = cr.load_settings()
    con = cr.Console(cfg)
    if not con.wait_ftp():
        sys.exit('console not answering')
    user_ini = con.get('harissa64v2.ini')
    keep = os.path.join(cr.DATA, 'user_ini.txt')
    if user_ini is not None and (b'exitafter=' in user_ini or b'loadstate=' in user_ini or b'pauseat=' in user_ini):
        user_ini = open(keep, 'rb').read()
    elif user_ini is not None:
        open(keep, 'wb').write(user_ini)
    backup = os.path.join(cr.DATA, 'saves_backup')
    print('saves backed up: %d files' % con.backup_saves(backup), flush=True)
    installed = con.get('harissa64v2.xex')
    con.put(open(a.old, 'rb').read(), 'ab_old.xex')
    con.put(open(a.new, 'rb').read(), 'ab_new.xex')
    res = {}
    try:
        for case in cases:
            if a.only and case[0] not in a.only:
                continue
            for r in range(a.runs):
                for tag in ('old', 'new'):
                    try:
                        m = one(con, 'ab_%s.xex' % tag, case, a.seconds, a.old_ini if tag == 'old' else a.new_ini)
                    except (OSError, EOFError) as e:   # an FTP timeout loses this run only
                        print('%-6s %s run %d: %s' % (case[0], tag, r + 1, e), flush=True)
                        con.wait_ftp(limit=120)
                        m = None
                    res.setdefault((case[0], tag), []).append(m)
                    print('%-6s %s run %d: %s' % (case[0], tag, r + 1,
                          'no [prof] lines' if not m else 'run %.2f gfx %.2f render %.2f parse %.2f ms/frame, %.1f VI/s'
                          ' (first period after the load: run %.1f)'
                          % (m['run'], m['gfx'], m['render'], m['parse'], m['vis'], m['warm'])), flush=True)
    finally:
        # Each step retried on its own: an FTP timeout must not skip the ini and saves.
        def step(what, fn):
            for attempt in range(3):
                try:
                    return fn()
                except (OSError, EOFError) as e:
                    print('cleanup: %s failed (%s), retrying' % (what, e), flush=True)
                    time.sleep(5)
                    con.wait_ftp(limit=120)
            print('cleanup: %s FAILED, do it by hand' % what, flush=True)
            return None
        step('back to the dashboard', lambda: con.xb('xbreboot.exe'))
        con.wait_ftp()
        step('delete ab_old.xex', lambda: con.delete('ab_old.xex'))
        step('delete ab_new.xex', lambda: con.delete('ab_new.xex'))
        if installed is not None:
            step('xex check', lambda: con.get('harissa64v2.xex') == installed or con.put(installed, 'harissa64v2.xex'))
        if user_ini is not None:
            step('user ini', lambda: con.put(user_ini, 'harissa64v2.ini'))
            print('user ini restored:', step('ini check', lambda: con.get('harissa64v2.ini') == user_ini), flush=True)
        ok = step('saves', lambda: con.restore_saves(backup))
        print('saves restored:', 'identical' if ok else 'MISMATCH, check ' + backup, flush=True)
    print('\ncase    old run/gfx/render ms   new run/gfx/render ms   VI/s old -> new')
    for case in cases:
        o = [m for m in res.get((case[0], 'old'), []) if m]
        n = [m for m in res.get((case[0], 'new'), []) if m]
        if not o or not n:
            continue
        av = lambda ms, k: sum(m[k] for m in ms) / len(ms)  # noqa: E731
        print('%-6s  %5.2f / %5.2f / %5.2f      %5.2f / %5.2f / %5.2f      %4.1f -> %4.1f' % (
            case[0], av(o, 'run'), av(o, 'gfx'), av(o, 'render'), av(n, 'run'), av(n, 'gfx'), av(n, 'render'),
            av(o, 'vis'), av(n, 'vis')))


if __name__ == '__main__':
    main()
