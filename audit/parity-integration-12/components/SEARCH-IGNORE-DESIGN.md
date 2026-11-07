# Native search ignore/filter design: glob and grepSearch vs pinned Stagewise

> **Status: DESIGN ONLY. All code is UNCOMPILED and all acceptance cases are UNTESTED.**
> This is a source review of the isolated snapshot only. Nothing was built, run or edited.
> The snapshot's `src/core/ToolExecutor.cpp` predates root's search fixes, which are deliberately not repeated here:
> - nested grep basename matching
> - honest skipped-file diagnostics
> - UTF-16 decoding
> - 500-unit previews
> - true 8 MiB read accounting
>
> Line references are to this snapshot. Root should graft the integration onto its current executor.

## 0. Correction to earlier browser reports
`BROWSER-TOOLS-REVIEW.md` (B4) and `BROWSER-EVENT-REPAIR.md` (D2, R6) predicted from source alone that an agent's `Runtime.disable`/`Log.disable` would stop console capture for the rest of a tab's life.
- **Not reproduced:** root's run on the real WebView2 baseline still captured console output after `Runtime.disable` + `Log.disable` followed by navigation. D2 is refuted for this runtime. The queued `restoreConsole` step in that design is optional, and R6 is a guard, not a failing test.
- **Reproduced:** only D1, where an agent's unsubscribe detaches the inspector's shared receiver.
- **Root's fix:** the owner-scoped patch passes 53 checks, including child-owner shutdown.

## 1. Current native behaviour (snapshot)
- **Schemas** (`ToolExecutor::definitions`, `ToolExecutor.cpp:363-368`):
  - `glob` takes `pattern` and `mount_prefix`.
  - `grepSearch` takes `query`, `case_sensitive`, `include_file_pattern`, `max_matches` and `mount_prefix`.
  - Neither has `include_gitignored`, and `grepSearch` has no `exclude_file_pattern`. A model that sends either has it silently ignored.
- **Execution** (`ToolExecutor.cpp:563-592`):
  - One walk with a 10,000-entry budget and depth 64.
  - `.git`, `node_modules` and reparse points are always skipped (`574`). `node_modules` cannot be included at all.
  - No ignore files are read.
  - `globRegex` (`233-249`) supports only `*`, `**` and `?`. It has no classes, braces or escapes, anchors to the whole relative path (snapshot), and is case-sensitive.
  - Glob output is unbounded apart from 64 Ki characters and the entry budget. There is no 50-result count contract.
- **Budget** (`walkScopedDirectory`, `117-145`; `readScopedDirectory`, `ScopedDirectory.cpp:24-101`): every enumerated name counts toward the budget (`accept` at 34-39), including siblings that are never visited. Skipped directories are not opened (good), but skipped files still consume budget.
- **Reparse points:** directory links are never descended (`ScopedDirectory.cpp:42-48`; walker `137`). File reads follow links but verify the final path is inside the mount (`readBoundedFile`, `67-97`).

## 2. Upstream contract (pinned Stagewise)

**Schemas** (`packages/agent-core/src/types/tools.ts:107-178`)
- grep: `include_file_pattern`, `exclude_file_pattern`, `include_gitignored`, `max_matches` (default 15, max 50).
- glob: `pattern` (described as `*, **, ?, [abc]`) and `include_gitignored`. Glob has no exclude parameter.

**Plumbing** (`services/toolbox/universal-tools.ts`)
- glob (757-792): `respectGitignore: !include_gitignored`, `maxResults: 50`.
- grep (794-862): `filePattern: include`, `excludePatterns: [exclude]`, `respectGitignore`.

**Engines**
- **Primary:** ripgrep v15.0.0, installed at app start (`agent/runtime-node/src/vscode-ripgrep/ensure-ripgrep.ts:11`; `apps/browser/src/backend/main.ts:421`). The JavaScript fallback is the exception: it runs only when rg is missing.
- **glob primary** (`glob/glob-ripgrep.ts:22-51`): `rg --files --no-config --hidden -g '!.git' [-g <pattern> unless '**'] [-g '!<exclude>'] [--no-ignore]`. It stops (kills rg) at 50 results.
- **grep primary** (`grep/grep-ripgrep.ts:38-74`): `rg --json --no-config --no-ignore-global (-i|--case-sensitive) --max-count N [-g include] [--glob '!exclude'] [--no-ignore] -e query <root>`. There is **no `--hidden`**.
- **Fallbacks:** `glob/glob-node-fallback.ts`, `grep/grep-node-fallback.ts`, plus `ClientRuntimeNode.isIgnoredSync`/`ensureGitignoreInitialized` (`agent/runtime-node/src/index.ts:541-714`).

**Upstream tests** (`universal-tools.test.ts:176-284`; these exercise the fallback, because rg is not assumed)
- `node_modules` is skipped by default and returned with `include_gitignored: true`.
- A user `.gitignore` containing `secrets/` hides `secrets/leak.md`, and `include_gitignored: true` returns it.

