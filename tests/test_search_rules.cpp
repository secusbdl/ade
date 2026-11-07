// Pure rule tables for native glob/grepSearch filtering. Expectations come
// from real ripgrep 15.0.0 runs (the engine upstream Stagewise uses) on
// throwaway fixtures; comments mark native policy and native pruning.
#include "core/SearchRules.h"
#include <QtTest>

using namespace ade::search;

namespace {
RuleFile ruleFile(const QString &text, const QString &base = {}) {
  RuleFile file;
  file.base = base;
  const auto lines = text.split('\n');
  for (int i = 0; i < lines.size(); ++i) {
    Rule rule;
    QString error;
    if (compileRule(lines[i], Syntax::IgnoreFile, i + 1, &rule, &error))
      file.rules.append(rule);
  }
  return file;
}
OverrideSet overrides(const QStringList &globs) {
  OverrideSet set;
  for (const auto &glob : globs) {
    QString error;
    if (!set.add(glob, &error))
      qFatal("Unexpected override error for %s: %s", qPrintable(glob),
             qPrintable(error));
  }
  return set;
}
IgnoreLevel level(const QString &base, const QString &rg,
                  const QString &ignore, const QString &git) {
  IgnoreLevel result;
  const QString texts[] = {rg, ignore, git};
  for (int kind = 0; kind < IgnoreKindCount; ++kind) {
    result.files[kind] = ruleFile(texts[kind], base);
    result.present[kind] = !texts[kind].isNull();
  }
  return result;
}
} // namespace
Q_DECLARE_METATYPE(ade::search::Match)
#define COMPARE_MATCH(actual, expected) QCOMPARE(int(actual), int(expected))

