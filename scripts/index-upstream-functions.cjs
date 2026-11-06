#!/usr/bin/env node
/* Development-only audit tooling. Never loaded by ADE or its sandbox worker.
 * npm install --prefix private/parity-tools --no-save --ignore-scripts typescript@5.9.3
 * node scripts/index-upstream-functions.cjs [--verify] [--self-test]
 */
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { execFileSync } = require('node:child_process');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');
const ts = require(path.join(root, 'private/parity-tools/node_modules/typescript'));
assert.equal(ts.version, '5.9.3', 'Use the parser version resolved in the pinned upstream lockfile.');
const digest = (bytes) => crypto.createHash('sha256').update(bytes).digest('hex');
const scriptExtensions = new Set(['.ts', '.tsx', '.js', '.jsx', '.mjs', '.cjs', '.mts', '.cts']);
const textExtensions = new Set([...scriptExtensions, '.md', '.mdx', '.json', '.jsonc', '.css', '.html', '.yml', '.yaml', '.sql', '.toml', '.sh', '.ps1', '.txt', '.xml', '.svg', '.gitignore', '.gitattributes', '.npmrc', '.nvmrc']);

function indexFunctions(name, text) {
  const source = ts.createSourceFile(name, text, ts.ScriptTarget.Latest, true);
  const functions = [];
  function label(node) {
    if (node.name) return node.name.getText(source);
    if (ts.isConstructorDeclaration(node)) return 'constructor';
    const parent = node.parent;
    if (parent && (ts.isVariableDeclaration(parent) || ts.isPropertyAssignment(parent) || ts.isPropertyDeclaration(parent)))
      return parent.name.getText(source);
    if (parent && ts.isBinaryExpression(parent) && parent.right === node)
      return parent.left.getText(source);
    if (parent && ts.isCallExpression(parent)) {
      const index = parent.arguments.indexOf(node);
      if (index >= 0) return `${parent.expression.getText(source).slice(0, 100)} argument ${index + 1}`;
    }
    return `<anonymous@${source.getLineAndCharacterOfPosition(node.getStart(source)).line + 1}>`;
  }
  function visit(node, scopes) {
    const isFunction = ts.isFunctionDeclaration(node) || ts.isFunctionExpression(node)
      || ts.isArrowFunction(node) || ts.isMethodDeclaration(node)
      || ts.isConstructorDeclaration(node) || ts.isGetAccessorDeclaration(node)
      || ts.isSetAccessorDeclaration(node);
    let nextScopes = scopes;
    if (isFunction && node.body) {
      const start = node.getStart(source), end = node.end;
      const first = source.getLineAndCharacterOfPosition(start);
      const last = source.getLineAndCharacterOfPosition(Math.max(start, end - 1));
      const ownName = label(node);
      const qualifiedName = [...scopes, ownName].join(' > ');
      functions.push({
        id: `${name}:${first.line + 1}:${first.character + 1}`,
        path: name, kind: ts.SyntaxKind[node.kind], name: qualifiedName,
        startLine: first.line + 1, startColumn: first.character + 1,
        endLine: last.line + 1, endColumn: last.character + 1,
        sourceSha256: digest(Buffer.from(text.slice(start, end))),
        nativeParity: 'not_established_by_static_index',
      });
      nextScopes = [...scopes, ownName];
    } else if ((ts.isClassDeclaration(node) || ts.isClassExpression(node) || ts.isModuleDeclaration(node)) && node.name) {
      nextScopes = [...scopes, node.name.getText(source)];
    }
    ts.forEachChild(node, child => visit(child, nextScopes));
  }
  visit(source, []);
  return { functions, diagnostics: source.parseDiagnostics.map(d => ({
    line: source.getLineAndCharacterOfPosition(d.start || 0).line + 1,
    code: d.code, message: ts.flattenDiagnosticMessageText(d.messageText, '\n'),
  })) };
}

