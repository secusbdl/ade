// Real filesystem fixtures through ToolExecutor for glob/grepSearch ignore
// and filter parity. Expected path sets were captured from ripgrep 15.0.0
// run exactly as upstream Stagewise runs it (glob: rg --files --no-config
// --hidden -g '!.git' [-g P] [--no-ignore]; grep: rg --no-config
// --no-ignore-global -i [-g I] [--glob '!E'] [--no-ignore]) inside a git
// work tree. Rows named "policy-*" are deliberate native divergences.
#include "core/ToolExecutor.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace ade;

namespace {
bool writeFile(const QString &path, const QByteArray &bytes) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    return false;
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
// Each entry is "path" (content "x\n") or "path=content".
bool writeTree(const QString &root, const QStringList &entries) {
  for (const auto &entry : entries) {
    const auto split = entry.indexOf('=');
    const auto path = split < 0 ? entry : entry.left(split);
    const auto content =
        split < 0 ? QByteArray("x\n") : entry.mid(split + 1).toUtf8();
    if (!writeFile(root + '/' + path, content))
      return false;
  }
  return true;
}
QVariantList run(ToolExecutor &tools, const QString &name,
                 const QJsonObject &args) {
  QSignalSpy completed(&tools, &ToolExecutor::completed);
  tools.execute("fixture", name, args);
  if (completed.isEmpty())
    completed.wait(30000);
  return completed.isEmpty() ? QVariantList{} : completed.first();
}
QString output(const QVariantList &result) {
  return result.value(1).toString();
}
bool failed(const QVariantList &result) {
  return result.size() == 4 && result.at(2).toBool();
}
bool complete(const QVariantList &result) {
  return result.size() == 4 && !result.at(2).toBool();
}
// Result rows end at the first blank line; completion notes follow it.
QStringList paths(const QString &text, bool grep) {
  QStringList result;
  if (text == "No matches.")
    return result;
  static const QRegularExpression row(QStringLiteral("^(.*?):\\d+: "));
  for (const auto &line : text.split('\n')) {
    if (line.isEmpty())
      break;
    const auto path = grep ? row.match(line).captured(1) : line;
    if (!result.contains(path))
      result.append(path);
  }
  result.sort();
  return result;
}
QJsonObject schemaProperty(const QString &tool, const QString &name) {
  for (const auto &value : ToolExecutor::definitions()) {
    const auto function = value.toObject()["function"].toObject();
    if (function["name"] == tool)
      return function["parameters"]
          .toObject()["properties"]
          .toObject()[name]
          .toObject();
  }
  return {};
}
} // namespace

