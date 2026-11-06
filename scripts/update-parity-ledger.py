"""Join source-review references onto all 701 feature rows without claiming parity.

Run after the three domain reviews are saved. --verify checks reproducibility.
This does not execute native tests or silently upgrade historical test evidence.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--verify", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]


def read(name):
    return json.loads((root / name).read_text(encoding="utf-8"))


def canonical_hash(name):
    return hashlib.sha256((root / name).read_text(encoding="utf-8").replace("\r\n", "\n").encode()).hexdigest()


inventory = read("audit/upstream-feature-inventory.json")
features = inventory["features"]
ids = {row["id"] for row in features}
assert len(features) == len(ids) == 701, "The agreed feature inventory must remain intact."
source_index = read("audit/upstream-source-index.json")
by_id = {feature_id: [] for feature_id in ids}
reports = []
for domain in ("core", "ui", "integrations", "shortcuts"):
    filename = f"audit/parity-review-{domain}.json"
    report = read(filename)
    assert report["upstreamCommit"] == source_index["upstream"]["commit"], filename
    seen = set()
    for capability in report["capabilities"]:
        assert capability["id"] not in seen, (filename, capability["id"])
        seen.add(capability["id"])
        for feature_id in sorted(set(capability.get("inventoryIds", []))):
            assert feature_id in ids, f"Unknown baseline ID {feature_id} in {filename}"
            by_id[feature_id].append({"report": filename, "capability": capability["id"],
                                      "assessment": capability["status"]})
    reports.append({"path": filename, "sha256CanonicalLf": canonical_hash(filename),
                    "capabilities": len(seen)})

rows = []
for feature in features:
    refs = by_id[feature["id"]]
    rows.append({"id": feature["id"], "category": feature["category"], "title": feature["title"],
                 "reviewState": "domain_review_linked" if refs else "not_individually_reassessed",
                 "reviews": refs,
                 "nativeParity": "not_proven_for_complete_acceptance_criteria"})
result = {
    "schemaVersion": 1,
    "upstream": source_index["upstream"],
    "purpose": "Complete scope ledger for the renewed source audit; source-review links are not completion counts.",
    "fullNativeParityEstablished": False,
    "lineByLineImplementationProven": False,
    "semanticReviewIsExhaustive": False,
    "sourceInventory": {"path": "audit/upstream-source-index.json",
                         "sha256CanonicalLf": canonical_hash("audit/upstream-source-index.json"),
                         "counts": source_index["counts"]},
    "featureBaseline": {"path": "audit/upstream-feature-inventory.json",
                         "sha256CanonicalLf": canonical_hash("audit/upstream-feature-inventory.json"),
                         "rows": len(features)},
    "reviews": reports,
    "counts": dict(Counter(row["reviewState"] for row in rows)),
    "limits": [
        "All 701 baseline rows remain, including rows without an individual reassessment.",
        "Multiple rows can describe the same procedure, contract or user workflow; counts are not independent functionality or parity percentages.",
        "A linked capability review can still report absent, partial, unverified or limited behavior. Read the referenced assessment and gaps.",
        "The AST inventory identifies functions and lines; it does not map every source statement to tested C++ behavior.",
        "Review reports retain the baseline source findings. Subsequent changes and newly executed tests belong in the separate integration checkpoint report.",
    ],
    "features": rows,
}
output = json.dumps(result, indent=2) + "\n"
destination = root / "audit/parity-review-coverage.json"
if args.verify:
    assert destination.read_text(encoding="utf-8") == output, "Parity ledger is stale."
else:
    destination.write_text(output, encoding="utf-8")
print(json.dumps({"verified": args.verify, "rows": len(features), **result["counts"], "fullNativeParityEstablished": False}))
