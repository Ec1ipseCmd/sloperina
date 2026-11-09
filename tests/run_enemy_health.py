#!/usr/bin/env python3
"""Compile and run the read-only Enemy Health host tests."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    args = parser.parse_args()
    out = ROOT / "obj" / "tests"
    out.mkdir(parents=True, exist_ok=True)
    exe = out / ("enemy_health.exe" if os.name == "nt" else "enemy_health")
    env = os.environ.copy()
    if Path(args.cc).is_absolute():
        env["PATH"] = str(Path(args.cc).parent) + os.pathsep + env.get("PATH", "")
    flags = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
    for command in ([args.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                     "-Isrc/ootgz", "tests/enemy_health.c", "-lm", "-o", str(exe)], [str(exe)]):
        result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, **flags)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        result.check_returncode()


if __name__ == "__main__":
    main()