## 3. Primary-vs-fallback contract differences (upstream)
| # | Topic | ripgrep primary | JS fallback | Native target |
|---|-------|-----------------|-------------|---------------|
| 1 | Built-in ignores | none except glob's `-g '!.git'` | 14 names at the root: `.git node_modules dist build .next out coverage .cache .turbo .vscode .idea *.log .DS_Store Thumbs.db` (`index.ts:595-610`), placed *after* user root rules so they beat user negations | `.git` always (policy). `node_modules` as a lowest-precedence built-in rule, keeping today's native behaviour and the upstream test. **No** other fallback defaults (they would hide real `build/`/`out/` sources) |
| 2 | Requires a git repo | `.gitignore` applies only inside a git repo (rg default; `.ignore`/`.rgignore` always) | always | always (equivalent to `--no-require-git`); documented divergence. Checking for an ancestor `.git` outside the mount is not allowed |
| 3 | Rule sources | `.gitignore`, `.ignore`, `.rgignore`, parent-directory ignore files above the mount, `.git/info/exclude`; global excludes for glob only (grep passes `--no-ignore-global`) | `.gitignore` files under the mount only | `.gitignore` under the mount only (Phase 1). `.ignore`/`.rgignore` optional in Phase 2. **Never** parent, global or `.git/info/exclude` (policy) |
| 4 | Nested precedence | git-like: the deepest matching file wins, last matching line wins, a deeper `!` can re-include; precedence by file type across depths (`.rgignore` > `.ignore` > `.gitignore`) | any level that ignores wins (`index.ts:565-570`); negation only within one file | git-like, deepest wins |
| 5 | Line parsing | git: comments, `\#`, `\!`, unescaped trailing spaces dropped, leading spaces significant, CRLF tolerated | `trim()` on both ends and `#` lines dropped (`index.ts:649-652`), losing escaped trailing and leading spaces | git rules |
| 6 | Pattern language | gitignore-style globs for `-g` and ignore lines: no `/` matches the basename at any depth, `/` anchors, `*` matches dotfiles, `{a,b}`, classes | minimatch defaults: `*.ts` root-only, `dot:false`; glob descends only when the pattern contains `**` (`glob-node-fallback.ts:65`); grep include matches the basename only (`grep-node-fallback.ts:129-133`) | gitignore-style everywhere (one compiler) |
| 7 | Hidden files | glob includes them (`--hidden`); **grep skips hidden files and directories** unless whitelisted | glob `*` skips dotfiles (minimatch); grep includes them | glob includes them. grep: **owner decision** (§6 D-H) |
| 8 | Include vs ignore | `-g` overrides "always override" ignore logic: an include glob re-includes gitignored or hidden *files* inside traversed directories; unmatched files are dropped; directories are not whitelisted unless matched | ignore checked first; include cannot re-include | rg semantics (needs RG-REF confirmation, §8) |
| 9 | Exclude | `!glob` override; a matching directory is pruned; the last matching glob wins (exclude comes after include) | minimatch on the relative path of each entry | rg semantics, plus exact pruning of `<prefix>/**` |
| 10 | `include_gitignored` | `--no-ignore` (grep still skips hidden and `.git`; glob still `-g '!.git'`) | turns off **all** checks, so the walk can enter `.git` (`glob-node-fallback.ts:51-55`) | turns off ignore files and built-in `node_modules`; `.git` and reparse points stay hidden |
| 11 | Count and order | glob stops at 50 with non-deterministic order (parallel walker); `totalMatches ≤ 50`; no truncation signal | sequential order, stops at `maxResults` | deterministic order; 50 shown; a 51st match found means `truncated` is reported honestly |
| 12 | Links | not followed | `Dirent` symlinks neither files nor directories | never traversed or listed (policy) |

## 4. Design: one small native module
New `src/core/SearchRules.{h,cpp}` with no file I/O, so it can be unit-tested as pure tables. A secure rule-file loader sits beside `readBoundedFile` in `ToolExecutor.cpp`, reusing its `pathFromHandle`/`within` helpers. Small extensions go into `readScopedDirectory` and `walkScopedDirectory`.

