"""Build and run collision tests using synthetic data, without a game process."""
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
env = os.environ.copy()
if Path(args.cc).is_absolute():
    env["PATH"] = str(Path(args.cc).parent) + os.pathsep + env.get("PATH", "")
flags = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
for name in ("collision_mesh", "collision_state"):
    exe = out / (name + (".exe" if os.name == "nt" else ""))
    for cmd in ([args.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Isrc/ootgz",
                 "tests/" + name + ".c", "-lm", "-o", str(exe)], [str(exe)]):
        result = subprocess.run(cmd, cwd=root, env=env, capture_output=True, text=True, **flags)
        print(result.stdout, end="")
        print(result.stderr, end="")
        result.check_returncode()
