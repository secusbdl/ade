// Real files and native reads, with growth scheduled after the validated size
// query. This reproduces discarded overflowing chunks escaping the I/O budget.
#include "core/ToolExecutor.h"
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <windows.h>

namespace {
QString fixtureRoot;
QSet<QString> pendingGrowth, grown;
qint64 actualReadBytes = 0;
QString hookError;
QString filePath(HANDLE handle) {
  wchar_t data[32768];
  const DWORD length = ::GetFinalPathNameByHandleW(
      handle, data, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (!length || length >= 32768)
    return {};
  auto path = QDir::fromNativeSeparators(QString::fromWCharArray(data, length));
  if (path.startsWith("//?/"))
    path.remove(0, 4);
  return path;
}
BOOL WINAPI boundaryGetFileSizeEx(HANDLE file, PLARGE_INTEGER size) {
  const auto path = filePath(file);
  if (!fixtureRoot.isEmpty() && path.startsWith(fixtureRoot + '/') &&
      actualReadBytes >= 16 * 500 * 1024 && !pendingGrowth.contains(path) &&
      !grown.contains(path)) {
    QFile writer(path);
    if (!writer.resize(1))
      hookError =
          "Cannot shrink the owned fixture before initial size validation.";
    pendingGrowth.insert(path);
  }
  return ::GetFileSizeEx(file, size);
}
BOOL WINAPI boundaryReadFile(HANDLE file, LPVOID buffer, DWORD requested,
                             LPDWORD read, LPOVERLAPPED overlap) {
  const auto path = filePath(file);
  if (pendingGrowth.remove(path)) {
    QFile writer(path);
    if (!writer.resize(512 * 1024))
      hookError = "Cannot grow the owned fixture after size validation.";
    grown.insert(path);
  }
  const BOOL result = ::ReadFile(file, buffer, requested, read, overlap);
  if (!fixtureRoot.isEmpty() && path.startsWith(fixtureRoot + '/') && read)
    actualReadBytes += *read;
  return result;
}
} // namespace

#define GetFileSizeEx boundaryGetFileSizeEx
#define ReadFile boundaryReadFile
#ifndef SEARCH_TOOL_EXECUTOR
#define SEARCH_TOOL_EXECUTOR "../src/core/ToolExecutor.cpp"
#endif
#include SEARCH_TOOL_EXECUTOR
#undef ReadFile
#undef GetFileSizeEx

class SearchReadBudget final : public QObject {
  Q_OBJECT
private slots:
  void growthCannotExceedAggregateReadLimit() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    for (int i = 0; i < 20; ++i) {
      QFile file(
          root.filePath(QString("file%1.txt").arg(i, 2, 10, QChar('0'))));
      QVERIFY(file.open(QIODevice::WriteOnly));
      QCOMPARE(file.write(QByteArray(500 * 1024, 'a')), 500 * 1024);
    }
    ade::ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    fixtureRoot = tools.workspace();
    actualReadBytes = 0;
    hookError.clear();
    pendingGrowth.clear();
    grown.clear();
    const auto reset = qScopeGuard([] { fixtureRoot.clear(); });
    QSignalSpy results(&tools, &ade::ToolExecutor::completed);
    tools.execute("growth", "grepSearch", {{"query", "needle"}});
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QVERIFY2(hookError.isEmpty(), qPrintable(hookError));
    QVERIFY(!grown.isEmpty());
    QVERIFY2(actualReadBytes <= 8 * 1024 * 1024,
             qPrintable(
                 QString("Actual native read bytes: %1").arg(actualReadBytes)));
    QVERIFY(!results[0][2].toBool());
    QVERIFY(results[0][1].toString().contains("Search incomplete"));
  }
};
QTEST_GUILESS_MAIN(SearchReadBudget)
#include "test_search_read_budget.moc"