### 4.1 Glob compiler (one language for ignore lines, `include_file_pattern`, `exclude_file_pattern` and the glob `pattern`)
- **Separators and anchoring:**
  - `/` is the only separator, and `\` escapes the next character (also on Windows, as in git and rg).
  - A leading `/` anchors the pattern. A `/` at the start or in the middle anchors it to the rule base (the ignore file's directory, or the mount root for tool patterns).
  - With no unescaped `/`, the pattern is treated as `**/<pattern>`, so it matches the basename at any depth. A trailing `/` means directory-only and does not count toward anchoring.
- **Wildcards:**
  - `*` → `[^/]*` (matches leading dots); `?` → `[^/]`.
  - `**` as a whole segment: `**/` → `(?:.*/)?`; a trailing `/**` → `/.*` (contents only, never the directory itself); `**` alone → `.*`.
  - Any other `**` (`a**b`) is treated as `*` (git) [RG-REF].
- **Classes:** `[abc]`, `[a-z]`, `[!x]`/`[^x]`, `]` first means a literal; never matches `/`. An unclosed `[` is an error [RG-REF].
- **Alternation:** `{a,b}` → `(?:a|b)`, nesting up to 8 levels, as in rg globsets. Git treats `{` literally [RG-REF for ignore files].
- **Case:** case-sensitive on every platform, as rg does by default. Git on Windows with `core.ignorecase` differs [RG-REF].
- **Errors:** pattern length ≤ 1024 and no NUL. A trailing lone `\`, an unclosed class or brace, or an invalid generated regex are errors.

```cpp
// SearchRules.h (UNCOMPILED)
namespace ade::search {
enum class RuleMatch { None, Ignore, Whitelist, Unmatched };
struct GlobRule {
    QRegularExpression regex;              // anchored ^...$ over '/'-separated paths relative to the rule base
    bool negated = false, directoryOnly = false;
    int line = 0;                          // 1-based line in an ignore file; 0 for tool patterns
    std::optional<QRegularExpression> prunePrefix;   // set for "<prefix>/**": every descendant of a matching dir matches
};
enum class GlobSyntax { IgnoreFileLine, ToolPattern };
// Returns nullopt with an empty *error for blank/comment lines, non-empty *error for malformed ones.
std::optional<GlobRule> compileRule(QString text, GlobSyntax syntax, int line, QString* error);

struct RuleSet {                           // one ignore file
    QString base;                          // mount-relative directory ("" = mount root)
    QString file;                          // mount-relative path, for diagnostics
    QList<GlobRule> rules;                 // file order; last match wins
    RuleMatch match(const QString& path, bool isDirectory) const;
};
class IgnoreStack {                        // rule sets of the current directory chain only
public:
    void push(RuleSet set) { m_sets.append(std::move(set)); }
    void pop() { m_sets.removeLast(); }
    RuleMatch decide(const QString& path, const QString& name, bool isDirectory) const;
private:
    QList<RuleSet> m_sets;
};
struct OverrideSet {                       // tool patterns in rg command-line order
    QList<GlobRule> rules;                 // non-negated = whitelist, '!'-negated = exclude
    bool whitelists = false;
    RuleMatch match(const QString& path, bool isDirectory) const;
    bool prunes(const QString& directory) const;
};
enum class Visit { Prune, Consider, Select };   // Prune: drop before budget; Consider: count, no output
struct SearchPolicy { bool respectIgnores = true; bool skipHidden = false; OverrideSet overrides; };
Visit decide(const SearchPolicy& policy, const IgnoreStack& stack, const QString& name,
             const QString& path, bool isDirectory, bool isReparsePoint);
}
```

```cpp
// SearchRules.cpp: core pieces (UNCOMPILED)
namespace ade::search {
namespace {
bool escapedAt(QStringView text, qsizetype index) {
    qsizetype slashes = 0;
    for (qsizetype k = index - 1; k >= 0 && text[k] == u'\\'; --k) ++slashes;
    return slashes % 2 == 1;
}
bool hasUnescapedSlash(QStringView text) {
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i] == u'\\') { ++i; continue; }
        if (text[i] == u'/') return true;
    }
    return false;
}
bool translate(QStringView glob, QString* rx, QString* error, int depth) {
    if (depth > 8) { *error = QStringLiteral("alternation nested too deeply"); return false; }
    for (qsizetype i = 0; i < glob.size(); ++i) {
        const QChar c = glob[i];
        const bool segmentStart = i == 0 || glob[i - 1] == u'/';
        if (c == u'\\') {
            if (i + 1 >= glob.size()) { *error = QStringLiteral("trailing backslash"); return false; }
            *rx += QRegularExpression::escape(QString(glob[++i])); continue;
        }
        if (c == u'*') {
            const bool doubled = i + 1 < glob.size() && glob[i + 1] == u'*';
            if (doubled && segmentStart && (i + 2 == glob.size() || glob[i + 2] == u'/')) {
                if (i + 2 == glob.size()) { *rx += QStringLiteral(".*"); i += 1; }
                else { *rx += QStringLiteral("(?:.*/)?"); i += 2; }
                continue;
            }
            *rx += QStringLiteral("[^/]*");
            while (i + 1 < glob.size() && glob[i + 1] == u'*') ++i;   // "a**b" == "a*b" [RG-REF]
            continue;
        }
        if (c == u'?') { *rx += QStringLiteral("[^/]"); continue; }
        if (c == u'[') {
            qsizetype j = i + 1; bool negate = false, first = true, closed = false; QString set;
            if (j < glob.size() && (glob[j] == u'!' || glob[j] == u'^')) { negate = true; ++j; }
            for (; j < glob.size(); ++j) {
                QChar k = glob[j];
                if (k == u']' && !first) { closed = true; break; }
                first = false;
                if (k == u'-' && !set.isEmpty() && j + 1 < glob.size() && glob[j + 1] != u']') { set += u'-'; continue; }
                if (k == u'\\') { if (j + 1 >= glob.size()) break; k = glob[++j]; }
                if (k == u'/') { *error = QStringLiteral("'/' inside a character class"); return false; }
                if (k == u'\\' || k == u']' || k == u'[' || k == u'^' || k == u'-') set += u'\\';
                set += k;
            }
            if (!closed) { *error = QStringLiteral("unclosed character class"); return false; }
            *rx += negate ? QStringLiteral("[^/") + set + u']' : u'[' + set + u']';
            i = j; continue;
        }
        if (c == u'{') {
            qsizetype j = i + 1, start = j; int nest = 0; QList<QStringView> parts;
            for (; j < glob.size(); ++j) {
                if (glob[j] == u'\\') { ++j; continue; }
                if (glob[j] == u'{') ++nest;
                else if (glob[j] == u'}') { if (!nest) break; --nest; }
                else if (glob[j] == u',' && !nest) { parts.append(glob.mid(start, j - start)); start = j + 1; }
            }
            if (j >= glob.size()) { *error = QStringLiteral("unclosed alternation"); return false; }
            parts.append(glob.mid(start, j - start));
            QStringList alternatives;
            for (const auto part : parts) { QString sub; if (!translate(part, &sub, error, depth + 1)) return false; alternatives.append(sub); }
            *rx += QStringLiteral("(?:") + alternatives.join(u'|') + u')';
            i = j; continue;
        }
        *rx += QRegularExpression::escape(QString(c));
    }
    return true;
}
std::optional<QRegularExpression> anchored(const QString& body, QString* error) {
    QString rx; if (!translate(body, &rx, error, 0)) return std::nullopt;
    QRegularExpression regex(u'^' + rx + u'$', QRegularExpression::DontCaptureOption);
    if (!regex.isValid()) { *error = regex.errorString(); return std::nullopt; }
    regex.optimize(); return regex;
}
} // namespace

