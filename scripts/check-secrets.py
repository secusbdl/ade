"""Check Git-visible source files for values from an authorized local key file.

This check emits paths and counts only. It is a targeted check for known values,
not a general secret detector. It never stages files or changes the key file.
"""
import argparse
import base64
import json
from pathlib import Path
import re
import subprocess
from urllib.parse import quote

parser = argparse.ArgumentParser()
parser.add_argument("--key-file", type=Path, required=True)
parser.add_argument("--staged", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
values = []
for line in args.key_file.read_text(encoding="utf-8-sig").splitlines():
    match = re.match(r"^(?:export\s+)?[A-Z][A-Z0-9_]*_API_KEY\s*[:=]\s*(.*?)\s*$", line.strip())
    if not match:
        continue
    value = match[1]
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        value = value[1:-1]
    if value:
        values.append(value)
if not values:
    raise SystemExit("No key values parsed; check did not run.")
needles = set()
for value in values:
    raw = value.encode()
    needles.update((raw, base64.b64encode(raw), quote(value, safe="").encode(), json.dumps(value)[1:-1].encode()))
command = ["git", "diff", "--cached", "--name-only", "--diff-filter=ACM", "-z"] if args.staged else ["git", "ls-files", "-c", "-o", "--exclude-standard", "-z"]
paths = set(filter(None, subprocess.check_output(command, cwd=root).split(b"\0")))
matches = []
for raw_path in sorted(paths):
    path = raw_path.decode("utf-8")
    content = subprocess.check_output(["git", "show", ":" + path], cwd=root) if args.staged else (root / path).read_bytes()
    if any(needle in content for needle in needles):
        matches.append(path)
print(json.dumps({"filesChecked": len(paths), "configuredValuesChecked": len(values), "matchedFiles": matches, "staged": args.staged}))
raise SystemExit(1 if matches else 0)
