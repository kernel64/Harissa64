#!/usr/bin/env python3
"""Run Harissa64 in Xenia (the user's build with the dynarec patch) and collect its screenshots.

Usage:
  python tests/scripts/xenia_run.py NAME ROM [--input SCRIPT] [--shots 100,200] [--exit N] [--ini key=value ...]
                                       [--xenia PATH] [--dir E:\\h64v2_xenia\\dynarec]

The xex comes from platform/xbox360/Release; the ROM from the ROM folder
(xbox64/Release/roms). Screenshots (BMP of the N64 frame, shots=) and the log
land in DIR/work/cache; they are copied to tests/console/data/xenia/NAME_*.png.
Xenia does not emulate the Xbox 360 GPU exactly: final checks of the picture
stay on the console (console_regress.py).
"""
import argparse, glob, os, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, 'tests', 'console', 'data', 'xenia')

try:
    from PIL import Image
except ImportError:
    sys.exit('needs Pillow')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('rom')
    ap.add_argument('--input', default='')
    ap.add_argument('--shots', default='')
    ap.add_argument('--exit', type=int, default=0)
    ap.add_argument('--ini', nargs='*', default=[])
    ap.add_argument('--timeout', type=int, default=600)
    ap.add_argument('--state', help='save state to load first (slot 9); --input/--shots frames are then counted as the console does')
    ap.add_argument('--folder', help='save folder name of the game (h64test --info), needed with --state')
    ap.add_argument('--xenia', default=r'E:\vs-projects\xenia\build\bin\Windows\Release\xenia.exe')
    ap.add_argument('--dir', default=r'E:\h64v2_xenia\dynarec')
    a = ap.parse_args()
    game, work = os.path.join(a.dir, 'game'), os.path.join(a.dir, 'work')
    cache = os.path.join(work, 'cache')
    os.makedirs(OUT, exist_ok=True)
    shutil.copyfile(os.path.join(ROOT, 'platform', 'xbox360', 'Release', 'harissa64v2.xex'), os.path.join(game, 'harissa64v2.xex'))
    ext = os.path.splitext(a.rom)[1].lower()
    romname = 'rom' + ext
    shutil.copyfile(os.path.join(ROOT, 'xbox64', 'Release', 'roms', a.rom), os.path.join(game, romname))
    ini = ['xenia=1', 'mode=play', 'cpu=dynarec', 'hle=1', 'rom=game:\\' + romname]
    if a.input:
        ini.append('input=' + a.input)
    if a.shots:
        ini.append('shots=' + a.shots)
    if a.exit:
        ini.append('exitafter=%d' % a.exit)
    if a.state:
        sd = os.path.join(cache, 'saves', a.folder)
        os.makedirs(sd, exist_ok=True)
        shutil.copyfile(a.state, os.path.join(sd, 'state9.h64s'))
        ini += ['stateslot=9', 'loadstate=1']
    ini += a.ini
    open(os.path.join(game, 'harissa64v2.ini'), 'w', newline='').write('\r\n'.join(ini) + '\r\n')
    for f in glob.glob(os.path.join(cache, 'shot_*.bmp')) + [os.path.join(cache, 'harissa64v2.log')]:
        if os.path.exists(f):
            os.remove(f)
    t0 = time.time()
    p = subprocess.Popen([a.xenia, '--log_file=' + os.path.join(work, 'xenia.log'), '--mount_cache=true',
                          '--d3d12_readback_resolve=true', os.path.join(game, 'harissa64v2.xex')], cwd=work)
    try:
        while p.poll() is None and time.time() - t0 < a.timeout:
            log = os.path.join(cache, 'harissa64v2.log')
            if a.exit and os.path.exists(log) and b'exitafter reached' in open(log, 'rb').read():
                time.sleep(3)
                break
            time.sleep(2)
    finally:
        if p.poll() is None:
            p.kill()
    log = os.path.join(cache, 'harissa64v2.log')
    if os.path.exists(log):
        shutil.copyfile(log, os.path.join(OUT, a.name + '.log'))
    shots = sorted(glob.glob(os.path.join(cache, 'shot_*.bmp')))
    for s in shots:
        n = os.path.basename(s)[5:-4]
        Image.open(s).convert('RGB').save(os.path.join(OUT, '%s_%s.png' % (a.name, n)))
    perf = [l for l in open(log, errors='replace') if '[perf]' in l][-3:] if os.path.exists(log) else []
    print('%s: %d shots in %d s' % (a.name, len(shots), time.time() - t0))
    for l in perf:
        print('  ' + l.strip()[:100])
    return 0


if __name__ == '__main__':
    sys.exit(main())