std::optional<GlobRule> compileRule(QString text, GlobSyntax syntax, int line, QString* error) {
    error->clear();
    if (text.size() > 1024 || text.contains(QChar::Null)) { *error = QStringLiteral("pattern too long or contains NUL"); return std::nullopt; }
    if (syntax == GlobSyntax::IgnoreFileLine) {
        if (text.endsWith(u'\r')) text.chop(1);
        if (text.isEmpty() || text.startsWith(u'#')) return std::nullopt;            // blank or comment
        while (text.endsWith(u' ') && !escapedAt(text, text.size() - 1)) text.chop(1);  // git: unescaped trailing spaces
    }
    GlobRule rule; rule.line = line;
    if (text.startsWith(u'!')) { rule.negated = true; text.remove(0, 1); }
    if (text.endsWith(u'/') && !escapedAt(text, text.size() - 1)) { rule.directoryOnly = true; text.chop(1); }
    if (text.isEmpty()) return std::nullopt;                                         // "!", "/" alone
    bool isAnchored = text.startsWith(u'/');
    if (isAnchored) text.remove(0, 1); else isAnchored = hasUnescapedSlash(text);
    if (text.isEmpty()) return std::nullopt;
    const QString body = isAnchored ? text : QStringLiteral("**/") + text;
    auto regex = anchored(body, error); if (!regex) return std::nullopt;
    rule.regex = *regex;
    if (body.endsWith(QStringLiteral("/**")) && !escapedAt(body, body.size() - 3)) {
        QString ignored; rule.prunePrefix = anchored(body.chopped(3), &ignored);
    }
    return rule;
}

RuleMatch RuleSet::match(const QString& path, bool isDirectory) const {
    if (!base.isEmpty() && !path.startsWith(base + u'/')) return RuleMatch::None;
    const QString relative = base.isEmpty() ? path : path.mid(base.size() + 1);
    for (auto it = rules.crbegin(); it != rules.crend(); ++it) {
        if (it->directoryOnly && !isDirectory) continue;
        if (it->regex.match(relative).hasMatch()) return it->negated ? RuleMatch::Whitelist : RuleMatch::Ignore;
    }
    return RuleMatch::None;
}
RuleMatch IgnoreStack::decide(const QString& path, const QString& name, bool isDirectory) const {
    for (auto it = m_sets.crbegin(); it != m_sets.crend(); ++it)              // deepest file first
        if (const auto m = it->match(path, isDirectory); m != RuleMatch::None) return m;
    if (isDirectory && name == QStringLiteral("node_modules")) return RuleMatch::Ignore;   // native built-in, lowest precedence
    return RuleMatch::None;
}
RuleMatch OverrideSet::match(const QString& path, bool isDirectory) const {
    for (auto it = rules.crbegin(); it != rules.crend(); ++it) {                 // last glob wins (rg)
        if (it->directoryOnly && !isDirectory) continue;
        if (it->regex.match(path).hasMatch()) return it->negated ? RuleMatch::Ignore : RuleMatch::Whitelist;
    }
    return whitelists && !isDirectory ? RuleMatch::Unmatched : RuleMatch::None;
}
bool OverrideSet::prunes(const QString& directory) const {
    for (auto it = rules.crbegin(); it != rules.crend(); ++it) {
        if (!it->negated) return false;                                          // a later whitelist could re-include
        if (it->prunePrefix && it->prunePrefix->match(directory).hasMatch()) return true;
    }
    return false;
}
Visit decide(const SearchPolicy& p, const IgnoreStack& stack, const QString& name,
             const QString& path, bool isDirectory, bool isReparsePoint) {
    if (isReparsePoint || name == QStringLiteral(".git")) return Visit::Prune;   // native policy precedes every override
    if (isDirectory && p.overrides.prunes(path)) return Visit::Prune;            // exact: all descendants excluded
    const auto o = p.overrides.match(path, isDirectory);
    if (o == RuleMatch::Ignore) return Visit::Prune;
    if (o == RuleMatch::Whitelist) return Visit::Select;                         // rg: overrides beat ignore files/hidden [RG-REF]
    if (p.respectIgnores) {
        const auto m = stack.decide(path, name, isDirectory);
        if (m == RuleMatch::Ignore) return Visit::Prune;
        if (m == RuleMatch::Whitelist) return o == RuleMatch::Unmatched ? Visit::Consider : Visit::Select;
    }
    if (p.skipHidden && name.startsWith(u'.')) return Visit::Prune;
    return o == RuleMatch::Unmatched ? Visit::Consider : Visit::Select;
}
}
```

**Building the overrides:**
- glob: `[pattern]`, except `pattern == "**"`, which adds no rule, like upstream (`glob-ripgrep.ts:40`).
- grep: `[include]` if non-empty, then `["!" + exclude]` if non-empty, compiled with `GlobSyntax::ToolPattern`.
- `whitelists` is true if any non-negated rule exists.
- An include starting with `!` becomes an exclusion, as in rg [RG-REF].

### 4.2 Secure rule-file loader (`ToolExecutor.cpp`, beside `readBoundedFile`)
- **Only** `<directory>/.gitignore` for directories the walk actually enters inside the mount, plus, for a search started below the mount root, each ancestor between the mount root and the start directory. Nothing outside the mount. Never `.git/info/exclude` or global excludes.
- **Opening:** opened with `FILE_FLAG_OPEN_REPARSE_POINT`. The file is **not applied**, with a diagnostic, when it is a link, not a regular file, or resolves outside the mount. A missing file means no rules and no diagnostic.
- **Limits:** at most 256 KiB, strict UTF-8 (a BOM is dropped, as git does), at most 10,000 lines. A file over these limits is not applied, with a diagnostic.
- **Bad lines:** a malformed line is skipped with a `line N` diagnostic; the rest of the file still applies.
- **Fail-open:** a rejected file contributes no rules, so matches it would have hidden can appear, and that is disclosed. Search over-inclusion is non-destructive, whereas fail-closed would silently hide whole trees.
- **Walk-wide caps:** 2,000 rule files and 100,000 rules. Beyond those, no further files are loaded, with a single diagnostic.
- **`include_gitignored = true`:** no rule file is opened.

```cpp
// ToolExecutor.cpp, anonymous namespace (UNCOMPILED)
enum class RuleFileStatus { Absent, Loaded, Rejected };
RuleFileStatus loadIgnoreFile(const QString& directory, const QString& relativeDirectory, const QString& root,
                              search::RuleSet* out, QStringList* diagnostics, const std::atomic_bool* cancelled) {
    constexpr qint64 MaxIgnoreBytes = 256 * 1024;
    const QString shown = (relativeDirectory.isEmpty() ? QString() : relativeDirectory + u'/') + QStringLiteral(".gitignore");
    const auto reject = [&](const QString& why) { diagnostics->append(shown + QStringLiteral(" not applied (") + why + u')'); return RuleFileStatus::Rejected; };
    QByteArray bytes;
#ifdef Q_OS_WIN
    const auto native = QDir::toNativeSeparators(directory + QStringLiteral("/.gitignore")).toStdWString();
    HANDLE handle = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return RuleFileStatus::Absent;
        return reject(QStringLiteral("unreadable, Windows error %1").arg(code));
    }
    const auto close = qScopeGuard([&] { CloseHandle(handle); });
    BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(handle, &info) || !GetFileSizeEx(handle, &size)) return reject(QStringLiteral("unreadable"));
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return reject(QStringLiteral("links are not followed"));
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return reject(QStringLiteral("not a regular file"));
    const auto resolved = pathFromHandle(handle);
    if (resolved.isEmpty() || !within(root, resolved)) return reject(QStringLiteral("outside the mounted workspace"));
    if (size.QuadPart > MaxIgnoreBytes) return reject(QStringLiteral("exceeds 256 KiB"));
    QByteArray chunk(65536, Qt::Uninitialized);
    for (;;) {
        if (cancelled && cancelled->load()) return reject(QStringLiteral("cancelled"));
        DWORD count = 0;
        if (!ReadFile(handle, chunk.data(), DWORD(chunk.size()), &count, nullptr)) return reject(QStringLiteral("unreadable"));
        if (!count) break;
        if (bytes.size() + count > MaxIgnoreBytes) return reject(QStringLiteral("grew beyond 256 KiB"));
        bytes.append(chunk.constData(), count);
    }
