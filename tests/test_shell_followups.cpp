#include "shell/SessionLogStore.h"
#include "shell/ShellProtocol.h"
#include "shell/ShellService.h"
#include "terminal/ConPtyProcess.h"
#include "terminal/TerminalModel.h"
#include "terminal/TerminalWidget.h"
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
using namespace ade;
namespace {
std::shared_ptr<SessionLogStore> store(const QString &root) {
  QDir().mkpath(root);
  return std::make_shared<SessionLogStore>(root, "chat");
}
struct Fixture {
  QTemporaryDir directory;
  std::shared_ptr<SessionLogStore> logs = store(directory.path() + "/data");
  ShellService service{"chat", logs};
  QSignalSpy results{&service, &ShellService::completed};
  Fixture() {
    QDir().mkpath(directory.path() + "/workspace/sub");
    service.setMounts({{"ws1", directory.path() + "/workspace"}});
  }
  QJsonObject result(const QString &request) const {
    for (const auto &row : results)
      if (row[0].toString() == request)
        return QJsonDocument::fromJson(row[1].toString().toUtf8()).object();
    return {};
  }
  QString failure(const QString &request) const {
    for (const auto &row : results)
      if (row[0].toString() == request && row[2].toBool())
        return row[1].toString();
    return {};
  }
  void command(const QString &request, const QString &session,
               const QString &text, int timeout = 5000) {
    service.execute(
        request, "executeShellCommand",
        {{"session_id", session},
         {"explanation", "Local test fixture"},
         {"command", text},
         {"wait_until", QJsonObject{{"timeout_ms", timeout}, {"idle_ms", 0}}}});
  }
};
QByteArray osc(const QByteArray &value) { return "\x1b]" + value + '\a'; }
} // namespace

class ShellFollowups : public QObject {
  Q_OBJECT
private slots:

