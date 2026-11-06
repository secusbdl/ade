"""Preserve omitted desktop build/test tooling and the public update-server.

Reads exact blobs from the existing pinned upstream checkout. No upstream code
is executed. Existing curated reference files remain untouched. Reruns verify
existing bytes and refuse overwrites if a supplemental file was modified.
"""
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess

root = Path(__file__).resolve().parents[1]
original = json.loads((root / "reference/stagewise/REFERENCE_MANIFEST.json").read_text(encoding="utf-8"))
commit = original["upstream"]["commit"]
upstream = root / "upstream/stagewise"
destination = root / "reference/stagewise-supplement"
prefixes = ("apps/update-server/", "apps/browser/.storybook/", "apps/browser/etc/",
            "apps/browser/scripts/", "apps/browser/vite-plugins/")
root_notices = {"LICENSE", "THIRD-PARTY-NOTICES.md"}


def git(*args):
    return subprocess.check_output(["git", "-C", str(upstream), *args])


assert git("rev-parse", f"{commit}^{{commit}}").decode().strip() == commit
selected = []
for record in filter(None, git("ls-tree", "-r", "-z", commit).split(b"\0")):
    metadata, raw_name = record.split(b"\t", 1)
    mode, kind, oid = metadata.decode().split()
    name = raw_name.decode()
    relative = PurePosixPath(name)
    if not (name in root_notices or name.startswith(prefixes)
            or (relative.parent.as_posix() == "apps/browser")):
        continue
    if (mode not in ("100644", "100755") or kind != "blob" or relative.is_absolute()
            or ".." in relative.parts or "\\" in name or ":" in name):
        raise SystemExit(f"Unreviewed reference entry: {name}")
    selected.append((name, oid))

files = []
with subprocess.Popen(["git", "-C", str(upstream), "cat-file", "--batch"],
                      stdin=subprocess.PIPE, stdout=subprocess.PIPE) as batch:
    for name, oid in selected:
        batch.stdin.write((oid + "\n").encode())
        batch.stdin.flush()
        actual_oid, kind, length = batch.stdout.readline().decode().split()
        assert actual_oid == oid and kind == "blob"
        data = batch.stdout.read(int(length))
        assert len(data) == int(length) and batch.stdout.read(1) == b"\n"
        # The explicit selection is source/configuration only; catch accidental
        # additions of media or binaries before they enter the curated snapshot.
        data.decode("utf-8")
        assert b"\0" not in data, name
        target = destination / name
        assert target.resolve().is_relative_to(destination.resolve())
        if target.exists():
            assert target.read_bytes() == data, f"Modified preserved source: {name}"
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        files.append({"path": name, "gitBlob": oid, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)})
    batch.stdin.close()
    assert batch.wait() == 0

manifest = {
    "schemaVersion": 1,
    "purpose": "Supplemental exact source reference: public update server and omitted desktop build/storybook tooling. No native implementation or upstream build is asserted.",
    "upstream": original["upstream"],
    "contentsUnmodified": True,
    "byteSource": "Exact Git blobs from the pinned commit.",
    "totalFiles": len(files),
    "totalBytes": sum(f["bytes"] for f in files),
    "selection": {"prefixes": prefixes, "directDirectoryFiles": ["apps/browser"], "rootNotices": sorted(root_notices)},
    "limits": [
        "Original curated reference/stagewise is unchanged. This supplement supplies additional source, not a native updater or installer.",
        "No dependencies, icons, logos, fonts, sounds or other media are installed or copied by this script.",
        "Build scripts may refer to omitted assets and external credentials/services. This is not a standalone buildable upstream checkout.",
        "Account/billing server implementation was not found at this pin; this public update-server must not be confused with those hosted services."
    ],
    "files": files,
}
(destination / "REFERENCE_MANIFEST.json").write_bytes((json.dumps(manifest, indent=2) + "\n").encode("utf-8"))
print(json.dumps({"files": manifest["totalFiles"], "bytes": manifest["totalBytes"], "commit": commit}))
