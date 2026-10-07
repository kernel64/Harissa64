"""Save-state determinism check: straight run vs. save mid-way + load in a new process.
usage: state_check.py RUNNER ROM FRAMES SAVEFRAME OUTDIR [extra h64test args...]"""
import subprocess, sys, re, os

runner, rom, frames, saveat, out = sys.argv[1:6]
extra = sys.argv[6:]
os.makedirs(out, exist_ok=True)
st = os.path.join(out, "mid.h64s")


def run(args, tag):
    cmd = runner.split() + ["--rom", rom, "--frames", frames] + extra + args
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    m = re.search(r"state hash cpu=(\w+) ram=(\w+)", p.stdout)
    saved = re.search(r"\[state\] saved at frame (\d+): (\d+) bytes", p.stdout)
    loaded = re.search(r"\[state\] loaded: frame (\d+)", p.stdout)
    print("%-6s %s %s %s" % (tag, m.groups() if m else ("no hash", p.stdout[-300:], p.stderr[-300:]),
                            "saved f%s %s bytes" % saved.groups() if saved else "",
                            "loaded f%s" % loaded.group(1) if loaded else ""))
    return m.groups() if m else None


a = run(["--fb-png", os.path.join(out, "a.png")], "A")
b = run(["--save-state", "%s:%s" % (saveat, st)], "B")
c = run(["--load-state", st, "--fb-png", os.path.join(out, "c.png")], "C")
ok = a is not None and a == b == c
if ok:
    ok = open(os.path.join(out, "a.png"), "rb").read() == open(os.path.join(out, "c.png"), "rb").read()
print("STATE CHECK", "OK" if ok else "MISMATCH")
