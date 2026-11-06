"""Export committed source for publication without copying development Git history.

No network, credential or Git mutation occurs. The destination must be a new
directory inside out/. Commit reviewed source and run verify-reference.py first.
"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--destination", type=Path, default=Path("out/publish-ade"))
args = parser.parse_args()
destination = (root / args.destination).resolve()
export_root = (root / "out").resolve()
if destination == export_root or not destination.is_relative_to(export_root):
    raise SystemExit("Publication destination must be a new subdirectory of out/.")
if destination.exists():
    raise SystemExit("Publication destination already exists; choose a new path.")


def git(*arguments):
    return subprocess.check_output(["git", *arguments], cwd=root)


if git("status", "--porcelain").strip():
    raise SystemExit("Commit reviewed source first; publication requires a clean working tree.")
subprocess.run([sys.executable, str(root / "scripts/verify-reference.py")], check=True)
source_commit = git("rev-parse", "HEAD").decode().strip()
tree = {}
for record in filter(None, git("ls-tree", "-r", "-z", source_commit).split(b"\0")):
    metadata, name = record.split(b"\t", 1)
    mode, kind, oid = metadata.decode().split()
    if kind != "blob" or mode not in ("100644", "100755"):
        raise SystemExit(f"Unreviewed non-file Git entry: {name.decode()}")
    tree[name.decode()] = oid
files = sorted(tree)
allowed_roots = {"src", "tests", "scripts", "assets", "audit", "reference", "third_party"}
allowed_files = {".gitattributes", ".gitignore", "AGENTS.md", "CMakeLists.txt", "CMakePresets.json",
                 "LICENSE", "NOTICE", "README.md", "resources.qrc"}
raster_extensions = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".avif", ".bmp", ".ico"}
selected, excluded = [], []
for name in files:
    path = PurePosixPath(name)
    if path.parts[0] == "audit" and path.suffix.lower() in raster_extensions:
        excluded.append(name)
        continue
    if name not in allowed_files and path.parts[0] not in allowed_roots:
        raise SystemExit(f"Unreviewed publication path: {name}")
    source = root / name
    if source.is_symlink() or not source.is_file() or not source.resolve().is_relative_to(root):
        raise SystemExit(f"Missing or externally linked publication file: {name}")
    selected.append(name)

destination.mkdir(parents=True)
manifest = []
# Read committed blobs through one process. Native text stays canonical LF;
# reference/-text blobs remain exact, independent of checkout autocrlf settings.
with subprocess.Popen(["git", "cat-file", "--batch"], cwd=root,
                      stdin=subprocess.PIPE, stdout=subprocess.PIPE) as batch:
    for name in selected:
        batch.stdin.write((tree[name] + "\n").encode())
        batch.stdin.flush()
        oid, kind, size = batch.stdout.readline().decode().split()
        if oid != tree[name] or kind != "blob":
            raise RuntimeError(f"Unexpected Git object for {name}")
        data = batch.stdout.read(int(size))
        if len(data) != int(size) or batch.stdout.read(1) != b"\n":
            raise RuntimeError(f"Truncated Git object for {name}")
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        manifest.append({"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    batch.stdin.close()
    if batch.wait() != 0:
        raise RuntimeError("Git object export failed")
subprocess.run([sys.executable, str(destination / "scripts/verify-reference.py")], check=True)
record = {
    "createdUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "purpose": "ADE native/source reference for a future app; full parity is not claimed.",
    "developmentCommit": source_commit,
    "developmentHistoryIncluded": False,
    "excludedAuditImages": excluded,
    "fileCount": len(manifest),
    "bytes": sum(entry["bytes"] for entry in manifest),
    "files": manifest,
    "publicationStatus": "Prepared locally; this manifest does not assert a GitHub push."
}
(destination / "PUBLICATION.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
print(json.dumps({"destination": str(destination), "files": len(manifest),
                  "excludedImages": len(excluded), "developmentCommit": source_commit}))
