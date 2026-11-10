#!/usr/bin/env python3
"""Host tests for production void visibility and same-frame NVN fill ordering."""
import argparse
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
args = parser.parse_args()
out = root / "obj/tests"
out.mkdir(parents=True, exist_ok=True)
nvn = (root / "src/ootgz/nvn.c").read_text(encoding="utf-8")
start = nvn.index("/* Read the current picture,")
end = nvn.index("static void draw_overlay(", start)
(out / "void_fill_production.inc").write_text(nvn[start:end], encoding="utf-8")
env = os.environ.copy()
env["PATH"] = str(Path(args.cc).parent) + os.pathsep + env.get("PATH", "")
hidden = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
for name in ("void_view", "void_fill"):
    exe = out / (name + (".exe" if os.name == "nt" else ""))
    extra = [] if name == "void_view" else ["src/ootgz/void_view.c"]
    for cmd in ([args.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Isrc/ootgz",
                 "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                 f"tests/{name}.c", "src/ootgz/world_view.c"] + extra + ["-lm", "-o", str(exe)], [str(exe)]):
        run = subprocess.run(cmd, cwd=root, env=env, capture_output=True, text=True, **hidden)
        print(run.stdout, end="")
        print(run.stderr, end="")
        run.check_returncode()
