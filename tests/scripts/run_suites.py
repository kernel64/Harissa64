#!/usr/bin/env python3
"""Runs the public test ROM suites with h64test and prints a score.

Usage:
  python tests/scripts/run_suites.py [--runner "CMD"] [--suite NAME ...] [--report FILE]

--runner is the command that runs h64test, e.g. build-msvc/h64test.exe or
"qemu-ppc64 build-ppc64/h64test" (default: build-msvc/h64test.exe on Windows,
build-gcc/h64test elsewhere). Fetch the ROMs first: tests/scripts/fetch_test_roms.py.
"""
import os
import re
import shlex
import struct
import subprocess
import sys
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ROMS = os.path.join(ROOT, "tests", "roms")


def run(runner, args, timeout=600):
    cmd = runner + args
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=ROOT)
        return p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        return -1, "TIMEOUT"


def suite_dillonb(runner):
    results = []
    d = os.path.join(ROMS, "dillonb")
    for name in sorted(os.listdir(d)):
        if not name.endswith(".z64"):
            continue
        code, out = run(runner, ["--rom", os.path.join(d, name), "--dillon", "--seconds", "10"])
        m = re.search(r"DILLON (PASS|FAIL test -?\d+|TIMEOUT)", out)
        status = m.group(1) if m else "ERROR"
        results.append((name, status.startswith("PASS"), status))
    return results


def suite_systemtest(runner):
    rom = os.path.join(ROMS, "n64-systemtest", "n64-systemtest.z64")
    code, out = run(runner, ["--rom", rom, "--seconds", "300"], timeout=3600)
    lines = [l[6:] for l in out.splitlines() if l.startswith("[isv] ")]
    m = re.search(r"Base: Failed (\d+) of (\d+) tests", out)
    if m:
        failed, total = int(m.group(1)), int(m.group(2))
        return [("n64-systemtest", failed == 0, "%d/%d passed" % (total - failed, total))], lines
    return [("n64-systemtest", False, "did not finish")], lines


PETERLEMON_SUITES = {"peterlemon": "CPUTest", "peterlemon-rsp": "RSPTest", "peterlemon-rdp": "RDPTest"}


def png_read(path):
    """Decodes an 8-bit RGB/RGBA/grey PNG (no interlace). Returns (w, h, rows of RGB tuples)."""
    with open(path, "rb") as f:
        data = f.read()
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        typ, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            if depth != 8 or ctype not in (0, 2, 6) or body[12] != 0:
                raise ValueError("unsupported PNG format in " + path)
            bpp = {0: 1, 2: 3, 6: 4}[ctype]
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    prev = bytearray(stride)
    rows = []
    for y in range(h):
        ft = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if ft == 1: line[x] = (line[x] + a) & 255
            elif ft == 2: line[x] = (line[x] + b) & 255
            elif ft == 3: line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if bpp == 1:
            rows.append([(v, v, v) for v in line])
        else:
            rows.append([tuple(line[i:i + 3]) for i in range(0, stride, bpp)])
    return w, h, rows


def image_match(ours, ref):
    """Share of reference pixels matched by our capture, ours scaled 2x on an
    axis where it is half size. Channels are compared on their top 5 bits
    (5551 pixels have no more). The references are captures of the real
    video output, which the VI stretches vertically (e.g. 474 framebuffer
    lines shown as 480 by some ROMs, not by others; no VI emulation yet), so
    each reference line is compared with the best of our lines between the
    unscaled and the stretched position, give or take 3, shifted by up to
    one pixel horizontally."""
    ow, oh, orows = ours
    rw, rh, rrows = ref
    sx = 2 if ow * 2 == rw else 1
    sy = 2 if oh * 2 <= rh + 16 else 1
    q = lambda p: (p[0] >> 3, p[1] >> 3, p[2] >> 3)
    black = [(0, 0, 0)] * rw
    our_q = [[q(row[x // sx]) if x // sx < ow else (0, 0, 0) for x in range(rw)] for row in orows]
    same = 0
    for y in range(rh):
        r = [q(p) for p in rrows[y]]
        lo, hi = (min(y, y * oh // rh), y) if sy == 1 else (y // 2, y // 2)
        best = 0
        for oy in range(lo - 3, hi + 4):
            o = our_q[oy] if 0 <= oy < oh else black
            if r == o:
                best = rw
                break
            for dx in (0, -1, 1):   # the capture also drifts by a pixel horizontally
                shifted = o if dx == 0 else o[dx:] + o[:dx]
                n = sum(1 for a, b in zip(r, shifted) if a == b)
                best = max(best, n)
            if best == rw:
                break
        same += best
    return same / float(rw * rh)


def suite_peterlemon(runner, sub):
    results = []
    base = os.path.join(ROMS, "peterlemon", sub)
    tmp = os.path.join(ROOT, "build-suites")
    os.makedirs(tmp, exist_ok=True)
    out_png = os.path.join(tmp, "peterlemon.png")
    for d, _, files in sorted(os.walk(base)):
        for name in sorted(files):
            if not name.endswith(".N64"):
                continue
            ref_png = os.path.join(d, name[:-4] + ".png")
            if not os.path.exists(ref_png):
                continue   # timing tests have no reference capture
            rel = os.path.relpath(os.path.join(d, name), base).replace(os.sep, "/")
            if os.path.exists(out_png):
                os.remove(out_png)
            code, out = run(runner, ["--rom", os.path.join(d, name), "--seconds", "3", "--fb-png", out_png], timeout=300)
            if not os.path.exists(out_png):
                results.append((rel, False, "no picture"))
                continue
            score = image_match(png_read(out_png), png_read(ref_png))
            results.append((rel, score >= 0.999, "%.2f%% of pixels match" % (score * 100)))
    return results


def main():
    args = sys.argv[1:]
    runner = None
    suites = []
    report = None
    i = 0
    while i < len(args):
        if args[i] == "--runner":
            runner = shlex.split(args[i + 1]); i += 2
        elif args[i] == "--suite":
            suites.append(args[i + 1]); i += 2
        elif args[i] == "--report":
            report = args[i + 1]; i += 2
        else:
            print(__doc__); return 2
    if runner is None:
        runner = [os.path.join(ROOT, "build-msvc", "h64test.exe")] if os.name == "nt" else [os.path.join(ROOT, "build-gcc", "h64test")]
    suites = suites or ["dillonb", "n64-systemtest", "peterlemon", "peterlemon-rsp", "peterlemon-rdp"]

    out_lines = []
    total_pass = total = 0
    for s in suites:
        if s == "dillonb":
            res = suite_dillonb(runner)
            extra = []
        elif s == "n64-systemtest":
            res, extra = suite_systemtest(runner)
        elif s in PETERLEMON_SUITES:
            res = suite_peterlemon(runner, PETERLEMON_SUITES[s])
            extra = []
        else:
            print("unknown suite", s); return 2
        passed = sum(1 for r in res if r[1])
        if s == "n64-systemtest":   # one ROM, many tests: show its own count
            out_lines.append("## %s: %s" % (s, res[0][2]))
        else:
            out_lines.append("## %s: %d/%d" % (s, passed, len(res)))
        for name, ok, status in res:
            out_lines.append("  %-28s %s" % (name, status))
        if extra:
            out_lines.append("  --- ISViewer output (last 60 lines) ---")
            out_lines.extend("  " + l for l in extra[-60:])
        total_pass += passed
        total += len(res)
    out_lines.append("TOTAL %d/%d" % (total_pass, total))
    text = "\n".join(out_lines)
    print(text)
    if report:
        with open(report, "w") as f:
            f.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
