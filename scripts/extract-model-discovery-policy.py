#!/usr/bin/env python3
"""Verify/extract fixed model-ID policy from the byte-pinned Stagewise source.
No network, credentials, current-model recommendations, or TypeScript execution.
"""
import argparse, hashlib, json, re
from pathlib import Path
parser=argparse.ArgumentParser()
parser.add_argument('--reference',type=Path)
parser.add_argument('--check',action='store_true')
a=parser.parse_args();root=Path(__file__).resolve().parents[1]
reference=a.reference or root/'reference/stagewise'
files={name:reference/'apps/browser/src/shared'/name for name in ('available-models.ts','flagship-models.ts')}
expected={'available-models.ts':'058d4a595e0b26d3f6b4d5bf96d040e40ce8dca9632e68c489588550fdc3f86b','flagship-models.ts':'672885db4f2b449a8b5541d7e03fa59ef358307c8879cb2e64f0492a6b4d4aca'}
for name,path in files.items():
 if hashlib.sha256(path.read_bytes()).hexdigest()!=expected[name]:raise SystemExit('Pinned source differs: '+name)
text=files['available-models.ts'].read_text(encoding='utf-8')
segment=text[text.index('export const availableModels = ['):]
pairs=re.findall(r"officialProvider:\s*'([^']+)',\s*modelId:\s*'([^']+)'",segment)
if len(pairs)!=54 or len(pairs)!=len(re.findall('officialProvider:',segment)):raise SystemExit('Catalog structure changed; inspect before regenerating.')
catalog={}
for vendor,model in pairs:catalog.setdefault(vendor,[]).append(model.lower())
text=files['flagship-models.ts'].read_text(encoding='utf-8')
router=re.findall(r"'([^']+)'",text.split('const OPENROUTER_FLAGSHIP_MODELS = new Set<string>([')[1].split(']);',1)[0])
block=text.split('const VENDOR_FLAGSHIP_DISCOVERED_MODELS:')[1].split('// ── Catalog',1)[0]
vendors={v:re.findall(r"'([^']+)'",ids) for v,ids in re.findall(r'(\w+): new Set\(\[([^]]+)\]\)',block)}
if len(router)!=47 or len(vendors)!=2:raise SystemExit('Flagship structure changed; inspect before regenerating.')
data={'sourceCommit':'17a0123a0e896885465df36169457199d5225eb5','sourceHashes':{'reference/stagewise/apps/browser/src/shared/'+name:expected[name] for name in files},'openrouter':router,'vendorFlagships':vendors,'catalog':catalog}
outputs={root/'assets/models/pinned-discovery-policy.json':(json.dumps(data,indent=2)+'\n').encode(),root/'src/core/ModelDiscoveryPolicyData.h':('// Derived from pinned Stagewise source. AGPL-3.0; see assets/models/pinned-discovery-policy.json.\n#pragma once\nnamespace ade::discovery_data { inline constexpr char json[] = R"ADE('+json.dumps(data,separators=(',',':'))+')ADE"; }\n').encode()}
for path,content in outputs.items():
 if a.check:
  if path.read_bytes()!=content:raise SystemExit('Generated policy differs: '+str(path))
 else:path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(content)
print('Pinned policy verified: 54 catalog IDs, 47 OpenRouter IDs, 2 vendor sets.')
