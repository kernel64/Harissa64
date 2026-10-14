#!/usr/bin/env python3
"""Regression checks on the Xbox 360 itself, on the picture the console sends to the TV.

Each case (tests/console/cases.txt) starts a game from a save state, presses its
buttons, freezes one frame for 20 s (ini pauseat=) and captures the console's
front buffer with the XDK's xbcapture: the final GPU picture, after the display
pass, which internal screenshots (shots=) do not show. The capture is compared
with a reference picture validated by eye (--update records it).

Usage:
  python tests/scripts/console_regress.py [--only NAME ...] [--update] [--xex PATH] [--list] [--accept NAME ...]

Local data, never committed (copyrighted game memory and pictures), in
tests/console/data/:
  <state file named in the case>   the save state (h64test --save-state or a console state)
  <name>.png                       the reference capture (--update)
  out/<name>.png, out/report.txt   the last run
Console settings, never committed: tests/console/console.local, lines key=value:
  ip=...  ftpuser=...  ftppass=...  dir=/Hdd1/Emulators/Harissa64v2  (FTP path of the emulator)
  xdir=hdd:\\Emulators\\Harissa64v2  (the same folder as the console sees it)

The console's saves folder is backed up first and put back afterwards; nothing
runs if the backup is incomplete. The user's harissa64v2.ini is put back too.
"""
import argparse, ftplib, io, os, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CDIR = os.path.join(ROOT, 'tests', 'console')
DATA = os.path.join(CDIR, 'data')
OUT = os.path.join(DATA, 'out')
XDK = os.environ.get('XEDK', r'D:\App\Microsoft Xbox 360 SDK')
XBIN = os.path.join(XDK, 'bin', 'win32')
H64TEST = os.path.join(ROOT, 'build-msvc', 'h64test.exe')
ROMDIR = os.path.join(ROOT, 'xbox64', 'Release', 'roms')
SLOT = 9                 # the state slot the checks use (the user's states stay)
THRESHOLD = 8.0          # mean absolute difference (0..255) on 320x180 pictures

try:
    from PIL import Image
except ImportError:
    sys.exit('needs Pillow (pip install pillow)')


def load_settings():
    path = os.path.join(CDIR, 'console.local')
    if not os.path.isfile(path):
        sys.exit('missing %s (ip=, ftpuser=, ftppass=, dir=, xdir=)' % path)
    cfg = {}
    for line in open(path, encoding='utf-8'):
        line = line.strip()
        if line and not line.startswith('#') and '=' in line:
            k, v = line.split('=', 1)
            cfg[k.strip()] = v.strip()
    for k in ('ip', 'ftpuser', 'ftppass', 'dir', 'xdir'):
        if k not in cfg:
            sys.exit('console.local: missing %s=' % k)
    return cfg