#else
    const QFileInfo info(directory + QStringLiteral("/.gitignore"));
    if (!info.exists() && !info.isSymLink()) return RuleFileStatus::Absent;
    if (info.isSymLink() || info.isJunction()) return reject(QStringLiteral("links are not followed"));
    if (!info.isFile()) return reject(QStringLiteral("not a regular file"));
    if (!within(root, info.canonicalFilePath())) return reject(QStringLiteral("outside the mounted workspace"));
    if (info.size() > MaxIgnoreBytes) return reject(QStringLiteral("exceeds 256 KiB"));
    QFile file(info.filePath()); if (!file.open(QIODevice::ReadOnly)) return reject(QStringLiteral("unreadable"));
    bytes = file.read(MaxIgnoreBytes + 1); if (bytes.size() > MaxIgnoreBytes) return reject(QStringLiteral("exceeds 256 KiB"));
#endif
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::ConvertInitialBom | QStringConverter::Flag::Stateless);
    const QString text = decoder(bytes);
    if (decoder.hasError()) return reject(QStringLiteral("not valid UTF-8"));
    const auto lines = text.split(u'\n');
    if (lines.size() > 10000) return reject(QStringLiteral("more than 10000 lines"));
    out->base = relativeDirectory; out->file = shown; out->rules.clear();
    for (qsizetype i = 0; i < lines.size(); ++i) {
        QString problem;
        if (auto rule = search::compileRule(lines[i], search::GlobSyntax::IgnoreFileLine, int(i + 1), &problem)) out->rules.append(std::move(*rule));
        else if (!problem.isEmpty()) diagnostics->append(shown + QStringLiteral(" line %1 skipped (").arg(i + 1) + problem + u')');
    }
    return RuleFileStatus::Loaded;
}
```

### 4.3 Pruning before the 10,000-entry budget
`readScopedDirectory` gains an optional admission filter and a walk-wide raw scan cap. Names dropped by the filter **do not count** toward `maxEntries`, but every name enumerated counts toward the scan cap, so time stays bounded.
```cpp
// ScopedDirectory.h (UNCOMPILED): add to ScopedDirectoryEntry
    int tag = 0;                       // caller-defined admission result
// new optional parameters (defaulted; existing callers unchanged)
struct ScopedAdmission {
    std::function<int(const ScopedDirectoryEntry&)> admit;   // <0 drop before counting; >=0 stored in entry.tag
    int* scanned = nullptr; int scanLimit = 0;               // raw names across the whole walk
    bool* scanLimitReached = nullptr;
};
bool readScopedDirectory(const QString& path, const QString& root, ScopedDirectorySnapshot* result, QString* error,
                         int maxEntries = 10000, const std::atomic_bool* cancelled = nullptr, bool allowRootAlias = true,
                         bool* truncated = nullptr, const ScopedAdmission* admission = nullptr);

