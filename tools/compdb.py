#!/usr/bin/env python3
"""Write compile_commands.json for editors (VS Code, clangd).

Runs `make -B -n all` (a dry run: nothing is built) and records the exact
compiler command for every .c/.S file, so the editor sees the same include
paths and flags as the real build (kernel, user programs and drivers each
have their own). Run it from the repo root: `make compdb`.
"""
import json
import os
import re
import shlex
import shutil
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = subprocess.run(["make", "-B", "-n", "all"], cwd=root, capture_output=True, text=True)
if out.returncode != 0:
    sys.exit("make -n failed:\n" + out.stderr)

entries = {}
for line in out.stdout.splitlines():
    if "gcc" not in line or " -c " not in line:
        continue
    # A recipe line can be several commands joined with && or ;
    for cmd in re.split(r"\s*(?:&&|;)\s*", line):
        try:
            args = shlex.split(cmd)
        except ValueError:
            continue
        if not args or not args[0].endswith("gcc") or "-c" not in args:
            continue
        src = args[args.index("-c") + 1]
        if not src.endswith((".c", ".S")):
            continue
        # Absolute compiler path: VS Code started from the Dock has no
        # /opt/homebrew/bin on its PATH.
        args[0] = shutil.which(args[0]) or args[0]
        # One entry per file: the first command that compiles it.
        entries.setdefault(src, {"directory": root, "arguments": args, "file": src})

with open(os.path.join(root, "compile_commands.json"), "w") as f:
    json.dump(list(entries.values()), f, indent=1)
print(f"compile_commands.json: {len(entries)} files")
