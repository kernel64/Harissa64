#!/usr/bin/env python3
"""Plays fixed game scenarios with h64test and checks the results against
tests/expected/games.txt (hashes of the VI image and of the audio).

Usage:
  python tests/scripts/run_games.py [--runner "CMD"] [--update] [--only NAME ...]

The ROMs are local copies in tests/roms/commercial/ (never committed). The
screenshots and WAV files go to build-suites/games/ (ignored); only their
SHA-1 hashes are committed, because game images and sound are copyrighted.
A scenario is deterministic: same ROM, same input script, same result on
every host (MSVC, gcc, ppc64 big-endian).
"""
import hashlib
import os
import shlex
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ROMS = os.path.join(ROOT, "tests", "roms", "commercial")
OUT = os.path.join(ROOT, "build-suites", "games")
EXPECTED = os.path.join(ROOT, "tests", "expected", "games.txt")

# name: (rom, emulated seconds, input script in VI frames, see h64test --input)
SM64_GAME = ("START@860-866,A@1000-1006,A@1150-1156,A@1400-1405,A@1500-1505,A@1600-1605,"
             "Y=80@3700-4200,A@3900-3910")
MK64_RACE = "START@600-605," + ",".join("A@%d-%d" % (f, f + 5) for f in
                                        (750, 850, 950, 1050, 1150, 1250, 1350, 1450, 1600, 1700)) + ",A@2150-2550"
OOT_FILE = ("START@1250-1255,START@1700-1705,A@2000-2005,A@2500-2504,START@2600-2604,A@2700-2704,"
            "A@2850-2854,A@2950-2954,A@3050-3054")
SCENARIOS = [
    ("sm64-title", "sm64.z64", 8, ""),           # Mario's head and the title music
    ("sm64-game", "sm64.z64", 68, SM64_GAME),    # file A, Peach's letter, Lakitu, Mario out of the pipe
    ("mk64-title", "mk64.z64", 11, ""),          # title screen after the Nintendo logo
    ("mk64-race", "mk64.z64", 43, MK64_RACE),    # 1P Mario GP 50cc, Mario, Luigi Raceway, racing
    ("oot-title", "oot.z64", 26, ""),            # title over Hyrule field ("PRESS START")
    # File "A" created on the name screen, opened, intro cutscene (the Deku Tree speaks). Further on, the
    # cutscene stops on "It seems the time has come for the boy without a fairy..." (open issue, PROGRESS.md).
    ("oot-cutscene", "oot.z64", 76, OOT_FILE),
]


def sha1(path):
    with open(path, "rb") as f:
        return hashlib.sha1(f.read()).hexdigest()


def load_expected():
    exp = {}
    if os.path.exists(EXPECTED):
        for line in open(EXPECTED):
            parts = line.split()
            if len(parts) == 3 and not line.startswith("#"):
                exp[parts[0]] = (parts[1], parts[2])
    return exp


def main():
    args = sys.argv[1:]
    runner = [os.path.join(ROOT, "build-msvc", "h64test.exe")] if os.name == "nt" else [os.path.join(ROOT, "build-gcc", "h64test")]
    update = False
    only = []
    i = 0
    while i < len(args):
        if args[i] == "--runner": runner = shlex.split(args[i + 1]); i += 2
        elif args[i] == "--update": update = True; i += 1
        elif args[i] == "--only": only.append(args[i + 1]); i += 2
        else: print(__doc__); return 2
    os.makedirs(OUT, exist_ok=True)
    expected = load_expected()
    results = {}
    failures = 0
    for name, rom, seconds, script in SCENARIOS:
        if only and name not in only:
            continue
        rom_path = os.path.join(ROMS, rom)
        if not os.path.exists(rom_path):
            print("%-16s SKIP (no %s)" % (name, rom))
            continue
        png = os.path.join(OUT, name + ".png")
        wav = os.path.join(OUT, name + ".wav")
        cmd = runner + ["--rom", rom_path, "--seconds", str(seconds), "--fb-png", png, "--wav", wav]
        if script:
            cmd += ["--input", script]
        for p in (png, wav):
            if os.path.exists(p):
                os.remove(p)
        subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
        got = (sha1(png) if os.path.exists(png) else "none", sha1(wav) if os.path.exists(wav) else "none")
        results[name] = got
        want = expected.get(name)
        status = "NEW" if want is None else ("OK" if want == got else "CHANGED")
        if status == "CHANGED":
            failures += 1
            detail = " (image %s, audio %s)" % ("same" if want[0] == got[0] else "differs", "same" if want[1] == got[1] else "differs")
        else:
            detail = ""
        print("%-16s %s%s  -> %s" % (name, status, detail, png))
    if update:
        merged = dict(expected)
        merged.update(results)
        os.makedirs(os.path.dirname(EXPECTED), exist_ok=True)
        with open(EXPECTED, "w", newline="\n") as f:
            f.write("# scenario  sha1(VI image PNG)  sha1(audio WAV) - written by tests/scripts/run_games.py --update\n")
            for name, _, _, _ in SCENARIOS:
                if name in merged:
                    f.write("%s %s %s\n" % (name, merged[name][0], merged[name][1]))
        print("expected hashes updated: %s" % EXPECTED)
    return 1 if failures and not update else 0


if __name__ == "__main__":
    sys.exit(main())