// ScopedDirectory.cpp: inside `accept`, after name validation and before the maxEntries test
    if (admission && admission->scanned && ++*admission->scanned > admission->scanLimit) {
        if (admission->scanLimitReached) *admission->scanLimitReached = true;
        if (truncated) { *truncated = true; return 2; }
        return -2;
    }
    ScopedDirectoryEntry entry{name, snapshot.canonicalPath + '/' + name, directory, reparse};
    if (admission && admission->admit) { const int tag = admission->admit(entry); if (tag < 0) return 0; entry.tag = tag; }
    if (snapshot.entries.size() >= maxEntries) { if (truncated) { *truncated = true; return 2; } return -2; }
    snapshot.entries.append(std::move(entry)); return 0;
```
**Walker (search variant):** for each directory to open:
1. If `respectIgnores`, call `loadIgnoreFile` **before** enumerating, and push the rule set onto `IgnoreStack`. Pop it when the frame pops.
2. Enumerate with `admit = decide(...)`. `Prune` → −1, `Consider` → 0, `Select` → 1.
3. Sort the admitted entries ordinally by name for deterministic results.

Pruned directories are never opened, so their contents and their ignore files are never read. The truncation reason is reported as **entry budget 10000**, **scan limit 200000 names** or **depth 64**.

### 4.4 Executor integration (adapt to root's current executor)
**Schemas** (upstream wording, `tools.ts:126-143,172-177`):
```cpp
// glob
{"include_gitignored", prop("boolean", "If true, includes files from gitignored paths (e.g. node_modules, dist). Use with specific patterns to avoid noise. Defaults to false.")}
// grepSearch
{"exclude_file_pattern", prop("string", "Glob pattern for files to exclude. Examples: \"**/test-*.js\", \"metadata/**\".")},
{"include_gitignored", prop("boolean", "If true, includes files from gitignored paths (e.g. node_modules, dist). Use with specific patterns to avoid noise. Defaults to false.")}
// glob `pattern` description: add "[abc] and {a,b}; a pattern without '/' matches names at any depth".
```
**Validation:**
- A non-boolean `include_gitignored` or a non-string `exclude_file_pattern` is an error.
- An empty include or exclude is treated as absent, as upstream treats falsy values.
- A compile error is reported with its reason.
- `exclude_file_pattern` exists only on grep, as upstream.

**Selection:**
- glob: output `Select` files.
- grep: search `Select` files. `Consider` files are counted but neither output nor read.

**Count:** glob shows the first 50 in deterministic order and keeps walking until a 51st match proves truncation. It reports `Found 50+ files (showing 50)`, or `Found N files` when N ≤ 50. This is a deliberate improvement on upstream's silent cut at 50.

**Diagnostics:** up to 10 rule-file lines, then `and N more`. They are kept separate from root's skipped-file diagnostics.

## 5. Policy invariants (native, unconditional)
- `.git`, whether a directory or a gitfile, at any depth, is never listed, searched or entered. That holds even with `include_gitignored: true` and with patterns such as `**/*` or `.git/**`. rg's override could re-include it; native does not.
- Reparse points (junctions, symbolic links, other reparse tags) are never traversed or listed, and ignore files that are links are not applied.
- No ignore or config file outside the mounted workspace is read: no parent directories, no global excludes, no `.git/info/exclude`, no user rg config.

## 6. Decisions needing the owner or root
- **D-H (grep hidden files):** rg primary skips hidden files and directories in grep, and `--no-ignore` does not change that. Native grep searches them today.
  - Option A, strict parity: `skipHidden = true` for grep; a gitignore negation or include glob can still whitelist a hidden file.
  - Option B, keep recall: `skipHidden = false`, documented as a divergence.
  - The acceptance cases (H1/H2) cover both. Root decides; parity favours A.
- **D-R (include re-includes ignored files):** follow rg overrides (§4.1 `decide`). Confirm with RG-REF cases O1–O4 before locking.
- **D-I (`.ignore`/`.rgignore`):** Phase 2, only after RG-REF T1–T3 capture rg's type-over-depth precedence.

## 7. Acceptance cases
Every case uses its own temporary mount `ws/`. Expected values are mount-relative path sets.

**Tags:**
- **[GIT]:** behaviour documented by git and rg.
- **[POL]:** native policy.
- **[DIFF-FB]:** differs from the upstream JS fallback.
- **[RG-REF]:** lock only after a real ripgrep v15.0.0 comparison (§8).
- **[PARSER]:** a parser unit test, because Windows cannot create the file name involved.

Unless stated, calls are `glob {pattern:"**"}` and `grepSearch {query:"x"}`, every fixture file contains `x`, and `include_gitignored` is false.

### Schema and validation
| # | Case | Expected |
|---|------|----------|
| S1 | definitions | glob has `include_gitignored`; grepSearch has `exclude_file_pattern` and `include_gitignored`, with upstream descriptions |
| S2 | `include_gitignored:"yes"` | error naming the field; no walk |
| S3 | `exclude_file_pattern:7` | error naming the field |
| S4 | `exclude_file_pattern:""`, `include_file_pattern:""` | treated as absent |
| S5 | include `"[abc"` | error "unclosed character class"; no walk [RG-REF] |

### Scoped `.gitignore` semantics
| # | Rules | Tree | Expected (glob `**`) |
|---|-------|------|----------------------|
| I1 | root `*.log` | `a.log`, `x/y/b.log`, `a.log.txt` | `a.log.txt` [GIT] |
| I2 | root `/build` | `build/o.js`, `src/build/o.js` | `src/build/o.js` [GIT] |
| I3 | root `docs/gen` | `docs/gen/a.md`, `x/docs/gen/a.md` | `x/docs/gen/a.md` [GIT] |
| I4 | root `cache/` | `cache/a`, `x/cache` (a file) | `x/cache` [GIT] |
| I5 | root `a/**/b` | `a/b`, `a/x/b`, `a/x/y/b`, `c/a/b` | `c/a/b` [GIT] |
| I6 | root `out/**` | `out/a`, `out/s/b`, `outer/c` | `outer/c` (the `out` directory is traversed, but every descendant matches; glob lists files only) [GIT] |
| I7 | root `*.md` then `!README.md` | `README.md`, `d/README.md`, `d/x.md` | `README.md`, `d/README.md` [GIT] |
| I8 | root `!README.md` then `*.md` | same | none [GIT] |
| I9 | root `secrets/` then `!secrets/keep.txt` | `secrets/keep.txt`, `secrets/a.txt` | none: an excluded parent cannot be re-included [GIT] |
| I10 | root `split/*` then `!split/keep.txt` | `split/keep.txt`, `split/a.txt` | `split/keep.txt` [GIT] |
| I11 | `sub/.gitignore`: `*.tmp` | `c.tmp`, `sub/a.tmp`, `sub/x/b.tmp` | `c.tmp` [GIT] |
| I12 | `sub/.gitignore`: `/local` | `local/f`, `sub/local/f`, `sub/x/local/f` | `local/f`, `sub/x/local/f` [GIT] |
| I13 | root `*.log`; `sub/.gitignore`: `!keep.log` | `sub/keep.log`, `sub/a.log` | `sub/keep.log` [GIT][DIFF-FB][RG-REF] |
| I14 | root `secrets/`; `secrets/.gitignore`: `!keep.txt` | `secrets/keep.txt` | none; `secrets/.gitignore` is never opened (loader counter = 0 for it) [GIT][POL] |
| I15 | root `\#notes`, `# comment`, `\!bang` | `#notes`, `!bang`, `comment` | `comment` [GIT] |
| I16 | root `trail␠␠` | `trail` | none: unescaped trailing spaces are dropped [GIT] |
| I17 | `foo\␠` | — | the rule matches the name `foo␠` [PARSER] |
| I18 | root `␠␠lead` | `␠␠lead`, `lead` | `lead`: leading spaces are significant [GIT][DIFF-FB][RG-REF] |
| I19 | root with CRLF line endings: `*.log\r\n` | `a.log` | none [GIT] |
| I20 | root with UTF-8 BOM before `*.log` | `a.log` | none [RG-REF] |
| I21 | root `file[0-9].txt`, `[!a]x.txt` | `file1.txt`, `filea.txt`, `bx.txt`, `ax.txt` | `filea.txt`, `ax.txt` [GIT] |
| I22 | root `*.{tmp,bak}` | `a.tmp`, `a.bak`, `a.{tmp,bak}` | rg: `a.{tmp,bak}`; git: `a.tmp`, `a.bak` [RG-REF] |
| I23 | root `Build/` | `build/f` | `build/f`: case-sensitive [RG-REF] |
| I24 | root `a**b` | `axyb`, `ax/yb` | `ax/yb` [RG-REF] |
| I25 | root `!`, `/`, blank lines | `f` | `f`, with no diagnostics [GIT] |
| I26 | search started at `ws1/sub` with root `*.log` | `sub/a.log`, `sub/b.txt` | `sub/b.txt`; the ancestor rules are loaded, but the start directory itself is never pruned [POL] |

### Built-ins, policy and include_gitignored
| # | Case | Expected |
|---|------|----------|
| P1 | `.git/config`, `x/.git/HEAD`, gitfile `w/.git`; glob `**/*`, `.git/**`, `**`, each with and without `include_gitignored` | never listed or searched [POL][RG-REF shows rg would list them under `**/*`] |
| P2 | `node_modules/m/i.js` | hidden by default; listed with `include_gitignored:true` (upstream test `universal-tools.test.ts:202-240`) [POL] |
| P3 | root `!node_modules/` | `node_modules/m/i.js` listed: the built-in has lowest precedence [POL][DIFF-FB] |
| P4 | `dist/a.js`, `build/b.js`, `out/c.js` with no rules | all listed: no fallback defaults [POL][DIFF-FB] |
| P5 | `ws/../.gitignore` containing `*` (the mount's parent) | no effect; the loader never opens a path outside `ws/` [POL] |
| P6 | junction `ws/link → ws/real` and `ws/link2 → outside` | neither traversed nor listed; `real/` contents listed once [POL] |
| P7 | `ws/.gitignore` is a file symlink to `ws/rules.txt` | not applied; diagnostic `.gitignore not applied (links are not followed)` [POL] |
| P8 | `ws/.gitignore` is a directory | diagnostic `not a regular file`; the walk continues [POL] |
| P9 | `include_gitignored:true` with every rule file malformed | no rule file opened (loader counter 0), no diagnostics, `.git` still hidden [POL] |
| P10 | `secrets/` ignored by `secrets/`; grep `include_gitignored:true` | `secrets/leak.md` searched (upstream test `universal-tools.test.ts:262-284`) |

### Malformed and unreadable rule files
| # | Case | Expected |
|---|------|----------|
| M1 | `sub/.gitignore` containing invalid UTF-8 | its rules are not applied (fail-open); exactly one diagnostic, `sub/.gitignore not applied (not valid UTF-8)` |
| M2 | root `.gitignore` of 300 KiB | not applied; diagnostic `exceeds 256 KiB` |
| M3 | a rule file whose ACL denies read | diagnostic `unreadable, Windows error 5`; the walk continues |
| M4 | lines `*.log`, `a[`, `*.tmp` | lines 1 and 3 apply; diagnostic `line 2 skipped (unclosed character class)` [RG-REF] |
| M5 | 30 malformed rule files | 10 diagnostics, then `and 20 more` |
| M6 | a malformed rule file inside an ignored directory | no diagnostic (never read) |

### Tool pattern language and count
| # | Call | Tree | Expected |
|---|------|------|----------|
| G1 | glob `*.ts` | `a.ts`, `src/b.ts`, `src/x/c.ts` | all three [rg][DIFF-FB] |
| G2 | glob `src/*.ts` and `/src/*.ts` | same | `src/b.ts` [rg][DIFF-FB] |
| G3 | glob `**/*.test.ts`, `src/**/config.json` | — | standard [rg] |
| G4 | glob `*.{ts,tsx}` | `a.ts`, `b.tsx`, `c.js` | `a.ts`, `b.tsx` [rg] |
| G5 | glob `file?.txt`, `[ab].txt`, `[!a].txt`, `\[x\].txt` | `file1.txt`, `a.txt`, `b.txt`, `[x].txt` | respectively `file1.txt`; `a.txt`, `b.txt`; `b.txt`; `[x].txt` [rg] |
| G6 | glob `*.yml` | `.github/w/ci.yml`, `.env.yml` | both: `*` matches dotfiles and glob includes hidden [rg][DIFF-FB] |
| G7 | glob `*.TS` | `a.ts` | none: case-sensitive [RG-REF] |
| G8 | glob `**/*.ts` over 120 matching files | — | 50 paths in deterministic order, `truncated:true`, message `50+` [DIFF: upstream order non-deterministic, no signal] |
| G9 | glob `**/*.ts` over exactly 50 matching files | — | 50, `truncated:false` |

### Overrides (include/exclude)
| # | Case | Expected |
|---|------|----------|
| E1 | grep exclude `**/test-*.js` | `test-a.js` and `d/test-b.js` excluded; `a.js` searched |
| E2 | grep exclude `metadata/**` over 20,000 files in `metadata/` plus 5 others | the 5 searched, not truncated; `metadata/` is never opened (prefix prune) |
| E3 | grep include `*.js`, exclude `vendor/**` | `vendor/a.js` excluded: the later glob wins [rg] |
| E4 | grep exclude with `include_gitignored:true` | the exclusion still applies |
| O1 | root `*.log`; glob `**/*.log` | rg override re-includes `a.log`: expected `a.log` [RG-REF; D-R] |
| O2 | root `*.log`; glob `**` | `a.log` hidden [rg] |
| O3 | root `gen/`; grep include `gen/**` | `gen/` pruned (a directory is not whitelisted by `gen/**`): no results [RG-REF] |
| O4 | grep include `!*.md` | treated as an exclusion (rg) [RG-REF] |

### Hidden files in grep (decision D-H)
| # | Case | Option A (parity) | Option B |
|---|------|-------------------|----------|
| H1 | grep `x` over `.env`, `.github/w/ci.yml`, `a.txt` | `a.txt` | all three |
| H2 | grep include `.env` | `.env` (whitelisted) [RG-REF] | `.env` |
| H3 | root `!.env` (negation) | `.env` [RG-REF] | `.env` |

### Budget and scan bounds
| # | Case | Expected |
|---|------|----------|
| B1 | gitignored `ignored/` with 20,000 files plus 10 others | 10 results, not truncated; `ignored/` never enumerated |
| B2 | non-ignored `logs/` with 15,000 `*.log` (ignored by `*.log`) plus 10 `.txt` | 10 results, not truncated: ignored names do not consume the budget |
| B3 | one directory with 250,000 ignored files | truncated, reason `scan limit 200000 names` |
| B4 | 10,001 admitted files | truncated, reason `entry budget 10000` |
| B5 | same as E2 with the prefix optimisation disabled | identical result set (the optimisation is exact) |

## 8. Behaviours requiring a real ripgrep reference capture
Root should capture golden outputs once with **ripgrep v15.0.0**, run exactly as upstream does:
- `rg --files --no-config --hidden -g '!.git' [-g P] [--no-ignore]`
- `rg --json --no-config --no-ignore-global -i --max-count 50 [-g I] [--glob '!E'] [--no-ignore] -e x .`

Run both on each fixture **inside** a `git init` repo, then commit the sorted path sets as test data. Native tests compare against the data; no native code runs rg.

Cases tagged [RG-REF] are S5, I13, I18, I20, I22, I23, I24, M4, G7, O1–O4 and H2–H3. Also capture:
- **T1–T3:** `.rgignore` vs `.ignore` vs `.gitignore` precedence across depths, before Phase 2 adds them.
- **Glob `**/*` vs `**`:** for gitignored files and for `.git`. Native keeps `.git` hidden regardless (P1).
- **The same tree outside a git repo:** this documents rg's require-git difference, which native deliberately doesn't follow.

## 9. Plan and tests
1. **Phase 1 (mandatory):**
   - `SearchRules` module.
   - Loader.
   - `ScopedAdmission` in `readScopedDirectory` and the search walker.
   - Schemas and validation.
   - Glob count and order.
   - Diagnostics.
2. **Tests:**
   - `tests/test_search_rules.cpp`, pure tables: every [GIT]/[PARSER] case plus compiler errors.
   - `tests/test_search_ignore_host.cpp`, filesystem fixtures through `ToolExecutor`: S, I, P, M, G, E, B; junction and ACL fixtures are Windows-only.
   - The root CMake file is not in this snapshot, so root must register both.
3. **Phase 2:** after the RG-REF capture, lock D-R, decide D-H, optionally add `.ignore`/`.rgignore` (D-I), and convert the [RG-REF] rows into golden comparisons.
4. **Keep green:** root's existing search tests (basename matching, skipped-file diagnostics, UTF-16, previews, 8 MiB accounting) must pass unchanged. The `include_file_pattern` matcher should switch to `compileRule(..., ToolPattern)` only where root's basename tests confirm identical results.