class SearchRulesTest : public QObject {
  Q_OBJECT
private slots:
  void ignoreLines_data() {
    QTest::addColumn<QString>("rules");
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("directory");
    QTest::addColumn<Match>("expected");
    const auto I = Match::Ignore, W = Match::Whitelist, N = Match::None;
    const struct {
      const char *rules, *path;
      bool directory;
      Match expected;
    } rows[] = {
        {"*.log", "a.log", false, I},
        {"*.log", "x/y/b.log", false, I},
        {"*.log", "a.log.txt", false, N},
        {"/build", "build", true, I},
        {"/build", "src/build", true, N},
        {"docs/gen", "docs/gen", true, I},
        {"docs/gen", "x/docs/gen", true, N},
        {"cache/", "cache", true, I},
        {"cache/", "x/cache", false, N},
        {"a/**/b", "a/b", false, I},
        {"a/**/b", "a/x/b", false, I},
        {"a/**/b", "a/x/y/b", false, I},
        {"a/**/b", "c/a/b", false, N},
        {"out/**", "out", true, N},
        {"out/**", "out/a", false, I},
        {"out/**", "out/s/b", false, I},
        {"out/**", "outer/c", false, N},
        {"*.md\n!README.md", "README.md", false, W},
        {"*.md\n!README.md", "d/README.md", false, W},
        {"*.md\n!README.md", "d/x.md", false, I},
        {"!README.md\n*.md", "README.md", false, I},
        {"split/*\n!split/keep.txt", "split/keep.txt", false, W},
        {"split/*\n!split/keep.txt", "split/a.txt", false, I},
        {"\\#notes\n# comment\n\\!bang", "#notes", false, I},
        {"\\#notes\n# comment\n\\!bang", "!bang", false, I},
        {"\\#notes\n# comment\n\\!bang", "comment", false, N},
        {"\\#notes\n# comment\n\\!bang", "# comment", false, N},
        {"trail  ", "trail", false, I},
        {"trail\t", "trail", false, I},
        {"*.log\r", "a.log", false, I},
        {"  lead", "  lead", false, I},
        {"  lead", "lead", false, N},
        {"file[0-9].txt", "file1.txt", false, I},
        {"file[0-9].txt", "filea.txt", false, N},
        {"[!a]x.txt", "bx.txt", false, I},
        {"[!a]x.txt", "ax.txt", false, N},
        {"[^a]x.txt", "bx.txt", false, I},
        {"[a-c]*.txt", "c1.txt", false, I},
        {"[a-c]*.txt", "d1.txt", false, N},
        {"[]]x", "]x", false, I},
        {"[]]x", "ax", false, N},
        {"*.{tmp,bak}", "a.tmp", false, I},
        {"*.{tmp,bak}", "a.bak", false, I},
        {"*.{tmp,bak}", "a.{tmp,bak}", false, N},
        {"*.{a,{b,c}}", "x.c", false, I},
        {"*.{a,{b,c}}", "x.d", false, N},
        {"\\{x\\}", "{x}", false, I},
        {"Build/", "build", true, N},
        {"*.LOG", "a.log", false, N},
        {"*.LOG", "b.LOG", false, I},
        {"a**b", "axyb", false, I},
        {"a**b", "ab", false, I},
        {"a**b", "ax/yb", false, N},
        {"**/cache/", "x/cache", true, I},
        {"**/cache/", "x/cachex", true, N},
        {"foo/**/bar/", "foo/bar", true, I},
        {"foo/**/bar/", "foo/q/bar", true, I},
        {"foo/**/bar/", "foo/zbar", true, N},
        {"**/x", "x", false, I},
        {"**/x", "a/x", true, I},
        {"**/x", "y", false, N},
        {"?.txt", "a.txt", false, I},
        {"?.txt", "ab.txt", false, N},
        {"?.txt", "d/b.txt", false, I},
        {"a#b", "a#b", false, I},
        {"*.txt\n !a.txt", "a.txt", false, I},
        {"*.txt\n !a.txt", " !a.txt", false, I},
        {"a[b*", "a[bc", false, I},
        {"a[b*", "a[", false, N},
        {"a[b*", "ab", false, N},
        {"x/**/", "x/d", true, I},
        {"x/**/", "x/a", false, N},
        {"**/", "a", true, I},
        {"**/", "c", false, N},
        {"a/b/", "a/b", true, I},
        {"a/b/", "x/a/b", true, N},
        {"*.txt\n!/keep.txt", "keep.txt", false, W},
        {"*.txt\n!/keep.txt", "d/keep.txt", false, I},
        {"a\\ ", "a ", false, I},
        {"a\\ ", "a", false, N},
        // A lone "!" becomes "!**/", which re-includes everything in rg.
        {"*.log\n!", "a.log", false, W},
        {"  # x", "  # x", false, I},
        {"*.env", ".env", false, I},
        {"*\n!*/\n!*.txt", "d", true, W},
        {"*\n!*/\n!*.txt", "d/c.txt", false, W},
        {"*\n!*/\n!*.txt", "d/e.md", false, I},
        {"foo\\\\", "foo", false, N},
    };
    for (const auto &row : rows)
      QTest::addRow("%s | %s%s", row.rules, row.path, row.directory ? "/" : "")
          << QString::fromUtf8(row.rules) << QString::fromUtf8(row.path)
          << row.directory << row.expected;
  }
  void ignoreLines() {
    QFETCH(QString, rules);
    QFETCH(QString, path);
    QFETCH(bool, directory);
    QFETCH(Match, expected);
    COMPARE_MATCH(ruleFile(rules).match(path, directory), expected);
  }
  void ignoreFileBaseIsItsDirectory() {
    const auto anchored = ruleFile("x/*.ts", "sub");
    COMPARE_MATCH(anchored.match("sub/x/a.ts", false), Match::Ignore);
    COMPARE_MATCH(anchored.match("sub/y/x/b.ts", false), Match::None);
    COMPARE_MATCH(anchored.match("x/a.ts", false), Match::None);
    const auto rooted = ruleFile("/local", "sub");
    COMPARE_MATCH(rooted.match("sub/local", true), Match::Ignore);
    COMPARE_MATCH(rooted.match("local", true), Match::None);
    COMPARE_MATCH(rooted.match("sub/x/local", true), Match::None);
    COMPARE_MATCH(rooted.match("subx/local", true), Match::None);
  }
  void malformedIgnoreLines_data() {
    QTest::addColumn<QString>("line");
    QTest::addColumn<QString>("reason");
    QTest::newRow("unclosed-brace") << "{a" << "unclosed alternate group";
    QTest::newRow("unopened-brace") << "a}b" << "unopened alternate group";
    QTest::newRow("dangling-escape") << "a\\" << "dangling";
    QTest::newRow("invalid-range") << "[z-a]" << "invalid character range";
  }
  void malformedIgnoreLines() {
    QFETCH(QString, line);
    QFETCH(QString, reason);
    Rule rule;
    QString error;
    QVERIFY(!compileRule(line, Syntax::IgnoreFile, 1, &rule, &error));
    QVERIFY2(error.contains(reason), qPrintable(error));
  }
  void blankAndCommentLinesAddNoRule() {
    for (const auto &line : {QString(), QString("   "), QString("# c")}) {
      Rule rule;
      QString error;
      QVERIFY(!compileRule(line, Syntax::IgnoreFile, 1, &rule, &error));
      QVERIFY(error.isEmpty());
    }
  }
  void overrideGlobs_data() {
    QTest::addColumn<QStringList>("globs");
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("directory");
    QTest::addColumn<Match>("expected");
    const auto I = Match::Ignore, W = Match::Whitelist, N = Match::None;
    const struct {
      QStringList globs;
      const char *path;
      bool directory;
      Match expected;
    } rows[] = {
        {{"*.ts"}, "a.ts", false, W},
        {{"*.ts"}, "src/x/c.ts", false, W},
        {{"*.ts"}, ".h.ts", false, W},
        {{"*.ts"}, "a.tsx", false, I},
        {{"*.ts"}, "src", true, N},
        {{"src/*.ts"}, "src/b.ts", false, W},
        {{"src/*.ts"}, "src/x/c.ts", false, I},
        {{"/src/*.ts"}, "src/b.ts", false, W},
        {{"./src/*.ts"}, "src/b.ts", false, I},
        {{"src/**"}, "src", true, N},
        {{"src/**"}, "src/x/c.ts", false, W},
        {{"*.{ts,{tsx,js}}"}, "c.js", false, W},
        {{"!*.ts"}, "a.ts", false, I},
        {{"!*.ts"}, "c.js", false, N},
        {{"#*.ts"}, "c.js", false, N},
        {{"*.ts "}, "a.ts", false, W},
        {{"a**s"}, "a.ts", false, W},
        {{"src\\*.ts"}, "src/b.ts", false, I},
        {{"*.{}"}, "a.ts", false, I},
        {{"/"}, "a.ts", false, I},
        {{""}, "a.ts", false, N},
        {{"**a"}, "d/ya", false, W},
        {{"***/c.ts"}, "a/b/c.ts", false, I},
        {{"***/c.ts"}, "c.ts", false, I},
        {{"{**/c,x}.ts"}, "a/b/c.ts", false, W},
        {{"{**/c,x}.ts"}, "c.ts", false, W},
        {{"{**/c,x}.ts"}, "x.ts", false, W},
        {{"a[b-]"}, "a-", false, W},
        {{"a[b-]"}, "a-b", false, I},
        {{"a\\/b"}, "a/b", false, W},
        {{"[!]]x"}, "ax", false, W},
        {{"[!]]x"}, "]x", false, I},
        {{"[ab].txt"}, "b.txt", false, W},
        {{"\\[x\\].txt"}, "[x].txt", false, W},
        {{"a[/-]b"}, "a/b", false, W},
        {{"a[!x]b"}, "a/b", false, W},
        {{"a[!x]b"}, "axb", false, I},
        {{"x/*/a.ts"}, "x/.d/a.ts", false, W},
        {{"*.TS"}, "a.ts", false, I},
        {{"src/*.ts"}, "SRC/a.ts", false, I},
        {{"src"}, "src", true, W},
        {{"src"}, "src/b.ts", false, I},
        {{"src/"}, "src", true, W},
        {{"src/"}, "src", false, I},
        {{"{a,b}/*.ts"}, "b/y.ts", false, W},
        {{"{a,b}/*.ts"}, "c/z.ts", false, I},
        {{"**/{a,b}/*.ts"}, "a/x.ts", false, W},
        {{"a,b"}, "a,b", false, W},
        {{"a,b"}, "a", false, I},
        {{"*.js", "!vendor/**"}, "vendor/a.js", false, I},
        {{"*.js", "!vendor/**"}, "b.js", false, W},
        {{"vendor/**", "!*.js"}, "vendor/a.js", false, I},
        {{"!metadata/**"}, "metadata", true, N},
        {{"!metadata/**"}, "metadata/s/b.js", false, I},
        {{"!metadata/**"}, "x/metadata/c.js", false, N},
        {{"!metadata"}, "x/metadata", true, I},
        {{"!build/"}, "build", true, I},
        {{"!build/"}, "build", false, N},
        {{"**/"}, "a", true, W},
        {{"**/"}, "a/b.js", false, I},
        {{"**"}, ".git", true, W},
    };
    for (const auto &row : rows)
      QTest::addRow("%s | %s%s", qPrintable(row.globs.join(" ")), row.path,
                    row.directory ? "/" : "")
          << row.globs << QString::fromUtf8(row.path) << row.directory
          << row.expected;
  }
  void overrideGlobs() {
    QFETCH(QStringList, globs);
    QFETCH(QString, path);
    QFETCH(bool, directory);
    QFETCH(Match, expected);
    COMPARE_MATCH(overrides(globs).match(path, directory), expected);
  }
  void overrideErrors_data() {
    QTest::addColumn<QString>("glob");
    QTest::addColumn<QString>("reason");
    QTest::newRow("unclosed-class") << "[ab.txt" << "unclosed character class";
    QTest::newRow("unclosed-brace") << "*.{ts" << "unclosed alternate group";
    QTest::newRow("unopened-brace") << "a}b" << "unopened alternate group";
    QTest::newRow("dangling-escape") << "a\\" << "dangling";
    QTest::newRow("invalid-range") << "[z-a].ts" << "invalid character range";
  }
  void overrideErrors() {
    QFETCH(QString, glob);
    QFETCH(QString, reason);
    OverrideSet set;
    QString error;
    QVERIFY(!set.add(glob, &error));
    QVERIFY2(error.contains(reason), qPrintable(error));
  }
  void excludePrefixPruningIsExact() {
    QVERIFY(overrides({"!metadata/**"}).prunes("metadata"));
    QVERIFY(!overrides({"!metadata/**"}).prunes("x/metadata"));
    QVERIFY(overrides({"*.js", "!vendor/**"}).prunes("vendor"));
    // A later include could still match a descendant.
    QVERIFY(!overrides({"!vendor/**", "*.js"}).prunes("vendor"));
    QVERIFY(!overrides({"vendor/**", "!*.js"}).prunes("vendor"));
    QVERIFY(!overrides({"!vendor/**/"}).prunes("vendor"));
    QVERIFY(!overrides({"!vendor/x/**"}).prunes("vendor"));
    QVERIFY(overrides({"!vendor/x/**"}).prunes("vendor/x"));
  }
  void ignorePrecedenceIsByKindThenDepth() {
    // .ignore beats .gitignore at the same level (rg T1).
    IgnoreStack t1;
    t1.push(level({}, {}, "!keep.log", "*.log"));
    COMPARE_MATCH(t1.match("keep.log", "keep.log", false), Match::Whitelist);
    COMPARE_MATCH(t1.match("a.log", "a.log", false), Match::Ignore);
    // A root .ignore beats a deeper .gitignore negation (rg T2).
    IgnoreStack t2;
    t2.push(level({}, {}, "*.log", {}));
    t2.push(level("sub", {}, {}, "!keep.log"));
    COMPARE_MATCH(t2.match("sub/keep.log", "keep.log", false), Match::Ignore);
    // .rgignore beats .ignore (rg T3).
    IgnoreStack t3;
    t3.push(level({}, "!keep.log", "*.log", {}));
    COMPARE_MATCH(t3.match("keep.log", "keep.log", false), Match::Whitelist);
    // A root .ignore negation beats a deeper .gitignore rule (rg T4).
    IgnoreStack t4;
    t4.push(level({}, {}, "!keep.log", {}));
    t4.push(level("sub", {}, {}, "*.log"));
    COMPARE_MATCH(t4.match("sub/keep.log", "keep.log", false), Match::Whitelist);
    COMPARE_MATCH(t4.match("sub/a.log", "a.log", false), Match::Ignore);
    // Within .gitignore the deepest matching file decides (rg I13).
    IgnoreStack nested;
    nested.push(level({}, {}, {}, "*.log"));
    nested.push(level("sub", {}, {}, "!keep.log"));
    COMPARE_MATCH(nested.match("sub/keep.log", "keep.log", false), Match::Whitelist);
    COMPARE_MATCH(nested.match("sub/a.log", "a.log", false), Match::Ignore);
    nested.pop();
    COMPARE_MATCH(nested.match("sub/keep.log", "keep.log", false), Match::Ignore);
  }
  void gitBoundaryScopesOnlyGitignore() {
    // A level holding .git applies its own .gitignore, never an ancestor's;
    // .ignore and .rgignore still cross the boundary (rg 15.0.0 fixtures).
    IgnoreStack stack;
    stack.push(level({}, "*.rg", "*.ig", "*.log\n*.tmp"));
    auto repo = level("sub", {}, {}, "!keep.tmp");
    repo.gitBoundary = true;
    stack.push(repo);
    COMPARE_MATCH(stack.match("sub/a.log", "a.log", false), Match::None);
    COMPARE_MATCH(stack.match("sub/keep.tmp", "keep.tmp", false), Match::Whitelist);
    COMPARE_MATCH(stack.match("sub/a.ig", "a.ig", false), Match::Ignore);
    COMPARE_MATCH(stack.match("sub/a.rg", "a.rg", false), Match::Ignore);
    stack.push(level("sub/x", {}, {}, {}));
    COMPARE_MATCH(stack.match("sub/x/a.log", "a.log", false), Match::None);
    stack.pop();
    stack.pop();
    COMPARE_MATCH(stack.match("a.log", "a.log", false), Match::Ignore);
  }
  void nodeModulesIsLowestPrecedenceBuiltIn() {
    // Native policy: upstream's fallback hides node_modules; rg alone does not.
    IgnoreStack empty;
    COMPARE_MATCH(empty.match("node_modules", "node_modules", true), Match::Ignore);
    COMPARE_MATCH(empty.match("x/node_modules", "node_modules", true), Match::Ignore);
    COMPARE_MATCH(empty.match("node_modules", "node_modules", false), Match::None);
    IgnoreStack reincluded;
    reincluded.push(level({}, {}, {}, "!node_modules/"));
    COMPARE_MATCH(reincluded.match("node_modules", "node_modules", true),
             Match::Whitelist);
  }
  void admitFollowsRgOrderWithNativePolicy() {
    const OverrideSet none;
    IgnoreStack ignores;
    ignores.push(level({}, {}, {}, "*.log\n!.env\n!.github/"));
    Policy grep;
    grep.skipHidden = true;
    Policy glob;
    // Native policy: .git and links are never admitted, even when an include
    // glob such as "**/*" would make rg list .git/HEAD.
    QVERIFY(!admit(glob, overrides({"**/*"}), ignores, ".git", ".git",
                   ".git", true, false, false));
    QVERIFY(!admit(glob, overrides({"**/*"}), ignores, ".git", "w/.git",
                   "w/.git", false, false, false));
    QVERIFY(!admit(glob, none, ignores, "link", "link", "link", true, true,
                   false));
    // Overrides beat ignore files and the hidden policy (rg O1, H2).
    QVERIFY(admit(grep, overrides({"*.log"}), ignores, "a.log", "a.log",
                  "a.log", false, false, false));
    QVERIFY(admit(grep, overrides({".env2"}), ignores, ".env2", ".env2",
                  ".env2", false, false, false));
    QVERIFY(!admit(glob, none, ignores, "a.log", "a.log", "a.log", false,
                   false, false));
    // grep skips hidden names and Windows hidden attributes (rg H1, W1);
    // glob does not (rg --hidden).
    QVERIFY(!admit(grep, none, ignores, ".hdir", ".hdir", ".hdir", true,
                   false, false));
    QVERIFY(!admit(grep, none, ignores, "h.txt", "h.txt", "h.txt", false,
                   false, true));
    QVERIFY(admit(glob, none, ignores, "h.txt", "h.txt", "h.txt", false,
                  false, true));
    QVERIFY(admit(glob, none, ignores, ".hdir", ".hdir", ".hdir", true,
                  false, false));
    // An ignore-file negation un-hides an entry for grep (rg H3, H3b).
    QVERIFY(admit(grep, none, ignores, ".env", ".env", ".env", false, false,
                  false));
    QVERIFY(admit(grep, none, ignores, ".github", ".github", ".github", true,
                  false, false));
    // include_gitignored disables ignore files and the node_modules built-in.
    Policy all = glob;
    all.respectIgnores = false;
    QVERIFY(admit(all, none, ignores, "a.log", "a.log", "a.log", false, false,
                  false));
    QVERIFY(admit(all, none, IgnoreStack{}, "node_modules", "node_modules",
                  "node_modules", true, false, false));
    QVERIFY(!admit(glob, none, IgnoreStack{}, "node_modules", "node_modules",
                   "node_modules", true, false, false));
    // A pruned exclude prefix is never entered.
    QVERIFY(!admit(glob, overrides({"!metadata/**"}), IgnoreStack{},
                   "metadata", "metadata", "metadata", true, false, false));
  }
};
QTEST_GUILESS_MAIN(SearchRulesTest)
#include "test_search_rules.moc"
