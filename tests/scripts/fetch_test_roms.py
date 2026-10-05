#!/usr/bin/env python3
"""Downloads the public N64 test ROMs into tests/roms/ (git-ignored, never committed).

Suites (pinned for reproducible scores):
  n64-systemtest  lemmy-64/n64-systemtest, MIT. No release: the ROM comes from
                  the CI artifact "n64-systemtest-z64" (needs `gh` logged in;
                  GitHub artifacts expire after 90 days). Without it, build it:
                  see BUILD_SYSTEMTEST below.
  dillonb         Dillonb/n64-tests, release "latest" (no licence stated: local
                  use only). Pass/fail: r30 becomes -1 (pass) or the failing
                  test number.
  peterlemon      PeterLemon/N64, public domain (Unlicense). Only the test ROMs
                  and their reference screenshots (.png) are fetched, not the
                  2.7 GB repository. Pass/fail is drawn on screen.

Usage: python tests/scripts/fetch_test_roms.py [--force] [suite ...]
"""
import io
import json
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEST = os.path.join(ROOT, "tests", "roms")

PETERLEMON_COMMIT = "7085543e4a19d8c539fc9e0a4d2869e788b4ed4b"
PETERLEMON_DIRS = ("CPUTest/", "RSPTest/", "RDPTest/")
DILLONB_URL = "https://github.com/Dillonb/n64-tests/releases/download/latest/dillon-n64-tests.zip"
SYSTEMTEST_REPO = "lemmy-64/n64-systemtest"
SYSTEMTEST_ARTIFACT = "n64-systemtest-z64"

BUILD_SYSTEMTEST = """
n64-systemtest has no release. To build it (in WSL, user-level, no sudo):
  curl https://sh.rustup.rs -sSf | sh -s -- -y
  source ~/.cargo/env
  git clone https://github.com/lemmy-64/n64-systemtest /tmp/n64-systemtest
  cd /tmp/n64-systemtest && cargo +stable install nust64 && cargo run --release
then copy the produced .z64 to tests/roms/n64-systemtest/n64-systemtest.z64
"""


def log(msg):
    print(msg, flush=True)


def http_get(url, token=None):
    req = urllib.request.Request(url, headers={"User-Agent": "harissa64-fetch"})
    if token:
        req.add_header("Authorization", "Bearer " + token)
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def gh_token():
    try:
        return subprocess.check_output(["gh", "auth", "token"], text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def fetch_systemtest(force):
    out_dir = os.path.join(DEST, "n64-systemtest")
    out = os.path.join(out_dir, "n64-systemtest.z64")
    if os.path.exists(out) and not force:
        log("n64-systemtest: already present")
        return True
    token = gh_token()
    if not token:
        log("n64-systemtest: `gh` is not logged in; cannot download the CI artifact." + BUILD_SYSTEMTEST)
        return False
    listing = json.loads(http_get("https://api.github.com/repos/%s/actions/artifacts?per_page=20" % SYSTEMTEST_REPO, token))
    arts = [a for a in listing.get("artifacts", []) if a["name"] == SYSTEMTEST_ARTIFACT and not a["expired"]]
    if not arts:
        log("n64-systemtest: no unexpired CI artifact." + BUILD_SYSTEMTEST)
        return False
    art = arts[0]
    # The archive URL redirects to blob storage, which rejects the forwarded
    # Authorization header (HTTP 401 with urllib): let gh follow it.
    data = subprocess.check_output(["gh", "api", "repos/%s/actions/artifacts/%d/zip" % (SYSTEMTEST_REPO, art["id"])])
    os.makedirs(out_dir, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        names = [n for n in z.namelist() if n.lower().endswith(".z64")]
        if not names:
            log("n64-systemtest: artifact has no .z64")
            return False
        with open(out, "wb") as f:
            f.write(z.read(names[0]))
    with open(os.path.join(out_dir, "SOURCE.txt"), "w") as f:
        f.write("%s artifact %d, commit %s, created %s\n" % (SYSTEMTEST_REPO, art["id"],
                art["workflow_run"]["head_sha"], art["created_at"]))
    log("n64-systemtest: %s (commit %s)" % (out, art["workflow_run"]["head_sha"][:10]))
    return True


def fetch_dillonb(force):
    out_dir = os.path.join(DEST, "dillonb")
    if os.path.isdir(out_dir) and os.listdir(out_dir) and not force:
        log("dillonb: already present")
        return True
    data = http_get(DILLONB_URL)
    shutil.rmtree(out_dir, ignore_errors=True)
    os.makedirs(out_dir)
    count = 0
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for n in z.namelist():
            if n.lower().endswith(".z64"):
                with open(os.path.join(out_dir, os.path.basename(n)), "wb") as f:
                    f.write(z.read(n))
                count += 1
    log("dillonb: %d ROMs" % count)
    return count > 0


def fetch_peterlemon(force):
    out_dir = os.path.join(DEST, "peterlemon")
    stamp = os.path.join(out_dir, "COMMIT.txt")
    if os.path.exists(stamp) and not force:
        log("peterlemon: already present")
        return True
    token = gh_token()
    tree = json.loads(http_get("https://api.github.com/repos/PeterLemon/N64/git/trees/%s?recursive=1" % PETERLEMON_COMMIT, token))
    wanted = [e["path"] for e in tree["tree"]
              if e["type"] == "blob" and e["path"].startswith(PETERLEMON_DIRS)
              and os.path.splitext(e["path"])[1].upper() in (".N64", ".PNG")]
    for i, path in enumerate(wanted):
        target = os.path.join(out_dir, *path.split("/"))
        if os.path.exists(target) and not force:
            continue
        os.makedirs(os.path.dirname(target), exist_ok=True)
        url = "https://raw.githubusercontent.com/PeterLemon/N64/%s/%s" % (PETERLEMON_COMMIT, urllib.request.quote(path))
        with open(target, "wb") as f:
            f.write(http_get(url))
        if (i + 1) % 50 == 0:
            log("peterlemon: %d/%d" % (i + 1, len(wanted)))
    with open(stamp, "w") as f:
        f.write(PETERLEMON_COMMIT + "\n")
    log("peterlemon: %d files" % len(wanted))
    return True


SUITES = {"n64-systemtest": fetch_systemtest, "dillonb": fetch_dillonb, "peterlemon": fetch_peterlemon}


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    force = "--force" in sys.argv
    ok = True
    for name in (args or list(SUITES)):
        if name not in SUITES:
            log("unknown suite %s (known: %s)" % (name, ", ".join(SUITES)))
            return 2
        ok = SUITES[name](force) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
