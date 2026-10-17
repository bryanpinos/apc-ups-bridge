#!/usr/bin/env python3
"""Emit a -D BUILD_ID=... flag: git short SHA (+ -dirty) and UTC build time.

WHY: these boards live on walls and in garages with no reachable console, so
"is my fix actually running on this device?" has to be answerable over the
network. Before this existed it took reconstructing the answer from flash logs
that happened to still be in /tmp.

Deliberately avoids % formatting anywhere in the emitted string: platformio.ini
is parsed by configparser, which treats % as interpolation and fails.
"""
import subprocess, datetime

def sh(*a):
    try:
        return subprocess.run(a, capture_output=True, text=True).stdout.strip()
    except Exception:
        return ""

sha = sh("git", "rev-parse", "--short", "HEAD") or "nogit"
try:
    dirty = "" if subprocess.run(["git", "diff", "--quiet"]).returncode == 0 else "-dirty"
except Exception:
    dirty = ""
t = datetime.datetime.now(datetime.timezone.utc).isoformat()[:16].replace("-", "").replace(":", "")
print('-D BUILD_ID=\\"' + sha + dirty + "-" + t + 'Z\\"')