def load_cases(fname='cases.txt'):
    cases = []
    for line in open(os.path.join(CDIR, fname), encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        f = line.split('|')
        if len(f) != 7:
            sys.exit('cases.txt: 7 fields expected: ' + line)
        cases.append({'name': f[0], 'rom': f[1], 'state': f[2], 'frame': int(f[3]), 'input': f[4],
                      'pauseat': int(f[5]), 'extra': [kv for kv in f[6].split(';') if kv]})
    return cases


class Console:
    def __init__(self, cfg):
        self.cfg = cfg

    def ftp(self):
        f = ftplib.FTP(self.cfg['ip'], timeout=60)
        f.login(self.cfg['ftpuser'], self.cfg['ftppass'])
        return f

    def wait_ftp(self, limit=180):
        t0 = time.time()
        while time.time() - t0 < limit:
            try:
                self.ftp().quit()
                return True
            except Exception:
                time.sleep(3)
        return False

    def put(self, data, rel):
        f = self.ftp()
        f.storbinary('STOR ' + self.cfg['dir'] + '/' + rel, io.BytesIO(data))
        f.quit()

    def get(self, rel):
        f = self.ftp()
        b = io.BytesIO()
        try:
            f.retrbinary('RETR ' + self.cfg['dir'] + '/' + rel, b.write)
        except ftplib.error_perm:
            f.quit()
            return None
        f.quit()
        return b.getvalue()

    def delete(self, rel):
        f = self.ftp()
        try:
            f.delete(self.cfg['dir'] + '/' + rel)
        except ftplib.error_perm:
            pass
        f.quit()

    def walk(self, f, path, rel=''):
        out = []
        f.cwd(path)
        lines = []
        f.retrlines('LIST', lines.append)
        for l in lines:
            p = l.split(None, 8)
            if len(p) < 9:
                continue
            if p[0].startswith('d'):
                out += self.walk(f, path + '/' + p[8], rel + p[8] + '/')
            else:
                out.append(rel + p[8])
        return out

    def backup_saves(self, dest):
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        f = self.ftp()
        root = self.cfg['dir'] + '/saves'
        files = self.walk(f, root)
        for rel in files:
            p = os.path.join(dest, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, 'wb') as o:
                f.retrbinary('RETR ' + root + '/' + rel, o.write)
        f.quit()
        if not files:
            sys.exit('saves backup: nothing found, stopping')
        open(os.path.join(dest, '.complete'), 'w').write(str(len(files)))
        return len(files)

    def restore_saves(self, src):
        if not os.path.isfile(os.path.join(src, '.complete')):
            sys.exit('restore refused: incomplete backup ' + src)
        mine = {}
        for d, _, files in os.walk(src):
            for fn in files:
                if fn != '.complete':
                    p = os.path.join(d, fn)
                    mine[os.path.relpath(p, src).replace(os.sep, '/')] = p
        f = self.ftp()
        root = self.cfg['dir'] + '/saves'
        for rel in self.walk(f, root):
            if rel not in mine:
                f.delete(root + '/' + rel)
        for rel, p in mine.items():
            with open(p, 'rb') as i:
                f.storbinary('STOR ' + root + '/' + rel, i)
        again = set(self.walk(f, root))
        f.quit()
        return again == set(mine)

    def xb(self, tool, *args):
        return subprocess.run([os.path.join(XBIN, tool), '/x', self.cfg['ip']] + list(args),
                              capture_output=True, text=True, errors='replace')

    def capture(self, path):
        self.xb('xbcapture.exe', path)
        return os.path.isfile(path)


def small(path):
    return Image.open(path).convert('RGB').resize((320, 180))


def noisy(img):
    # The screen shows noise for a moment while the title quits; shrunk to
    # 320x180 it averages to an even grey, so a run of such captures looked
    # like a frozen frame and could win over the real one. Neighbouring
    # pixels of noise differ far more than in any game picture.
    p = img.convert('L').tobytes()
    w, h = img.size
    tot = n = 0
    for y in range(0, h, 2):
        row = y * w
        for x in range(0, w - 1, 2):
            tot += abs(p[row + x] - p[row + x + 1])
            n += 1
    return n and tot / float(n) > 13.0


def diff(a, b):
    pa, pb = a.tobytes(), b.tobytes()
    return sum(abs(x - y) for x, y in zip(pa, pb)) / float(len(pa))


def save_folder(rom):
    r = subprocess.run([H64TEST, '--rom', os.path.join(ROMDIR, rom), '--info'], capture_output=True, text=True, errors='replace')
    for l in r.stdout.splitlines():
        if 'save folder name:' in l:
            return l.split('save folder name:', 1)[1].strip()
    sys.exit('no save folder name for ' + rom + ' (build-msvc/h64test.exe needed)')


def run_case(con, c, update, log):
    name = c['name']
    boot = c['state'] == '-'   # no state: the game starts from its boot, input frames are absolute
    boot_mode = boot
    st = os.path.join(DATA, c['state'])
    if not boot and not os.path.isfile(st):
        log('%-24s SKIP (no state %s)' % (name, c['state']))
        return None
    if not boot:
        folder = save_folder(c['rom'])
        con.put(open(st, 'rb').read(), 'saves/%s/state%d.h64s' % (folder, SLOT))
    inp = c['input'].replace('{F}+', '').strip()
    events = []
    for ev in [e for e in inp.split(',') if e]:
        k, rng = ev.split('@')
        lo, _, hi = rng.partition('-')
        events.append('%s@%d%s' % (k, c['frame'] + int(lo), ('-%d' % (c['frame'] + int(hi))) if hi else ''))
    ini = ['mode=play', 'cpu=dynarec', 'hle=1', 'rom=game:\\roms\\' + c['rom'],
           'pauseat=%d' % c['pauseat'], 'exitafter=%d' % (c['pauseat'] + 10)]
    if not boot:
        ini += ['stateslot=%d' % SLOT, 'loadstate=1']
    if events:
        ini.append('input=' + ','.join(events))
    ini += c['extra']
    con.put(('\r\n'.join(ini) + '\r\n').encode(), 'harissa64v2.ini')
    con.delete('harissa64v2.log')
    tmp = os.path.join(OUT, 'tmp')
    if os.path.isdir(tmp):
        shutil.rmtree(tmp)
    os.makedirs(tmp)
    con.xb('xbreboot.exe', con.cfg['xdir'] + '\\harissa64v2.xex')
    # Captures for ~90 s; the frozen frame is the longest run of near-identical
    # captures that does not look like the dashboard (taken before launch).
    # Captures stop the title for ~300 ms each, so none is taken while it
    # runs: the frame freezes (pauseat=) for 20 s after about BOOT s plus the
    # VIs before it; captures start a little after that and last 18 s. (The
    # log cannot tell: the FTP answers too slowly while a title runs.)
    boot = 16.0 if not boot_mode else 8.0
    wait = boot + c['pauseat'] / 60.0 * 1.15 + 2.0
    time.sleep(wait)
    caps = []
    t0 = time.time()
    while time.time() - t0 < 18:
        p = os.path.join(tmp, '%03d.bmp' % len(caps))
        if con.capture(p):
            full = Image.open(p)
            cw, ch = full.size
            centre = full.crop((cw // 2 - 160, ch // 2 - 90, cw // 2 + 160, ch // 2 + 90))
            caps.append(None if noisy(centre) else small(p))
        else:
            caps.append(None)
        time.sleep(0.5)
    con.xb('xbreboot.exe')
    con.wait_ftp()
    dash = run.dashboard
    best, bestLen, i = -1, 0, 0
    while i < len(caps):
        if caps[i] is None or (dash and diff(caps[i], dash) < 4.0):
            i += 1
            continue
        j = i + 1
        while j < len(caps) and caps[j] is not None and diff(caps[j], caps[i]) < 2.0:
            j += 1
        if j - i > bestLen:
            best, bestLen = i, j - i
        i = j
    try:
        logtxt = con.get('harissa64v2.log') or b''
    except Exception:
        time.sleep(5)
        logtxt = con.get('harissa64v2.log') or b''
    open(os.path.join(OUT, name + '.log'), 'wb').write(logtxt)
    if best < 0 or bestLen < 2 or b'paused at VI' not in logtxt:
        log('%-24s FAIL (no frozen frame captured)' % name)
        return False
    shot = os.path.join(OUT, name + '.png')
    Image.open(os.path.join(tmp, '%03d.bmp' % best)).convert('RGB').save(shot)
    ref = os.path.join(DATA, name + '.png')
    if update or not os.path.isfile(ref):
        # Never accepted automatically: --accept NAME copies it once it has
        # been checked by eye (by the user too).
        log('%-24s TO CHECK %s (accept with --accept %s)' % (name, shot, name))
        return None
    d = diff(small(shot), small(ref))
    ok = d < THRESHOLD
    log('%-24s %s (difference %.1f, limit %.1f) %s' % (name, 'OK' if ok else 'FAIL', d, THRESHOLD, shot))
    return ok


def run():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', action='append', default=[])
    ap.add_argument('--update', action='store_true')
    ap.add_argument('--xex', help='upload this xex first (else the console keeps its own)')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--cases', default='cases.txt', help='case list in tests/console (explore.txt: games not checked yet)')
    ap.add_argument('--accept', action='append', default=[], help='make out/NAME.png the reference of NAME (after checking it)')
    a = ap.parse_args()
    if a.accept:
        for n in a.accept:
            shutil.copyfile(os.path.join(OUT, n + '.png'), os.path.join(DATA, n + '.png'))
            print('reference accepted:', n)
        return 0
    cases = load_cases(a.cases)
    if a.only:
        cases = [c for c in cases if c['name'] in a.only]
    if a.list:
        for c in cases:
            print('%-24s %s' % (c['name'], c['rom']))
        return 0
    cfg = load_settings()
    con = Console(cfg)
    os.makedirs(OUT, exist_ok=True)
    report = open(os.path.join(OUT, 'report.txt'), 'w', encoding='utf-8')

    def log(s):
        print(s)
        report.write(s + '\n')
        report.flush()

    con.xb('xbreboot.exe')
    if not con.wait_ftp():
        sys.exit('console not answering')
    # The user's ini: a test ini left by an interrupted run (pauseat=,
    # exitafter=) is never taken for it; the last clean copy is kept locally.
    user_ini = con.get('harissa64v2.ini')
    keep = os.path.join(DATA, 'user_ini.txt')
    if user_ini is not None and (b'pauseat=' in user_ini or b'exitafter=' in user_ini or b'loadstate=' in user_ini):
        log('the console has a test ini: the saved copy of the user ini will be put back')
        user_ini = open(keep, 'rb').read() if os.path.isfile(keep) else None
    elif user_ini is not None:
        open(keep, 'wb').write(user_ini)
    backup = os.path.join(DATA, 'saves_backup')
    for attempt in range(3):   # the console's FTP sometimes times out
        try:
            n = con.backup_saves(backup)
            break
        except Exception as e:
            log('saves backup attempt %d failed: %s' % (attempt + 1, e))
            if attempt == 2:
                sys.exit('saves backup failed: nothing was changed on the console')
            time.sleep(10)
    log('saves backed up: %d files' % n)
    dpath = os.path.join(OUT, 'dashboard.bmp')
    run.dashboard = small(dpath) if con.capture(dpath) else None
    if a.xex:
        con.put(open(a.xex, 'rb').read(), 'harissa64v2.xex')
        log('xex uploaded: ' + a.xex)
    results = []
    try:
        for c in cases:
            results.append(run_case(con, c, a.update, log))
    finally:
        con.xb('xbreboot.exe')
        con.wait_ftp()
        if user_ini is not None:
            for attempt in range(3):
                try:
                    con.put(user_ini, 'harissa64v2.ini')
                    if con.get('harissa64v2.ini') == user_ini:
                        break
                except Exception:
                    pass
                time.sleep(5)
            log('user ini restored: %s' % ('yes' if con.get('harissa64v2.ini') == user_ini else 'NO, put it back by hand'))
        same = con.restore_saves(backup)
        log('saves restored: %s' % ('identical' if same else 'MISMATCH, check ' + backup))
    failed = [c['name'] for c, r in zip(cases, results) if r is False]
    log('RESULT: %s' % ('ALL OK' if not failed else 'FAILED: ' + ', '.join(failed)))
    return 1 if failed else 0


run.dashboard = None

if __name__ == '__main__':
    sys.exit(run())
