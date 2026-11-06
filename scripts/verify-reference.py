"""Verify the preserved source bytes without executing or installing upstream code."""
import hashlib
import json
from pathlib import Path, PurePosixPath


def verify_manifest(directory, manifest_name):
    manifest = json.loads((directory / manifest_name).read_text(encoding="utf-8"))
    errors, seen, total_bytes = [], set(), 0
    for entry in manifest["files"]:
        name = entry["path"]
        relative = PurePosixPath(name)
        if relative.is_absolute() or ".." in relative.parts or "\\" in name or ":" in name:
            errors.append(f"Invalid manifest path: {name}")
            continue
        if name in seen:
            errors.append(f"Duplicate manifest path: {name}")
            continue
        seen.add(name)
        path = directory / name
        if not path.resolve().is_relative_to(directory.resolve()) or not path.is_file():
            errors.append(f"Missing or escaped reference file: {name}")
            continue
        content = path.read_bytes()
        total_bytes += len(content)
        if hashlib.sha256(content).hexdigest() != entry["sha256"]:
            errors.append(f"Hash mismatch: {name}")
        if "bytes" in entry and len(content) != entry["bytes"]:
            errors.append(f"Size mismatch: {name}")
    if "totalFiles" in manifest and len(seen) != manifest["totalFiles"]:
        errors.append("Manifest file count mismatch")
    if "totalBytes" in manifest and total_bytes != manifest["totalBytes"]:
        errors.append("Manifest total byte count mismatch")
    # A valid subset is not sufficient: every preserved file must be declared.
    # The manifest itself is the only intentionally unhashed file in its tree.
    allowed = seen | {manifest_name}
    for path in sorted(directory.rglob("*")):
        if path.is_file() and path.relative_to(directory).as_posix() not in allowed:
            errors.append(f"Unlisted reference file: {path.relative_to(directory).as_posix()}")
    return {"files": len(seen), "bytes": total_bytes, "errors": errors}


root = Path(__file__).resolve().parents[1]
results = {"stagewise": verify_manifest(root / "reference/stagewise", "REFERENCE_MANIFEST.json")}
supplement = root / "reference/stagewise-supplement"
if supplement.exists():
    results["stagewise-supplement"] = verify_manifest(supplement, "REFERENCE_MANIFEST.json")
for experiment in ("file-mutations", "git-usage"):
    results[experiment] = verify_manifest(root / "reference/native-experiments" / experiment,
                                          "EXPERIMENT_MANIFEST.json")
print(json.dumps(results, indent=2))
raise SystemExit(1 if any(result["errors"] for result in results.values()) else 0)
