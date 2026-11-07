#!/usr/bin/env python3
"""Extract only context metadata from the pinned reference; no network/TS execution."""
import argparse
import hashlib
import json
import re
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--reference', type=Path)
parser.add_argument('--output-root', type=Path)
parser.add_argument('--check', action='store_true')
args = parser.parse_args()
root = args.output_root or Path(__file__).resolve().parents[1]
reference = args.reference or root / 'reference/stagewise'
relative = 'apps/browser/src/shared/available-models.ts'
source = reference / relative
expected = '058d4a595e0b26d3f6b4d5bf96d040e40ce8dca9632e68c489588550fdc3f86b'
if hashlib.sha256(source.read_bytes()).hexdigest() != expected:
    raise SystemExit('Pinned context source differs; inspect before regenerating.')
text = source.read_text(encoding='utf-8')
catalog_text = text.split('export const availableModels = [', 1)[1].split('export const availableModelAliases', 1)[0]
entries = re.findall(r"modelId:\s*'([^']+)'[\s\S]*?modelContextRaw:\s*([\d_]+)", catalog_text)
if len(entries) != 54 or len(set(model for model, _ in entries)) != 54:
    raise SystemExit('Pinned catalog structure changed.')
catalog = {model: int(value.replace('_', '')) for model, value in entries}
aliases = dict(re.findall(r"modelId:\s*'([^']+)',\s*targetModelId:\s*'([^']+)'", text))
if len(aliases) != 3 or any(target not in catalog for target in aliases.values()):
    raise SystemExit('Pinned alias structure changed.')
data = {'sourceCommit': '17a0123a0e896885465df36169457199d5225eb5',
        'sourcePath': 'reference/stagewise/' + relative, 'sourceSha256': expected,
        'unit': 'context window tokens as declared in pinned source; no current entitlement/spec guarantee',
        'models': catalog, 'aliases': aliases}
header = '// Derived from pinned Stagewise 1.31.1, AGPL-3.0. See assets/models/pinned-context-policy.json.\n'
header += 'struct ContextEntry { const char* id; int tokens; };\nstatic constexpr ContextEntry ContextEntries[]{\n'
header += ''.join('    {"' + model + '", ' + str(tokens) + '},\n' for model, tokens in catalog.items())
header += '};\nstruct ContextAlias { const char* id; const char* target; };\nstatic constexpr ContextAlias ContextAliases[]{\n'
header += ''.join('    {"' + model + '", "' + target + '"},\n' for model, target in aliases.items())
header += '};\n'
outputs = {root / 'assets/models/pinned-context-policy.json': json.dumps(data, indent=2) + '\n',
           root / 'src/attachments/ModelContextEntries.inc': header}
for path, value in outputs.items():
    content = value.encode('utf-8')
    if args.check:
        if path.read_bytes() != content:
            raise SystemExit('Generated context data differs: ' + str(path))
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
print('Pinned context policy verified: 54 model IDs, 3 aliases.')