if (process.argv.includes('--self-test')) {
  const fixture = 'declare function overload(x: string): void;\r\n'
    + 'export function outer() { const f = () => 2; return [1].map(x => f() + x); }\r\n'
    + 'class C { constructor() {} get n() { return 1; } set n(x:number) {} m() {} }\r\n'
    + 'const view = () => <button onClick={() => {}}>Hi</button>;\r\n';
  const result = indexFunctions('fixture.tsx', fixture);
  assert.equal(result.diagnostics.length, 0);
  assert.equal(result.functions.length, 9);
  assert.equal(result.functions[0].name, 'outer');
  assert.equal(result.functions[0].startLine, 2);
  assert.equal(result.functions[0].endLine, 2);
  assert(result.functions.some(f => f.name === 'outer > f'));
  assert(result.functions.some(f => f.name === 'C > constructor'));
  assert(!result.functions.some(f => f.name === 'overload'));
  assert.equal(new Set(result.functions.map(f => f.id)).size, 9);
  assert.equal(result.functions[0].sourceSha256, digest(Buffer.from(fixture.split('\r\n')[1])));
  assert(indexFunctions('invalid.ts', 'function broken( {').diagnostics.length > 0);
  console.log('AST index regression checks passed (declarations, nesting, TSX, CRLF, spans, hashes, syntax errors).');
  process.exit(0);
}