class SearchIgnoreTest : public QObject {
  Q_OBJECT
private slots:
  void schemaMatchesUpstreamContract() {
    QCOMPARE(schemaProperty("glob", "include_gitignored")["type"].toString(),
             QString("boolean"));
    QCOMPARE(schemaProperty("grepSearch", "include_gitignored")["type"].toString(),
             QString("boolean"));
    QCOMPARE(schemaProperty("grepSearch", "exclude_file_pattern")["type"].toString(),
             QString("string"));
    QVERIFY(schemaProperty("grepSearch", "exclude_file_pattern")["description"]
                .toString()
                .contains("metadata/**"));
    QVERIFY(schemaProperty("glob", "pattern")["description"].toString().contains(
        "[abc]"));
    QVERIFY(schemaProperty("glob", "exclude_file_pattern").isEmpty());
  }
  void fixtures_data() {
    QTest::addColumn<QString>("tool");
    QTest::addColumn<QStringList>("tree");
    QTest::addColumn<QJsonObject>("args");
    QTest::addColumn<QStringList>("expected");
    const auto glob = [](const char *name, const QStringList &tree,
                         const QJsonObject &args, const QStringList &expected) {
      QTest::newRow(name) << QString("glob") << tree << args << expected;
    };
    const auto grep = [](const char *name, const QStringList &tree,
                         const QJsonObject &args, const QStringList &expected) {
      auto search = args;
      search["query"] = "x";
      search["max_matches"] = 50;
      QTest::newRow(name) << QString("grepSearch") << tree << search
                          << expected;
    };
    const QJsonObject all{{"pattern", "**"}};
    glob("gi-extension", {".gitignore=*.log\n", "a.log", "x/y/b.log", "a.log.txt"}, all, {".gitignore", "a.log.txt"});
    glob("gi-anchored", {".gitignore=/build\n", "build/o.js", "src/build/o.js"}, all, {".gitignore", "src/build/o.js"});
    glob("gi-middle-slash", {".gitignore=docs/gen\n", "docs/gen/a.md", "x/docs/gen/a.md"}, all, {".gitignore", "x/docs/gen/a.md"});
    glob("gi-directory-only", {".gitignore=cache/\n", "cache/a", "x/cache"}, all, {".gitignore", "x/cache"});
    glob("gi-double-star", {".gitignore=a/**/b\n", "a/b", "a/x/b", "a/x/y/b", "c/a/b"}, all, {".gitignore", "c/a/b"});
    glob("gi-contents-only", {".gitignore=out/**\n", "out/a", "out/s/b", "outer/c"}, all, {".gitignore", "outer/c"});
    glob("gi-negation", {".gitignore=*.md\n!README.md\n", "README.md", "d/README.md", "d/x.md"}, all, {".gitignore", "README.md", "d/README.md"});
    glob("gi-negation-order", {".gitignore=!README.md\n*.md\n", "README.md", "d/README.md", "d/x.md"}, all, {".gitignore"});
    glob("gi-excluded-parent", {".gitignore=secrets/\n!secrets/keep.txt\n", "secrets/keep.txt", "secrets/a.txt"}, all, {".gitignore"});
    glob("gi-star-children", {".gitignore=split/*\n!split/keep.txt\n", "split/keep.txt", "split/a.txt"}, all, {".gitignore", "split/keep.txt"});
    glob("gi-nested", {"sub/.gitignore=*.tmp\n", "c.tmp", "sub/a.tmp", "sub/x/b.tmp"}, all, {"c.tmp", "sub/.gitignore"});
    glob("gi-nested-anchor", {"sub/.gitignore=/local\n", "local/f", "sub/local/f", "sub/x/local/f"}, all, {"local/f", "sub/.gitignore", "sub/x/local/f"});
    glob("gi-nested-negation", {".gitignore=*.log\n", "sub/.gitignore=!keep.log\n", "sub/keep.log", "sub/a.log"}, all, {".gitignore", "sub/.gitignore", "sub/keep.log"});
    glob("gi-ignored-parent-file-unread", {".gitignore=secrets/\n", "secrets/.gitignore=!keep.txt\n", "secrets/keep.txt"}, all, {".gitignore"});
    glob("gi-escapes", {".gitignore=\\#notes\n# comment\n\\!bang\n", "#notes", "!bang", "comment"}, all, {".gitignore", "comment"});
    glob("gi-trailing-spaces", {".gitignore=trail  \n", "trail"}, all, {".gitignore"});
    glob("gi-leading-spaces", {".gitignore=  lead\n", "  lead", "lead"}, all, {".gitignore", "lead"});
    glob("gi-crlf", {".gitignore=*.log\r\n", "a.log", "b.txt"}, all, {".gitignore", "b.txt"});
    glob("gi-bom", {QString::fromUtf8(".gitignore=\xef\xbb\xbf*.log\n"), "a.log", "b.txt"}, all, {".gitignore", "b.txt"});
    glob("gi-classes", {".gitignore=file[0-9].txt\n[!a]x.txt\n", "file1.txt", "filea.txt", "bx.txt", "ax.txt"}, all, {".gitignore", "ax.txt", "filea.txt"});
    glob("gi-braces", {".gitignore=*.{tmp,bak}\n", "a.tmp", "a.bak", "a.{tmp,bak}", "c.txt"}, all, {".gitignore", "a.{tmp,bak}", "c.txt"});
    glob("gi-case-sensitive", {".gitignore=Build/\n", "build/f"}, all, {".gitignore", "build/f"});
    glob("gi-inner-double-star", {".gitignore=a**b\n", "axyb", "ax/yb", "ab", "c.txt"}, all, {".gitignore", "ax/yb", "c.txt"});
    glob("gi-bare-bang-reincludes-all", {".gitignore=*.log\n!\n", "a.log", "d/b.log", "c.txt"}, all, {".gitignore", "a.log", "c.txt", "d/b.log"});
    glob("gi-bare-slash-noop", {".gitignore=*.log\n/\n", "a.log", "c.txt"}, all, {".gitignore", "c.txt"});
    glob("gi-bang-slash-noop", {".gitignore=*.log\n!/\n", "a.log", "c.txt"}, all, {".gitignore", "c.txt"});
    glob("gi-blank-lines-noop", {".gitignore=*.log\n\n   \n", "a.log", "c.txt"}, all, {".gitignore", "c.txt"});
    glob("gi-directory-match-prunes", {".gitignore=*.log\n", "x.log/inner.txt", "y.txt"}, all, {".gitignore", "y.txt"});
    glob("gi-unclosed-class-literal", {".gitignore=*.log\na[\n*.tmp\n", "a.log", "a.tmp", "a[", "c.txt"}, all, {".gitignore", "c.txt"});
    glob("gi-reinclude-tree", {".gitignore=*\n!*/\n!*.txt\n", "a.txt", "b.md", "d/c.txt", "d/e.md"}, all, {"a.txt", "d/c.txt"});
    glob("ignore-beats-gitignore", {".gitignore=*.log\n", ".ignore=!keep.log\n", "keep.log", "a.log"}, all, {".gitignore", ".ignore", "keep.log"});
    glob("ignore-kind-beats-depth", {".ignore=*.log\n", "sub/.gitignore=!keep.log\n", "sub/keep.log", "sub/a.log"}, all, {".ignore", "sub/.gitignore"});
    // A .git entry (directory or gitdir file) ends ancestor .gitignore
    // scope; .ignore and .rgignore still cross it.
    glob("gi-nested-repo-boundary", {".git/HEAD", "sub/.git/HEAD", ".gitignore=*.log\n", "a.log", "b.txt", "sub/c.log", "sub/d.txt"}, all, {".gitignore", "b.txt", "sub/c.log", "sub/d.txt"});
    glob("gi-nested-repo-git-file", {".git/HEAD", ".gitignore=*.log\n", "a.log", "sub/.git=gitdir: x\n", "sub/c.log"}, all, {".gitignore", "sub/c.log"});
    glob("gi-nested-repo-deep", {".git/HEAD", "sub/.git/HEAD", ".gitignore=*.log\nsub/deep/\n", "sub/deep/e.txt", "sub/x/f.log"}, all, {".gitignore", "sub/deep/e.txt", "sub/x/f.log"});
    glob("gi-nested-repo-own-rules", {".git/HEAD", "sub/.git/HEAD", ".gitignore=*.log\n", "sub/.gitignore=*.tmp\n", "sub/c.log", "sub/c.tmp", "sub/k/d.tmp"}, all, {".gitignore", "sub/.gitignore", "sub/c.log"});
    glob("gi-nested-repo-intermediate", {".git/HEAD", "mid/sub/.git/HEAD", ".gitignore=*.log\n", "mid/.gitignore=*.tmp\n", "mid/a.log", "mid/sub/c.log", "mid/sub/c.tmp"}, all, {".gitignore", "mid/.gitignore", "mid/sub/c.log", "mid/sub/c.tmp"});
    glob("gi-parent-ignores-nested-repo-directory", {".git/HEAD", "sub/.git/HEAD", ".gitignore=sub/\n", "sub/c.txt", "e.txt"}, all, {".gitignore", "e.txt"});
    glob("ignore-crosses-nested-repo", {".git/HEAD", "sub/.git/HEAD", ".ignore=*.log\n", "sub/c.log", "sub/d.txt"}, all, {".ignore", "sub/d.txt"});
    glob("rgignore-crosses-nested-repo", {".git/HEAD", "sub/.git/HEAD", ".rgignore=*.log\n", "sub/c.log", "sub/d.txt"}, all, {".rgignore", "sub/d.txt"});
    glob("rgignore-beats-ignore", {".rgignore=!keep.log\n", ".ignore=*.log\n", "keep.log", "a.log"}, all, {".ignore", ".rgignore", "keep.log"});
    const QStringList ts{"a.ts", "src/b.ts", "src/x/c.ts", ".h.ts", "a.tsx", "c.js"};
    glob("glob-any-depth", ts, {{"pattern", "*.ts"}}, {".h.ts", "a.ts", "src/b.ts", "src/x/c.ts"});
    glob("glob-anchored", ts, {{"pattern", "src/*.ts"}}, {"src/b.ts"});
    glob("glob-leading-slash", ts, {{"pattern", "/src/*.ts"}}, {"src/b.ts"});
    glob("glob-dot-segment", ts, {{"pattern", "./src/*.ts"}}, {});
    glob("glob-recursive-suffix", ts, {{"pattern", "src/**"}}, {"src/b.ts", "src/x/c.ts"});
    glob("glob-braces", ts, {{"pattern", "*.{ts,tsx}"}}, {".h.ts", "a.ts", "a.tsx", "src/b.ts", "src/x/c.ts"});
    glob("glob-nested-braces", ts, {{"pattern", "*.{ts,{tsx,js}}"}}, {".h.ts", "a.ts", "a.tsx", "c.js", "src/b.ts", "src/x/c.ts"});
    glob("glob-case-sensitive", ts, {{"pattern", "*.TS"}}, {});
    glob("glob-directory-name", ts, {{"pattern", "src"}}, {});
    glob("glob-exclusion-only", ts, {{"pattern", "!*.ts"}}, {"a.tsx", "c.js"});
    glob("glob-comment", ts, {{"pattern", "#*.ts"}}, {".h.ts", "a.ts", "a.tsx", "c.js", "src/b.ts", "src/x/c.ts"});
    glob("glob-escaped-backslash", ts, {{"pattern", "src\\*.ts"}}, {});
    const QStringList classes{"file1.txt", "a.txt", "b.txt", "[x].txt", "ab.txt"};
    glob("glob-question", classes, {{"pattern", "file?.txt"}}, {"file1.txt"});
    glob("glob-class", classes, {{"pattern", "[ab].txt"}}, {"a.txt", "b.txt"});
    glob("glob-negated-class", classes, {{"pattern", "[!a].txt"}}, {"b.txt"});
    glob("glob-escaped-class", classes, {{"pattern", "\\[x\\].txt"}}, {"[x].txt"});
    glob("glob-hidden", {".github/w/ci.yml", ".env.yml", "a.yml"}, {{"pattern", "*.yml"}}, {".env.yml", ".github/w/ci.yml", "a.yml"});
    glob("glob-reincludes-ignored", {".gitignore=*.log\n", "a.log", "d/b.log"}, {{"pattern", "**/*.log"}}, {"a.log", "d/b.log"});
    glob("glob-ignored-directory-stays-pruned", {".gitignore=gen/\n", "gen/a.js"}, {{"pattern", "gen/**"}}, {});
    glob("glob-include-gitignored", {".gitignore=*.log\n", "node_modules/m/i.js", "a.log"}, {{"pattern", "**"}, {"include_gitignored", true}}, {".gitignore", "a.log", "node_modules/m/i.js"});
    glob("glob-no-fallback-defaults", {"dist/a.js", "build/b.js", "out/c.js", ".vscode/s.json", "a.log"}, all, {".vscode/s.json", "a.log", "build/b.js", "dist/a.js", "out/c.js"});
    glob("policy-git-never-listed", {"a.txt", "x/.git/HEAD", "w/.git=gitdir: x\n"}, {{"pattern", "**/*"}}, {"a.txt"});
    glob("policy-git-with-include-gitignored", {"a.txt", "x/.git/HEAD"}, {{"pattern", "**/*"}, {"include_gitignored", true}}, {"a.txt"});
    glob("policy-node-modules-hidden", {"node_modules/m/i.js", "a.js"}, all, {"a.js"});
    glob("policy-node-modules-reincluded", {".gitignore=!node_modules/\n", "node_modules/a.js"}, all, {".gitignore", "node_modules/a.js"});
    const QStringList hidden{".env", ".github/w/ci.yml", "a.txt", ".hdir/z.txt"};
    grep("grep-skips-hidden", hidden, {}, {"a.txt"});
    grep("grep-skips-hidden-without-ignores", hidden, {{"include_gitignored", true}}, {"a.txt"});
    grep("grep-include-names-hidden-file", hidden, {{"include_file_pattern", ".env"}}, {".env"});
    grep("grep-hidden-directory-not-entered", hidden, {{"include_file_pattern", "*.yml"}}, {});
    grep("grep-hidden-directory-glob", hidden, {{"include_file_pattern", ".github/**"}}, {});
    grep("grep-negation-unhides-file", QStringList(hidden) << ".gitignore=!.env\n", {}, {".env", "a.txt"});
    grep("grep-negation-unhides-directory", QStringList(hidden) << ".gitignore=!.github/\n", {}, {".github/w/ci.yml", "a.txt"});
    grep("grep-exclude-any-depth", {"test-a.js", "d/test-b.js", "a.js"}, {{"exclude_file_pattern", "**/test-*.js"}}, {"a.js"});
    grep("grep-exclude-basename", {"test-a.js", "d/test-b.js", "a.js"}, {{"exclude_file_pattern", "test-*.js"}}, {"a.js"});
    grep("grep-exclude-anchored-contents", {"metadata/a.js", "metadata/s/b.js", "x/metadata/c.js", "a.js"}, {{"exclude_file_pattern", "metadata/**"}}, {"a.js", "x/metadata/c.js"});
    grep("grep-exclude-directory-name", {"metadata/a.js", "x/metadata/c.js", "a.js"}, {{"exclude_file_pattern", "metadata"}}, {"a.js"});
    grep("grep-exclude-after-include", {"vendor/a.js", "b.js", "c.ts"}, {{"include_file_pattern", "*.js"}, {"exclude_file_pattern", "vendor/**"}}, {"b.js"});
    grep("grep-exclude-wins-last", {"vendor/a.js", "b.js"}, {{"include_file_pattern", "vendor/**"}, {"exclude_file_pattern", "*.js"}}, {});
    grep("grep-exclude-with-include-gitignored", {".gitignore=gen/\n", "gen/a.js", "gen/test-b.js"}, {{"exclude_file_pattern", "**/test-*.js"}, {"include_gitignored", true}}, {"gen/a.js"});
    grep("grep-include-does-not-whitelist-directory", {".gitignore=gen/\n", "gen/a.js"}, {{"include_file_pattern", "gen/**"}}, {});
    grep("grep-negated-include-excludes", {"a.md", "b.txt"}, {{"include_file_pattern", "!*.md"}}, {"b.txt"});
    grep("grep-include-reincludes-ignored", {".gitignore=*.log\n", "a.log", "b.txt"}, {{"include_file_pattern", "*.log"}}, {"a.log"});
    grep("grep-exclude-directory-slash", {"build/a.js", "src/build/b.js", "c.js"}, {{"exclude_file_pattern", "build/"}}, {"c.js"});
    grep("grep-empty-patterns-absent", {"a.js", "b.ts"}, {{"include_file_pattern", ""}, {"exclude_file_pattern", ""}}, {"a.js", "b.ts"});
    grep("grep-nested-repo-boundary", {".git/HEAD", "sub/.git/HEAD", ".gitignore=*.log\n", "a.log", "b.txt", "sub/c.log", "sub/d.txt"}, {}, {"b.txt", "sub/c.log", "sub/d.txt"});
    grep("policy-grep-git-never-searched", {"a.txt", ".git/HEAD"}, {{"include_file_pattern", "**"}}, {"a.txt"});
    grep("policy-grep-node-modules-hidden", {"node_modules/a.js", "b.js"}, {}, {"b.js"});
    grep("grep-node-modules-include-gitignored", {"node_modules/a.js", "b.js"}, {{"include_gitignored", true}}, {"b.js", "node_modules/a.js"});
  }
  void fixtures() {
    QFETCH(QString, tool);
    QFETCH(QStringList, tree);
    QFETCH(QJsonObject, args);
    QFETCH(QStringList, expected);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(writeTree(root.path(), tree));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result = run(tools, tool, args);
    QVERIFY2(complete(result), qPrintable(output(result)));
    QVERIFY2(!output(result).contains("Ignore file"), qPrintable(output(result)));
    expected.sort();
    QCOMPARE(paths(output(result), tool == "grepSearch"), expected);
  }
  void invalidArguments_data() {
    QTest::addColumn<QString>("tool");
    QTest::addColumn<QJsonObject>("args");
    QTest::addColumn<QString>("message");
    QTest::newRow("glob-include-gitignored-type")
        << "glob" << QJsonObject{{"pattern", "**"}, {"include_gitignored", "yes"}}
        << "include_gitignored must be a boolean";
    QTest::newRow("grep-include-gitignored-type")
        << "grepSearch" << QJsonObject{{"query", "x"}, {"include_gitignored", 1}}
        << "include_gitignored must be a boolean";
    QTest::newRow("grep-exclude-type")
        << "grepSearch" << QJsonObject{{"query", "x"}, {"exclude_file_pattern", 7}}
        << "exclude_file_pattern must be a string";
    QTest::newRow("glob-unclosed-class")
        << "glob" << QJsonObject{{"pattern", "[ab.txt"}}
        << "Invalid pattern: unclosed character class";
    QTest::newRow("grep-unclosed-brace")
        << "grepSearch" << QJsonObject{{"query", "x"}, {"include_file_pattern", "*.{ts"}}
        << "Invalid include_file_pattern: unclosed alternate group";
    QTest::newRow("grep-exclude-dangling-escape")
        << "grepSearch" << QJsonObject{{"query", "x"}, {"exclude_file_pattern", "a\\"}}
        << "Invalid exclude_file_pattern: dangling";
    QTest::newRow("glob-invalid-range")
        << "glob" << QJsonObject{{"pattern", "[z-a].ts"}}
        << "Invalid pattern: invalid character range";
  }
  void invalidArguments() {
    QFETCH(QString, tool);
    QFETCH(QJsonObject, args);
    QFETCH(QString, message);
    QTemporaryDir root;
    QVERIFY(writeTree(root.path(), {"a.ts"}));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result = run(tools, tool, args);
    QVERIFY2(failed(result), qPrintable(output(result)));
    QVERIFY2(output(result).contains(message), qPrintable(output(result)));
  }
  // Upstream universal-tools.test.ts "skips noisy gitignored directories by
  // default" and "plumbs include_gitignored through to runtime-node".
  void upstreamIncludeGitignoredContract() {
    QTemporaryDir root;
    QVERIFY(writeTree(root.path(),
                      {"README.md", "pkg/README.md",
                       "node_modules/foo/README.md=should be ignored\n",
                       ".gitignore=secrets/\n", "secrets/leak.md=shh\n"}));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    auto result = run(tools, "glob", {{"pattern", "**/README.md"}});
    QVERIFY(complete(result));
    QCOMPARE(paths(output(result), false),
             QStringList({"README.md", "pkg/README.md"}));
    result = run(tools, "glob",
                 {{"pattern", "**/README.md"}, {"include_gitignored", true}});
    QVERIFY(paths(output(result), false).contains("node_modules/foo/README.md"));
    result = run(tools, "glob", {{"pattern", "**/leak.md"}});
    QVERIFY(complete(result));
    QVERIFY(!output(result).contains("secrets/leak.md"));
    result = run(tools, "glob",
                 {{"pattern", "**/leak.md"}, {"include_gitignored", true}});
    QCOMPARE(paths(output(result), false), QStringList({"secrets/leak.md"}));
    result = run(tools, "grepSearch", {{"query", "shh"}});
    QCOMPARE(output(result), QString("No matches."));
    result = run(tools, "grepSearch",
                 {{"query", "shh"}, {"include_gitignored", true}});
    QCOMPARE(output(result), QString("secrets/leak.md:1: shh\n"));
  }
  void ancestorRulesApplyButStartIsNeverPruned() {
    QTemporaryDir root;
    QVERIFY(writeTree(root.path(), {".gitignore=*.log\n/sub/skip/\nsub/\n",
                                    "sub/a.log", "sub/b.txt",
                                    "sub/skip/c.txt", "sub/keep/d.txt"}));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result = run(tools, "glob", {{"path", "sub"}, {"pattern", "**"}});
    QVERIFY2(complete(result), qPrintable(output(result)));
    QCOMPARE(paths(output(result), false), QStringList({"b.txt", "keep/d.txt"}));
  }
  // rg 15.0.0 run from the start directory: ancestor .gitignore rules stop
  // at the nearest directory holding .git, even above the search start.
  void nestedRepositoryStopsAncestorRulesAboveStart() {
    QTemporaryDir root;
    QVERIFY(writeTree(root.path(), {".git/HEAD", ".gitignore=*.log\n",
                                    "repo/.git/HEAD", "repo/inner/a.log",
                                    "repo/inner/b.txt", "plain/inner/a.log",
                                    "plain/inner/b.txt"}));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    auto result =
        run(tools, "glob", {{"path", "repo/inner"}, {"pattern", "**"}});
    QVERIFY2(complete(result), qPrintable(output(result)));
    QCOMPARE(paths(output(result), false), QStringList({"a.log", "b.txt"}));
    result = run(tools, "glob", {{"path", "plain/inner"}, {"pattern", "**"}});
    QVERIFY2(complete(result), qPrintable(output(result)));
    QCOMPARE(paths(output(result), false), QStringList({"b.txt"}));
  }
  // The wall-clock bound ends the walk with the ordinary limit note. A zero
  // limit has expired before the first directory is read.
  void searchTimeLimitEndsWalk() {
    QTemporaryDir root;
    for (int i = 0; i < 30; ++i)
      QVERIFY(writeFile(root.filePath(QString("d%1/f.txt").arg(i, 2, 10, QChar('0'))),
                        "x\n"));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    tools.setSearchTimeLimit(0);
    for (const auto &tool : {QString("grepSearch"), QString("glob")}) {
      const auto args = tool == "glob"
                            ? QJsonObject{{"pattern", "**"}}
                            : QJsonObject{{"query", "x"}, {"max_matches", 50}};
      const auto result = run(tools, tool, args);
      QVERIFY2(complete(result), qPrintable(output(result)));
      QCOMPARE(output(result),
               QString("\n[Search limit reached; narrow the query.]"));
    }
    tools.setSearchTimeLimit(20000);
    const auto result =
        run(tools, "grepSearch", {{"query", "x"}, {"max_matches", 50}});
    QVERIFY2(complete(result), qPrintable(output(result)));
    QCOMPARE(paths(output(result), true).size(), 30);
    QVERIFY(!output(result).contains("Search limit reached"));
  }
  void ignoreFileDiagnostics() {
    QTemporaryDir root;
    QVERIFY(writeTree(root.path(), {"sub/.gitignore={a\n*.js\n", "sub/{a",
                                    "sub/c.js", "sub/d.ts",
                                    "skip/.gitignore=[z-a]\n", "skip/e.ts"}));
    QVERIFY(writeFile(root.filePath("u/.gitignore"),
                      QByteArray("*.log\n\xff\n*.tmp\n")));
    QVERIFY(writeTree(root.path(), {"u/a.log", "u/a.tmp", ".gitignore=skip/\n"}));
    QVERIFY(QDir().mkpath(root.filePath("d/.gitignore")));
    QVERIFY(writeTree(root.path(), {"d/f.txt"}));
    QVERIFY(writeFile(root.filePath("big/.gitignore"),
                      "*.log\n" + QByteArray(300 * 1024, '#')));
    QVERIFY(writeTree(root.path(), {"big/a.log"}));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    auto result = run(tools, "glob", {{"pattern", "**"}});
    QVERIFY2(complete(result), qPrintable(output(result)));
    const auto text = output(result);
    QCOMPARE(paths(text, false),
             QStringList({".gitignore", "big/.gitignore", "big/a.log", "d/f.txt",
                          "sub/.gitignore", "sub/d.ts", "sub/{a",
                          "u/.gitignore", "u/a.tmp"}));
    QVERIFY2(text.contains("[Ignore file sub/.gitignore line 1 skipped: "
                           "unclosed alternate group; missing '}'.]"),
             qPrintable(text));
    QVERIFY2(text.contains("[Ignore file u/.gitignore: line 2 is not valid "
                           "UTF-8; it and later lines were not applied.]"),
             qPrintable(text));
    QVERIFY2(text.contains("[Ignore file d/.gitignore not applied: not a "
                           "regular file.]"),
             qPrintable(text));
    QVERIFY2(text.contains("[Ignore file big/.gitignore not applied: exceeds "
                           "256 KiB.]"),
             qPrintable(text));
    // The ignored skip/ directory is never opened, so its rules are unread.
    QVERIFY2(!text.contains("skip/.gitignore"), qPrintable(text));
    // include_gitignored opens no rule file at all.
    result = run(tools, "glob", {{"pattern", "**"}, {"include_gitignored", true}});
    QVERIFY(complete(result));
    QVERIFY2(!output(result).contains("Ignore file"), qPrintable(output(result)));
    QVERIFY(paths(output(result), false).contains("skip/e.ts"));
  }
  void excludedPrefixIsNeverOpened() {
    QTemporaryDir root;
    QVERIFY(writeTree(root.path(), {"metadata/.gitignore=[z-a]\n",
                                    "metadata/a.js", "b.js"}));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result = run(tools, "grepSearch",
                            {{"query", "x"},
                             {"exclude_file_pattern", "metadata/**"}});
    QVERIFY(complete(result));
    QCOMPARE(output(result), QString("b.js:1: x\n"));
  }
  void globReturnsAtMostFiftyPaths_data() {
    QTest::addColumn<int>("files");
    QTest::addColumn<bool>("truncated");
    QTest::newRow("exactly-50") << 50 << false;
    QTest::newRow("120") << 120 << true;
  }
  void globReturnsAtMostFiftyPaths() {
    QFETCH(int, files);
    QFETCH(bool, truncated);
    QTemporaryDir root;
    for (int i = 0; i < files; ++i)
      QVERIFY(writeFile(root.filePath(QString("f%1.ts").arg(i, 3, 10, QChar('0'))),
                        "x\n"));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result = run(tools, "glob", {{"pattern", "**/*.ts"}});
    QVERIFY(complete(result));
    QCOMPARE(paths(output(result), false).size(), 50);
    QCOMPARE(output(result).endsWith("[Search limit reached; narrow the query.]"),
             truncated);
  }
  void ignoredNamesDoNotConsumeEntryBudget() {
    QTemporaryDir root;
    QVERIFY(writeTree(root.path(), {".gitignore=*.log\n"}));
    QVERIFY(QDir().mkpath(root.filePath("logs")));
    for (int i = 0; i < 10050; ++i) {
      QFile file(root.filePath(QString("logs/%1.log").arg(i)));
      QVERIFY(file.open(QIODevice::WriteOnly));
    }
    for (int i = 0; i < 5; ++i)
      QVERIFY(writeFile(root.filePath(QString("z/keep%1.txt").arg(i)), "x\n"));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result = run(tools, "glob", {{"pattern", "**"}});
    QVERIFY(complete(result));
    QCOMPARE(output(result),
             QString(".gitignore\nz/keep0.txt\nz/keep1.txt\nz/keep2.txt\n"
                     "z/keep3.txt\nz/keep4.txt\n"));
  }
  void windowsHiddenAttributeIsHiddenForGrepOnly() {
#ifdef Q_OS_WIN
    QTemporaryDir root;
    QVERIFY(writeTree(root.path(), {"h.txt", "hd/a.txt", "v.txt"}));
    for (const auto &name : {QString("h.txt"), QString("hd")})
      QVERIFY(SetFileAttributesW(
          reinterpret_cast<const wchar_t *>(
              QDir::toNativeSeparators(root.filePath(name)).utf16()),
          FILE_ATTRIBUTE_HIDDEN));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    auto result = run(tools, "grepSearch", {{"query", "x"}});
    QVERIFY(complete(result));
    QCOMPARE(paths(output(result), true), QStringList({"v.txt"}));
    result = run(tools, "glob", {{"pattern", "**"}});
    QCOMPARE(paths(output(result), false),
             QStringList({"h.txt", "hd/a.txt", "v.txt"}));
#else
    QSKIP("Windows file attribute fixture.");
#endif
  }
};
QTEST_GUILESS_MAIN(SearchIgnoreTest)
#include "test_search_ignore.moc"