  void withoutReadLine_data() {
    QTest::addColumn<QString>("mode");
    QTest::newRow("pattern") << QString("pattern");
    QTest::newRow("idle") << QString("idle");
    QTest::newRow("echo-negative") << QString("echo");
    QTest::newRow("raw") << QString("raw");
    QTest::newRow("poll") << QString("poll");
  }
  void withoutReadLine() {
    QFETCH(QString, mode);
    QTemporaryDir wrapper;
    QVERIFY(wrapper.isValid());
    // Mirror ShellService::shellExecutable(): PowerShell 7 when installed,
    // otherwise the Windows PowerShell that ADE actually runs.
    auto real = QStandardPaths::findExecutable("pwsh.exe");
    if (real.isEmpty())
      real = QStandardPaths::findExecutable("powershell.exe");
    QVERIFY(!real.isEmpty());
    QVERIFY(QFile::copy(QCoreApplication::applicationDirPath() +
                            "/shell_fallback_fixture.exe",
                        wrapper.path() + "/pwsh.exe"));
    const auto oldPath = qgetenv("PATH"),
               oldReal = qgetenv("ADE_TEST_REAL_POWERSHELL");
    auto env = qScopeGuard([&] {
      qputenv("PATH", oldPath);
      if (oldReal.isNull())
        qunsetenv("ADE_TEST_REAL_POWERSHELL");
      else
        qputenv("ADE_TEST_REAL_POWERSHELL", oldReal);
    });
    qputenv("ADE_TEST_REAL_POWERSHELL", real.toUtf8());
    qputenv("PATH", wrapper.path().toUtf8() + ";" + oldPath);
    Fixture f;
    f.service.execute("create", "createShellSession", {{"cwd", "ws1"}});
    QTRY_VERIFY_WITH_TIMEOUT(
        !f.result("create").isEmpty() || !f.failure("create").isEmpty(), 15000);
    QVERIFY2(f.failure("create").isEmpty(), qPrintable(f.failure("create")));
    const auto id = f.result("create")["session_id"].toString();
    QVERIFY(!id.isEmpty());
    QSignalSpy snapshots(&f.service, &ShellService::outputSnapshot);
    QJsonObject wait{{"timeout_ms", 1800}, {"idle_ms", 0}};
    QString command;
    if (mode == "echo") {
      command = "$value='ECHO_ONLY'; Start-Sleep 3";
      wait["output_pattern"] = "ECHO_ONLY";
    } else {
      command = "Write-Output ('FALLBACK_' + 'READY'); Start-Sleep 3";
      if (mode == "pattern" || mode == "raw" || mode == "poll")
        wait["output_pattern"] = "FALLBACK_READY";
      else
        wait["idle_ms"] = 250;
    }
    if (mode == "poll")
      command += "; Write-Output ('POLL_'+'DONE')";
    QJsonObject args{{"session_id", id},
                     {"explanation", "No PSReadLine fixture"},
                     {"wait_until", wait}};
    if (mode == "raw")
      args["stdin"] = command + "\\r";
    else
      args["command"] = command;
    f.service.execute("run", "executeShellCommand", args);
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("run").isEmpty(), 6000);
    const auto expected = mode == "echo"   ? QString("timeout")
                          : mode == "idle" ? QString("idle")
                                           : QString("pattern");
    QCOMPARE(f.result("run")["resolved_by"].toString(), expected);
    if (mode == "poll") {
      f.service.execute(
          "poll", "executeShellCommand",
          {{"session_id", id},
           {"explanation", "Continue existing output region"},
           {"wait_until", QJsonObject{{"output_pattern", "POLL_DONE"},
                                      {"timeout_ms", 5000},
                                      {"idle_ms", 0}}}});
      QTRY_VERIFY_WITH_TIMEOUT(!f.result("poll").isEmpty(), 7000);
      QCOMPARE(f.result("poll")["resolved_by"].toString(), QString("pattern"));
      QVERIFY(f.result("poll")["output"].toString().contains("POLL_DONE"));
    }
    if (mode != "echo")
      QVERIFY(f.result("run")["output"].toString().contains("FALLBACK_READY"));
    if (mode == "idle") {
      QVERIFY(!snapshots.isEmpty());
      QVERIFY(snapshots.last()[1].toString().contains("FALLBACK_READY"));
    }
  }
  void watcherCmdletFailure_data() {
    QTest::addColumn<QString>("command");
    QTest::newRow("final-write-error")
        << QString("Write-Error 'LOCAL_FIXTURE_ERROR'");
    QTest::newRow("native-success-then-write-error")
        << QString("cmd.exe /d /c exit 0; Write-Error 'LOCAL_FIXTURE_ERROR'");
    QTest::newRow("failed-cmdlet")
        << QString("Get-Item -LiteralPath './missing-fixture-item'");
  }
  void watcherCmdletFailure() {
    QFETCH(QString, command);
    Fixture f;
    QSignalSpy wakes(&f.service, &ShellService::watcherCompleted);
    f.service.execute("watch", "createWatcherSession",
                      {{"cwd", "ws1"},
                       {"title", "Failure fixture"},
                       {"command", command},
                       {"timeout_ms", 15000}});
    QTRY_COMPARE_WITH_TIMEOUT(wakes.size(), 1, 15000);
    const auto result = wakes[0][0].toJsonObject();
    QCOMPARE(result["outcome"].toString(), QString("failed"));
    QCOMPARE(result["exitCode"].toInt(), 1);
  }

  void watcherExitPolicy_data() {
    QTest::addColumn<QString>("command");
    QTest::addColumn<int>("code");
    QTest::newRow("native-failure") << QString("cmd.exe /d /c exit 7") << 7;
    QTest::newRow("native-failure-then-output-retains-code")
        << QString("cmd.exe /d /c exit 7; Write-Output recovered") << 7;
    QTest::newRow("handled-nonterminal-error")
        << QString("Write-Error 'LOCAL_EXPECTED'; Write-Output recovered") << 0;
    QTest::newRow("throw") << QString("throw 'LOCAL_THROW'") << 1;
    QTest::newRow("explicit-exit-wins")
        << QString("Write-Error 'LOCAL_EXPECTED'; exit 0") << 0;
  }
  void watcherExitPolicy() {
    QFETCH(QString, command);
    QFETCH(int, code);
    Fixture f;
    QSignalSpy wakes(&f.service, &ShellService::watcherCompleted);
    f.service.execute("watch", "createWatcherSession",
                      {{"cwd", "ws1"},
                       {"title", "Exit policy fixture"},
                       {"command", command},
                       {"timeout_ms", 15000}});
    QTRY_COMPARE_WITH_TIMEOUT(wakes.size(), 1, 15000);
    const auto result = wakes[0][0].toJsonObject();
    QCOMPARE(result["exitCode"].toInt(), code);
    QCOMPARE(result["outcome"].toString(),
             code ? QString("failed") : QString("triggered"));
  }
  void unicodeWatcherWithinSourceBound() {
    Fixture f;
    QSignalSpy wakes(&f.service, &ShellService::watcherCompleted);
    const auto tail = QStringLiteral(
        "\nWrite-Output ('UNICODE_'+'OK'); Write-Output '中🙂'; exit 0");
    const auto command =
        QString("#") + QString(65536 - 1 - tail.size(), QChar(0x4e2d)) + tail;
    QCOMPARE(command.size(), 65536);
    f.service.execute("watch", "createWatcherSession",
                      {{"cwd", "ws1"},
                       {"title", "Unicode bound"},
                       {"command", command},
                       {"timeout_ms", 30000}});
    QTRY_VERIFY_WITH_TIMEOUT(
        !f.result("watch").isEmpty() || !f.failure("watch").isEmpty(), 15000);
    QVERIFY2(f.failure("watch").isEmpty(), qPrintable(f.failure("watch")));
    QTRY_COMPARE_WITH_TIMEOUT(wakes.size(), 1, 30000);
    QCOMPARE(wakes[0][0].toJsonObject()["exitCode"].toInt(), 0);
    QCOMPARE(f.results.size(), 1);
    QVERIFY(
        wakes[0][0].toJsonObject()["output"].toString().contains("UNICODE_OK"));
    QVERIFY(wakes[0][0].toJsonObject()["output"].toString().contains(
        QStringLiteral("中🙂")));
  }
  void stdinEscapeExpansion() {
    QCOMPARE(ShellProtocol::expandStdinEscapes(
                 QStringLiteral("a\\x41\\r\\n\\t\\a\\b\\\\z")),
             QStringLiteral("aA\r\n\t\a\b\\z"));
    QCOMPARE(
        ShellProtocol::expandStdinEscapes(QStringLiteral("\\q\\xZZ\\x1\\")),
        QStringLiteral("\\q\\xZZ\\x1\\"));
    QCOMPARE(ShellProtocol::expandStdinEscapes(QStringLiteral("already\r\n")),
             QStringLiteral("already\r\n"));
  }
  void watcherRetainsExactProcessExitCode() {
    Fixture f;
    QSignalSpy wakes(&f.service, &ShellService::watcherCompleted);
    f.service.execute(
        "watch", "createWatcherSession",
        {{"cwd", "ws1"},
         {"title", "Exit code"},
         {"command",
          "Write-Output first\rStart-Sleep -Milliseconds 250; exit 7"},
         {"timeout_ms", 15000}});
    QTRY_COMPARE_WITH_TIMEOUT(wakes.size(), 1, 15000);
    QCOMPARE(wakes.first()[0].toJsonObject()["outcome"].toString(),
             QString("failed"));
    QCOMPARE(wakes.first()[0].toJsonObject()["exitCode"].toInt(), 7);
  }
  void echoMustNotMatchOutputPattern() {
    Fixture f;
    f.service.execute("create", "createShellSession", {{"cwd", "ws1"}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("create").isEmpty(), 15000);
    const auto id = f.result("create")["session_id"].toString();
    f.service.execute("pattern", "executeShellCommand",
                      {{"session_id", id},
                       {"explanation", "Synthetic shell regression"},
                       {"command", "$x='NEEDLE'; Start-Sleep 3"},
                       {"wait_until", QJsonObject{{"output_pattern", "NEEDLE"},
                                                  {"timeout_ms", 1500},
                                                  {"idle_ms", 0}}}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("pattern").isEmpty(), 8000);
    QCOMPARE(f.result("pattern")["resolved_by"].toString(), QString("timeout"));
  }
  void silenceDoesNotStartIdleClock() {
    Fixture f;
    f.service.execute("create", "createShellSession", {{"cwd", "ws1"}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("create").isEmpty(), 15000);
    const auto id = f.result("create")["session_id"].toString();
    f.service.execute("silence", "executeShellCommand",
                      {{"session_id", id},
                       {"explanation", "Synthetic shell regression"},
                       {"command", "Start-Sleep 7"}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("silence").isEmpty(), 13000);
    QCOMPARE(f.result("silence")["resolved_by"].toString(), QString("exit"));
  }
  void rawInputExpandsEnterEscape() {
    Fixture f;
    f.service.execute("create", "createShellSession", {{"cwd", "ws1"}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("create").isEmpty(), 15000);
    const auto id = f.result("create")["session_id"].toString();
    f.service.execute(
        "raw", "executeShellCommand",
        {{"session_id", id},
         {"explanation", "Synthetic shell regression"},
         {"stdin", "Write-Output ('RAW_' + 'OK')\\r"},
         {"wait_until", QJsonObject{{"timeout_ms", 4000}, {"idle_ms", 0}}}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("raw").isEmpty(), 8000);
    QCOMPARE(f.result("raw")["resolved_by"].toString(), QString("exit"));
    QVERIFY(f.result("raw")["output"].toString().contains("RAW_OK"));
  }
  void multilineWatcherWaitsForWholeScript() {
    Fixture f;
    QSignalSpy wakes(&f.service, &ShellService::watcherCompleted);
    f.service.execute("watch", "createWatcherSession",
                      {{"cwd", "ws1"},
                       {"title", "Multiline"},
                       {"command", "Write-Output armed\rStart-Sleep 3; exit 0"},
                       {"timeout_ms", 15000}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("watch").isEmpty(), 15000);
    QTest::qWait(1200);
    QCOMPARE(wakes.size(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(wakes.size(), 1, 8000);
    QCOMPARE(wakes.first()[0].toJsonObject()["outcome"].toString(),
             QString("triggered"));
  }
  void previewContainsOnlyCurrentCommand() {
    Fixture f;
    f.service.execute("create", "createShellSession", {{"cwd", "ws1"}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("create").isEmpty(), 15000);
    const auto id = f.result("create")["session_id"].toString();
    f.command("first", id, "Write-Output ('FIRST_' + 'MARKER')");
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("first").isEmpty(), 8000);
    QSignalSpy snapshots(&f.service, &ShellService::outputSnapshot);
    f.command("second", id,
              "Write-Output ('SECOND_' + 'MARKER'); Start-Sleep 2");
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("second").isEmpty(), 8000);
    QVERIFY(!snapshots.isEmpty());
    for (const auto &row : snapshots)
      if (row[0].toString() == "second")
        QVERIFY2(!row[1].toString().contains("FIRST_MARKER"),
                 qPrintable(row[1].toString()));
  }
};
QTEST_MAIN(ShellFollowups)
#include "test_shell_followups.moc"