const inventory = JSON.parse(fs.readFileSync(path.join(root, 'audit/upstream-feature-inventory.json'), 'utf8'));
const reference = JSON.parse(fs.readFileSync(path.join(root, 'reference/stagewise/REFERENCE_MANIFEST.json'), 'utf8'));
const commit = reference.upstream.commit;
const upstream = path.join(root, 'upstream/stagewise');
const git = (...args) => execFileSync('git', ['-C', upstream, ...args], { maxBuffer: 512 * 1024 * 1024 });
assert.equal(git('rev-parse', `${commit}^{commit}`).toString().trim(), commit);
assert.equal(inventory.features.length, 701, 'Do not silently shrink the agreed feature scope.');
const desktopPaths = new Set(inventory.source_manifest.map(f => f.path));
const preservedPaths = new Set(reference.files.map(f => f.path));
const supplementalPath = path.join(root, 'reference/stagewise-supplement/REFERENCE_MANIFEST.json');
const supplemental = fs.existsSync(supplementalPath) ? JSON.parse(fs.readFileSync(supplementalPath, 'utf8')) : null;
if (supplemental) assert.equal(supplemental.upstream.commit, commit);
const supplementalPaths = new Set(supplemental ? supplemental.files.map(f => f.path) : []);
const entries = git('ls-tree', '-r', '-z', commit).toString().split('\0').filter(Boolean).map(record => {
  const tab = record.indexOf('\t');
  const [mode, kind, oid] = record.slice(0, tab).split(' ');
  assert.equal(kind, 'blob', `Non-blob upstream entry needs explicit handling: ${record}`);
  return { mode, oid, name: record.slice(tab + 1) };
});
const batch = execFileSync('git', ['-C', upstream, 'cat-file', '--batch'], {
  input: entries.map(f => f.oid + '\n').join(''), maxBuffer: 512 * 1024 * 1024,
});
let offset = 0;
const files = [], functions = [], diagnostics = [];
for (const entry of entries) {
  const headerEnd = batch.indexOf(10, offset);
  assert(headerEnd >= offset, 'Truncated Git batch header');
  const [oid, kind, sizeText] = batch.subarray(offset, headerEnd).toString().split(' ');
  assert.equal(oid, entry.oid); assert.equal(kind, 'blob');
  const size = Number(sizeText);
  assert(Number.isSafeInteger(size) && size >= 0);
  const bytes = batch.subarray(headerEnd + 1, headerEnd + 1 + size);
  assert.equal(bytes.length, size);
  offset = headerEnd + 1 + size;
  assert.equal(batch[offset++], 10);
  const ext = path.posix.extname(entry.name).toLowerCase();
  const utf8 = bytes.toString('utf8');
  const isText = (textExtensions.has(ext) || !ext) && !bytes.includes(0) && Buffer.from(utf8).equals(bytes);
  let indexed = { functions: [], diagnostics: [] };
  if (scriptExtensions.has(ext)) {
    assert(isText, `Script is not UTF-8 text: ${entry.name}`);
    indexed = indexFunctions(entry.name, utf8);
    functions.push(...indexed.functions);
    diagnostics.push(...indexed.diagnostics.map(d => ({ path: entry.name, ...d })));
  }
  files.push({ path: entry.name, gitBlob: oid, mode: entry.mode, sha256: digest(bytes), bytes: size,
    lineCount: isText ? (utf8.length ? utf8.split('\n').length - (utf8.endsWith('\n') ? 1 : 0) : 0) : null,
    kind: isText ? 'text' : 'binary_or_unclassified',
    inOriginalDesktopManifest: desktopPaths.has(entry.name),
    preservation: preservedPaths.has(entry.name) ? 'curated_reference'
      : supplementalPaths.has(entry.name) ? 'supplemental_reference' : 'pinned_upstream_checkout_only',
    parser: scriptExtensions.has(ext) ? 'typescript_ast' : null,
    functions: indexed.functions.length, parseDiagnostics: indexed.diagnostics.length,
    semanticReview: 'not_established_by_static_index',
  });
}
assert.equal(offset, batch.length, 'Unconsumed Git batch output');
assert.equal(new Set(functions.map(f => f.id)).size, functions.length);
const indexText = functions.map(f => JSON.stringify(f)).join('\n') + '\n';
const manifest = {
  schemaVersion: 1, upstream: reference.upstream, parser: { name: 'typescript', version: ts.version },
  method: 'All tracked Git blobs at the pinned commit; AST functions with bodies, including nested callbacks. No working-tree line-ending conversion.',
  limits: [
    'This is a complete file inventory and static TS/JS function index, not evidence that each function or line has been semantically reviewed or ported.',
    'Function bodies exclude overload/type/interface signatures. Runtime-generated functions and non-TS/JS languages are not AST indexed.',
    'Counts include production code, test bodies, stories, build tools, callbacks and decorative components; functions are not independent user-visible features.',
    'Function spans overlap for nesting. Line counts cannot be added to infer behavior coverage.',
    'The 701 feature rows remain the agreed desktop scope. Extra website, documentation, tooling, restricted assets and binary files are inventoried without claiming they belong in the native product.',
    'Source preservation, native implementation, native test evidence and live upstream comparison are separate claims.',
  ],
  counts: { trackedFiles: files.length, textFiles: files.filter(f => f.kind === 'text').length,
    astParsedFiles: files.filter(f => f.parser).length, functionsWithBodies: functions.length,
    parseDiagnostics: diagnostics.length, originalDesktopManifestFiles: files.filter(f => f.inOriginalDesktopManifest).length,
    originalFeatureRows: inventory.features.length,
    curatedReferenceFiles: files.filter(f => f.preservation === 'curated_reference').length,
    supplementalReferenceFiles: files.filter(f => f.preservation === 'supplemental_reference').length },
  functionIndex: { path: 'upstream-function-index.jsonl', sha256: digest(Buffer.from(indexText)), records: functions.length },
  parseDiagnostics: diagnostics, files,
};
for (const [name, content] of [['upstream-source-index.json', JSON.stringify(manifest, null, 2) + '\n'],
                              ['upstream-function-index.jsonl', indexText]]) {
  const destination = path.join(root, 'audit', name);
  if (process.argv.includes('--verify')) assert.equal(fs.readFileSync(destination, 'utf8'), content, `Out-of-date index: ${name}`);
  else fs.writeFileSync(destination, content);
}
console.log(JSON.stringify({ verified: process.argv.includes('--verify'), ...manifest.counts }));
