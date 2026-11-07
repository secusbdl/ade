#include "core/ToolExecutor.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QScopeGuard>
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
QVariantList run(ToolExecutor &tools, const QString &name,
                 const QJsonObject &args) {
  QSignalSpy completed(&tools, &ToolExecutor::completed);
  tools.execute("fixture", name, args);
  if (completed.isEmpty())
    completed.wait(10000);
  return completed.isEmpty() ? QVariantList{} : completed.first();
}
QString output(const QVariantList &result) {
  return result.value(1).toString();
}
bool complete(const QVariantList &result) {
  return result.size() == 4 && !result.at(2).toBool();
}
QByteArray utf16(const QString &text, bool littleEndian) {
  QByteArray result =
      littleEndian ? QByteArray::fromHex("fffe") : QByteArray::fromHex("feff");
  for (const auto unit : text) {
    const ushort value = unit.unicode();
    result.append(char(littleEndian ? value & 255 : value >> 8));
    result.append(char(littleEndian ? value >> 8 : value & 255));
  }
  return result;
}
} // namespace

class SearchFindings : public QObject {
  Q_OBJECT
private slots:
  void grepPattern_data() {
    QTest::addColumn<QString>("pattern");
    QTest::addColumn<bool>("rootHit");
    QTest::newRow("basename-any-depth") << QString("*.ts") << true;
    QTest::newRow("explicit-path") << QString("src/**/*.ts") << false;
    QTest::newRow("recursive") << QString("**/*.ts") << true;
  }
  void grepPattern() {
    QFETCH(QString, pattern);
    QFETCH(bool, rootHit);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(writeFile(root.filePath("src/a.ts"), "needle\n"));
    QVERIFY(writeFile(root.filePath("b.ts"), "needle\n"));
    QVERIFY(writeFile(root.filePath("src/skip.js"), "needle\n"));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result =
        run(tools, "grepSearch",
            {{"query", "needle"}, {"include_file_pattern", pattern}});
    QVERIFY(complete(result));
    QVERIFY2(output(result).contains("src/a.ts:1: needle"),
             qPrintable(output(result)));
    QCOMPARE(output(result).contains("b.ts:1: needle"), rootHit);
    QVERIFY(!output(result).contains("skip.js"));
  }
  void largeFileOmissionIsDisclosed() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(writeFile(root.filePath("large.txt"),
                      QByteArray(600 * 1024, 'a') + "\nneedle\n"));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result = run(tools, "grepSearch", {{"query", "needle"}});
    QVERIFY(complete(result));
    QVERIFY2(output(result).contains("needle") ||
                 output(result).contains("limit", Qt::CaseInsensitive) ||
                 output(result).contains("skipped", Qt::CaseInsensitive),
             qPrintable(output(result)));
    QVERIFY(output(result) != "No matches.");
  }
  void unreadableFileOmissionIsDisclosed() {
#ifdef Q_OS_WIN
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto path = root.filePath("held.txt");
    QVERIFY(writeFile(path, "needle\n"));
    HANDLE file = CreateFileW(reinterpret_cast<const wchar_t *>(path.utf16()),
                              GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(file != INVALID_HANDLE_VALUE);
    const auto close = qScopeGuard([&] { CloseHandle(file); });
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result = run(tools, "grepSearch", {{"query", "needle"}});
    QVERIFY(complete(result));
    QVERIFY2(output(result).contains("skipped", Qt::CaseInsensitive) ||
                 output(result).contains("incomplete", Qt::CaseInsensitive),
             qPrintable(output(result)));
#else
    QSKIP("Windows sharing fixture.");
#endif
  }
  void exhaustedReadBudgetIsDisclosed() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    for (int i = 0; i < 18; ++i)
      QVERIFY(writeFile(
          root.filePath(QString("part%1.txt").arg(i, 2, 10, QChar('0'))),
          QByteArray(500 * 1024, 'a')));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result = run(tools, "grepSearch", {{"query", "needle"}});
    QVERIFY(complete(result));
    QVERIFY2(output(result).contains("limit", Qt::CaseInsensitive) ||
                 output(result).contains("skipped", Qt::CaseInsensitive),
             qPrintable(output(result)));
  }
  void previewIsBounded() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(writeFile(root.filePath("minified.js"),
                      "needle" + QByteArray(10000, 'a') + "\n"));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result = run(tools, "grepSearch", {{"query", "needle"}});
    QVERIFY(complete(result));
    const auto prefix = QString("minified.js:1: ");
    QVERIFY(output(result).startsWith(prefix));
    const auto preview = output(result).mid(prefix.size()).section('\n', 0, 0);
    QCOMPARE(preview.size(), 500);
    QVERIFY(preview.endsWith("..."));
  }
  void textEncoding_data() {
    QTest::addColumn<QByteArray>("bytes");
    QTest::newRow("utf8") << QByteArray("import x\n");
    QTest::newRow("utf8-bom") << (QByteArray::fromHex("efbbbf") + "import x\n");
    QTest::newRow("utf16-le") << utf16("import x\n", true);
    QTest::newRow("utf16-be") << utf16("import x\n", false);
  }
  void textEncoding() {
    QFETCH(QByteArray, bytes);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(writeFile(root.filePath("a.ts"), bytes));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result = run(tools, "grepSearch", {{"query", "^import"}});
    QVERIFY(complete(result));
    QVERIFY2(output(result).contains("a.ts:1: import x"),
             qPrintable(output(result)));
  }
  void previewDoesNotSplitUnicode() {
    QTemporaryDir root;
    const auto line = QString(496, QChar('a')) + QString::fromUcs4(U"🙂") +
                      QString(100, QChar('z'));
    QVERIFY(writeFile(root.filePath("a.txt"), line.toUtf8()));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result = run(tools, "grepSearch", {{"query", "a"}});
    QVERIFY(complete(result));
    QCOMPARE(output(result),
             QString("a.txt:1: ") + QString(496, QChar('a')) + "...\n");
  }
  void matchesAndSkippedFilesRemainVisible() {
    QTemporaryDir root;
    QVERIFY(
        writeFile(root.filePath("a-large.txt"), QByteArray(600 * 1024, 'a')));
    QVERIFY(writeFile(root.filePath("b-small.txt"), "needle\n"));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result = run(tools, "grepSearch", {{"query", "needle"}});
    QVERIFY(complete(result));
    QVERIFY(output(result).contains("b-small.txt:1: needle"));
    QVERIFY(output(result).contains("Search incomplete: skipped 1 file(s)"));
  }
  void malformedUtf16IsExplicit() {
    QTemporaryDir root;
    QVERIFY(
        writeFile(root.filePath("odd.txt"), QByteArray::fromHex("fffe69006d")));
    QVERIFY(writeFile(root.filePath("surrogate.txt"),
                      QByteArray::fromHex("fffe00d8")));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result = run(tools, "grepSearch", {{"query", "needle"}});
    QVERIFY(complete(result));
    QVERIFY2(output(result).contains("Search incomplete: skipped 2 file(s)"),
             qPrintable(output(result)));
  }
  void binaryFilesRemainExcluded() {
    QTemporaryDir root;
    QVERIFY(writeFile(root.filePath("binary.dat"),
                      QByteArray("needle\0binary", 13)));
    QVERIFY(writeFile(root.filePath("utf16-zero.dat"),
                      utf16(QString("needle") + QChar(0), true)));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result = run(tools, "grepSearch", {{"query", "needle"}});
    QVERIFY(complete(result));
    QCOMPARE(output(result), QString("No matches."));
  }
  void matchLimitRemainsExplicit() {
    QTemporaryDir root;
    QVERIFY(writeFile(root.filePath("a.txt"),
                      QByteArray("needle\n").repeated(100)));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result =
        run(tools, "grepSearch", {{"query", "needle"}, {"max_matches", 10000}});
    QVERIFY(complete(result));
    QCOMPARE(output(result).count(": needle\n"), 50);
    QVERIFY(
        output(result).endsWith("[Search limit reached; narrow the query.]"));
    QVERIFY(output(result).size() <= 64 * 1024);
  }
  void exactAggregateBudgetCanFinishLastFile() {
    QTemporaryDir root;
    const auto bytes = QByteArray(512 * 1024 - 8, 'a') + "\nneedle\n";
    QCOMPARE(bytes.size(), 512 * 1024);
    for (int i = 0; i < 16; ++i)
      QVERIFY(writeFile(root.filePath(QString("part%1.txt").arg(i)), bytes));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result =
        run(tools, "grepSearch", {{"query", "needle"}, {"max_matches", 50}});
    QVERIFY(complete(result));
    QCOMPARE(output(result).count(":2: needle\n"), 16);
    QVERIFY(!output(result).contains("incomplete"));
    QVERIFY(!output(result).contains("limit"));
  }
  void outputLimitKeepsWholeRowsAndDiagnostic_data() {
    QTest::addColumn<QString>("tool");
    QTest::newRow("grep") << QString("grepSearch");
    QTest::newRow("glob") << QString("glob");
  }
  void outputLimitKeepsWholeRowsAndDiagnostic() {
    QFETCH(QString, tool);
    QTemporaryDir root;
    QString directory;
    for (int i = 0; i < 8; ++i)
      directory += QString(220, QChar('a' + i)) + '/';
    for (int i = 0; i < 50; ++i)
      QVERIFY(writeFile(
          root.filePath(directory +
                        QString("file%1.txt").arg(i, 2, 10, QChar('0'))),
          "needle\n"));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result = run(
        tools, tool,
        {{"query", "needle"}, {"max_matches", 50}, {"pattern", "**/*.txt"}});
    QVERIFY(complete(result));
    QVERIFY(output(result).size() <= 64 * 1024);
    QVERIFY(
        output(result).endsWith("[Search limit reached; narrow the query.]"));
    const auto rows = output(result).split('\n', Qt::SkipEmptyParts);
    QVERIFY(rows.size() > 1 && rows.size() < 51);
    for (qsizetype i = 0; i < rows.size() - 1; ++i)
      QVERIFY(rows[i].endsWith(tool == "glob" ? ".txt" : ".txt:1: needle"));
  }
  void unchangedGlobPatternAndHiddenFiles() {
    QTemporaryDir root;
    QVERIFY(writeFile(root.filePath("src/a.ts"), "needle"));
    QVERIFY(writeFile(root.filePath(".hidden.ts"), "needle"));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto result = run(tools, "glob", {{"pattern", "*.ts"}});
    QVERIFY(complete(result));
    QCOMPARE(output(result), QString(".hidden.ts\n"));
  }
  void caseAndLineNumbersRemainStable() {
    QTemporaryDir root;
    QVERIFY(writeFile(root.filePath("a.txt"), "first\nNeEdLe\r\nneedle\n"));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const auto insensitive = run(tools, "grepSearch", {{"query", "needle"}});
    const auto sensitive = run(tools, "grepSearch",
                               {{"query", "needle"}, {"case_sensitive", true}});
    QVERIFY(complete(insensitive));
    QVERIFY(complete(sensitive));
    QCOMPARE(output(insensitive),
             QString("a.txt:2: NeEdLe\r\na.txt:3: needle\n"));
    QCOMPARE(output(sensitive), QString("a.txt:3: needle\n"));
  }
  void mountIsolation_data() {
    QTest::addColumn<QString>("mount");
    QTest::newRow("second-workspace") << QString("ws2");
    QTest::newRow("attachment") << QString("att");
    QTest::newRow("shells") << QString("shells");
    QTest::newRow("escape") << QString("../outside");
  }
  void mountIsolation() {
    QFETCH(QString, mount);
    QTemporaryDir fixture;
    const auto workspace = fixture.filePath("owned");
    QVERIFY(QDir().mkpath(workspace));
    QVERIFY(
        writeFile(fixture.filePath("outside/SECRET.txt"), "outside-secret"));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(workspace));
    const auto result =
        run(tools, "grepSearch",
            {{"query", "outside-secret"}, {"mount_prefix", mount}});
    QCOMPARE(result.size(), 4);
    QVERIFY(result.at(2).toBool());
    QVERIFY(!output(result).contains("SECRET"));
  }
  void completionMayReplaceOrDeleteOwner() {
    QTemporaryDir root;
    QVERIFY(writeFile(root.filePath("a.txt"), "needle"));
    auto *tools = new ToolExecutor;
    QVERIFY(tools->setWorkspace(root.path()));
    QPointer<ToolExecutor> guard(tools);
    int calls = 0;
    QObject::connect(tools, &ToolExecutor::completed, tools,
                     [&](const QString &, const QString &, bool error) {
                       QVERIFY(!error);
                       if (++calls == 1)
                         tools->execute("second", "grepSearch",
                                        {{"query", "needle"}});
                       else
                         delete tools;
                     });
    tools->execute("first", "grepSearch", {{"query", "needle"}});
    QTRY_COMPARE_WITH_TIMEOUT(calls, 2, 5000);
    QVERIFY(guard.isNull());
  }
};
QTEST_GUILESS_MAIN(SearchFindings)
#include "test_search_findings.moc"
