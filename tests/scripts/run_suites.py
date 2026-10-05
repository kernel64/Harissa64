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
import subprocess
import sys

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
    suites = suites or ["dillonb", "n64-systemtest"]

    out_lines = []
    total_pass = total = 0
    for s in suites:
        if s == "dillonb":
            res = suite_dillonb(runner)
            extra = []
        elif s == "n64-systemtest":
            res, extra = suite_systemtest(runner)
        else:
            print("unknown suite", s); return 2
        passed = sum(1 for r in res if r[1])
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
