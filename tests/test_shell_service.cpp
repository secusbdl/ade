#include "shell/SessionLogStore.h"
#include "shell/ShellProtocol.h"
#include "shell/ShellService.h"
#include "terminal/ConPtyProcess.h"
#include "terminal/TerminalModel.h"
#include "terminal/TerminalWidget.h"
#include <QJsonDocument>
#include <QPointer>
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
class ShellServiceTest : public QObject {
  Q_OBJECT
private slots:
  void boundaryChunksAndSpoofedCwd() {
    const QByteArray token = "0123456789abcdef";
    const auto data =
        osc("133;D;0;" + token) + osc("7;file:///D:/allowed") + osc("133;A") +
        "hello" + osc("7;file:///D:/evil") + osc("133;C") +
        osc("133;D;99;foreign") + osc("7;file:///D:/evil") + "world" +
        osc("133;D;7;" + token) + osc("7;file:///D:/next") + osc("133;A");
    for (int split = 0; split <= data.size(); ++split) {
      ShellProtocol parser(QString::fromLatin1(token));
      auto events = parser.feed(data.left(split));
      events += parser.feed(data.mid(split));
      QStringList paths;
      QList<int> codes;
      QByteArray output;
      int ready = 0;
      for (const auto &e : events) {
        if (e.kind == ShellProtocolEvent::WorkingDirectory)
          paths << e.text;
        if (e.kind == ShellProtocolEvent::CommandDone)
          codes << e.exitCode;
        if (e.kind == ShellProtocolEvent::Output)
          output += e.bytes;
        if (e.kind == ShellProtocolEvent::Ready)
          ++ready;
      }
      QCOMPARE(paths, QStringList({"D:/allowed", "D:/next"}));
      QCOMPARE(codes, QList<int>({7}));
      QCOMPARE(output, QByteArray("helloworld"));
      QCOMPARE(ready, 1);
    }
  }
  void waitBoundsAndEnvironment() {
    ShellWaitPolicy p;
    QString error;
    QVERIFY(ShellWaitPolicy::parse({}, &p, &error));
    QCOMPARE(p.timeoutMs, 10000);
    QVERIFY(ShellWaitPolicy::parse({{"stdin", ""}}, &p, &error));
    QCOMPARE(p.timeoutMs, 5000);
    QVERIFY(ShellWaitPolicy::parse(
        {{"wait_until", QJsonObject{{"exited", true}}}}, &p, &error));
    QCOMPARE(p.timeoutMs, 300000);
    for (const auto &wait : QList<QJsonObject>{{{"timeout_ms", 60001}},
                                               {{"timeout_ms", 0}},
                                               {{"idle_ms", 60001}},
                                               {{"timeout_ms", 2.5}},
                                               {{"output_pattern", "["}},
                                               {{"exited", "true"}}})
      QVERIFY(!ShellWaitPolicy::parse({{"wait_until", wait}}, &p, &error));
    QProcessEnvironment env;
    env.insert("PATH", "fixture-path");
    env.insert("SystemRoot", "C:/Windows");
    env.insert("FAKE_PROVIDER_KEY", "secret");
    env.insert("ELECTRON_RUN_AS_NODE", "1");
    env.insert("REGULAR_VALUE", "ordinary");
    env.insert("API_URL", "private");
    const auto safe = ShellProtocol::agentEnvironment(env);
    QCOMPARE(safe.value("PATH"), QString("fixture-path"));
    QCOMPARE(safe.value("SystemRoot"), QString("C:/Windows"));
    QVERIFY(!safe.contains("FAKE_PROVIDER_KEY"));
    QVERIFY(!safe.contains("ELECTRON_RUN_AS_NODE"));
    QVERIFY(!safe.contains("API_URL"));
    QCOMPARE(safe.value("REGULAR_VALUE"), QString("ordinary"));
    QCOMPARE(safe.value("HISTFILE"), QString("NUL"));
  }
  void persistentVariablesCwdAndLogs() {
    Fixture f;
    f.service.execute("create", "createShellSession", {{"cwd", "ws1"}});
    QTRY_VERIFY_WITH_TIMEOUT(
        !f.result("create").isEmpty() || !f.failure("create").isEmpty(), 15000);
    QVERIFY2(f.failure("create").isEmpty(), qPrintable(f.failure("create")));
    const auto id = f.result("create")["session_id"].toString();
    QVERIFY(!id.isEmpty());
    f.command("one", id,
              "$adeFixtureValue = 'PERSIST_947'; Set-Location sub; "
              "Write-Output 'FIRST_129'");
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("one").isEmpty(), 8000);
    QVERIFY2(f.result("one")["output"].toString().contains("FIRST_129"),
             qPrintable(QJsonDocument(f.result("one")).toJson()));
    QCOMPARE(f.result("one")["resolved_by"].toString(), QString("exit"));
    f.command("two", id,
              "Write-Output ($adeFixtureValue + '|' + (Split-Path "
              "(Get-Location) -Leaf))");
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("two").isEmpty(), 8000);
    QVERIFY2(f.result("two")["output"].toString().contains("PERSIST_947|sub"),
             qPrintable(QJsonDocument(f.result("two")).toJson()));
    QVERIFY(f.service.manifest().contains("/sub"));
    const auto bytes =
        f.logs->rawRead("shells/" + id + ".shell.log", 1024 * 1024);
    QVERIFY(bytes.contains("PERSIST_947|sub"));
    QVERIFY(!bytes.contains("133;D;"));
    f.service.closeAll();
  }
  void timeoutPollAndRawStdin() {
    Fixture f;
    f.service.execute("create", "createShellSession", {{"cwd", "ws1"}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("create").isEmpty(), 15000);
    const auto id = f.result("create")["session_id"].toString();
    f.command("slow", id,
              "Start-Sleep -Milliseconds 700; Write-Output 'FINISHED_LATER'",
              100);
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("slow").isEmpty(), 3000);
    QCOMPARE(f.result("slow")["resolved_by"].toString(), QString("timeout"));
    QVERIFY(!f.result("slow")["session_exited"].toBool());
    f.command("poll", id, "", 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("poll").isEmpty(), 5000);
    QCOMPARE(f.result("poll")["resolved_by"].toString(), QString("exit"));
    QVERIFY(f.result("poll")["output"].toString().contains("FINISHED_LATER"));
    f.command(
        "read", id,
        "$answer = Read-Host 'QUESTION'; Write-Output ('ANSWER:' + $answer)",
        150);
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("read").isEmpty(), 3000);
    f.service.execute(
        "stdin", "executeShellCommand",
        {{"session_id", id},
         {"explanation", "Fixture answer"},
         {"stdin", "native answer\r"},
         {"wait_until", QJsonObject{{"timeout_ms", 4000}, {"idle_ms", 0}}}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("stdin").isEmpty(), 6000);
    QVERIFY2(
        f.result("stdin")["output"].toString().contains("ANSWER:native answer"),
        qPrintable(QJsonDocument(f.result("stdin")).toJson()));
  }
  void cancelOwnedSessionAndRejectForeign() {
    Fixture f;
    f.service.execute(
        "foreign", "executeShellCommand",
        {{"session_id", "other-chat"}, {"explanation", "bad"}, {"kill", true}});
    QVERIFY(!f.failure("foreign").isEmpty());
    f.service.execute("escape", "createShellSession", {{"cwd", "ws1/../data"}});
    QVERIFY(!f.failure("escape").isEmpty());
    f.service.execute("create", "createShellSession", {{"cwd", "ws1"}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("create").isEmpty(), 15000);
    const auto id = f.result("create")["session_id"].toString();
    f.command("wait", id, "Start-Sleep -Seconds 60", 5000);
    f.service.cancel("wait");
    QCOMPARE(f.result("wait")["resolved_by"].toString(), QString("abort"));
    QVERIFY(f.result("wait")["session_exited"].toBool());
    f.command("after", id, "Write-Output 'MUST_NOT_RUN'");
    QCOMPARE(f.result("after")["resolved_by"].toString(),
             QString("session_exited"));
  }
  void watcherTriggersOnce() {
    Fixture f;
    QSignalSpy wakes(&f.service, &ShellService::watcherCompleted);
    f.service.execute("watch", "createWatcherSession",
                      {{"cwd", "ws1"},
                       {"title", "Fixture condition"},
                       {"command", "Write-Output 'CONDITION_MET'; exit 0"},
                       {"timeout_ms", 10000}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("watch").isEmpty(), 15000);
    QTRY_COMPARE_WITH_TIMEOUT(wakes.size(), 1, 5000);
    QCOMPARE(wakes[0][0].toJsonObject()["outcome"].toString(),
             QString("triggered"));
    QVERIFY(wakes[0][0].toJsonObject()["output"].toString().contains(
        "CONDITION_MET"));
    QTest::qWait(100);
    QCOMPARE(wakes.size(), 1);
  }
  void completionObserverMayDeleteOwner() {
    QTemporaryDir data;
    auto logs = store(data.path() + "/data");
    QPointer<ShellService> service = new ShellService("chat", logs);
    QVERIFY(service->setMounts({{"ws1", data.path()}}));
    connect(
        service, &ShellService::completed, this,
        [&](const QString &, const QString &, bool) { delete service.data(); });
    service->execute("create", "createShellSession", {{"cwd", "ws1"}});
    QTRY_VERIFY_WITH_TIMEOUT(!service, 15000);
  }
  void timeoutDoesNotDiscardPacedInput() {
    Fixture f;
    f.service.execute("create", "createShellSession", {{"cwd", "ws1"}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("create").isEmpty(), 15000);
    const auto id = f.result("create")["session_id"].toString();
    f.command("long", id,
              "$longFixture = '" + QString(4096, 'x') +
                  "'; Write-Output ('LENGTH:' + $longFixture.Length)",
              1);
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("long").isEmpty(), 3000);
    QCOMPARE(f.result("long")["resolved_by"].toString(), QString("timeout"));
    f.command("poll", id, "", 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("poll").isEmpty(), 7000);
    QVERIFY2(f.result("poll")["output"].toString().contains("LENGTH:4096"),
             qPrintable(QJsonDocument(f.result("poll")).toJson()));
    QCOMPARE(f.result("poll")["resolved_by"].toString(), QString("exit"));
  }
  void reentrantReplacementOwnsItsWait() {
    Fixture f;
    f.service.execute("create", "createShellSession", {{"cwd", "ws1"}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("create").isEmpty(), 15000);
    const auto id = f.result("create")["session_id"].toString();
    f.command("pending", id, "", 5000);
    connect(&f.service, &ShellService::completed, &f.service,
            [&](const QString &request) {
              if (request == "pending")
                f.command("inner", id, "Write-Output 'INNER_WINS'");
            });
    f.command("outer", id, "Write-Output 'OUTER_MUST_NOT_DISPATCH'");
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("inner").isEmpty(), 7000);
    QVERIFY(f.result("inner")["output"].toString().contains("INNER_WINS"));
    QVERIFY(!f.failure("outer").isEmpty());
    QVERIFY(!f.logs->rawRead("shells/" + id + ".shell.log", 1024 * 1024)
                 .contains("OUTER_MUST_NOT_DISPATCH"));
  }
  void patternAndIdleLeaveSessionRunning_data() {
    QTest::addColumn<bool>("pattern");
    QTest::newRow("pattern") << true;
    QTest::newRow("idle") << false;
  }
  void patternAndIdleLeaveSessionRunning() {
    QFETCH(bool, pattern);
    Fixture f;
    f.service.execute("create", "createShellSession", {{"cwd", "ws1"}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("create").isEmpty(), 15000);
    const auto id = f.result("create")["session_id"].toString();
    QJsonObject wait{{"timeout_ms", 4000}, {"idle_ms", pattern ? 0 : 150}};
    if (pattern)
      wait["output_pattern"] = "PATTERN_MATCH";
    f.service.execute(
        "wait", "executeShellCommand",
        {{"session_id", id},
         {"explanation", "Wait fixture"},
         {"command",
          "Write-Output ('PATTERN_' + 'MATCH'); Start-Sleep -Seconds 20"},
         {"wait_until", wait}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("wait").isEmpty(), 6000);
    QCOMPARE(f.result("wait")["resolved_by"].toString(),
             pattern ? QString("pattern") : QString("idle"));
    QVERIFY(!f.result("wait")["session_exited"].toBool());
    QVERIFY(f.service.snapshot()["sessions"]
                .toArray()[0]
                .toObject()["running"]
                .toBool());
  }
  void snapshotObserverMayCancelOrDelete_data() {
    QTest::addColumn<bool>("destroy");
    QTest::newRow("cancel") << false;
    QTest::newRow("destroy") << true;
  }
  void snapshotObserverMayCancelOrDelete() {
    QFETCH(bool, destroy);
    QTemporaryDir data;
    auto logs = store(data.path() + "/data");
    QPointer<ShellService> service = new ShellService("chat", logs);
    QVERIFY(service->setMounts({{"ws1", data.path()}}));
    QSignalSpy completed(service, &ShellService::completed);
    service->execute("create", "createShellSession", {{"cwd", "ws1"}});
    QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 15000);
    const auto id = QJsonDocument::fromJson(completed[0][1].toString().toUtf8())
                        .object()["session_id"]
                        .toString();
    QVERIFY(!id.isEmpty());
    bool acted = false;
    connect(service, &ShellService::outputSnapshot, this,
            [&](const QString &request, const QString &text) {
              if (acted)
                return;
              acted = true;
              QVERIFY(text.toUtf8().size() <= 16 * 1024);
              if (destroy)
                delete service.data();
              else
                service->cancel(request);
            });
    service->execute(
        "output", "executeShellCommand",
        {{"session_id", id},
         {"explanation", "Snapshot callback fixture"},
         {"command", "Write-Output 'LIVE_GRID'; Start-Sleep -Seconds 60"},
         {"wait_until", QJsonObject{{"timeout_ms", 5000}, {"idle_ms", 0}}}});
    QTRY_VERIFY_WITH_TIMEOUT(acted, 7000);
    if (service) {
      QCOMPARE(completed.size(), 2);
      QVERIFY(QJsonDocument::fromJson(completed[1][1].toString().toUtf8())
                  .object()["session_exited"]
                  .toBool());
      delete service.data();
    }
    QTest::qWait(50);
  }
  void watchersCancelTimeoutAndManifest() {
    Fixture f;
    QSignalSpy wakes(&f.service, &ShellService::watcherCompleted);
    f.service.execute("watch", "createWatcherSession",
                      {{"cwd", "ws1"},
                       {"title", "Watch <fixture>"},
                       {"description", "A & B"},
                       {"command", "Start-Sleep -Seconds 60"},
                       {"timeout_ms", 10000}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("watch").isEmpty(), 15000);
    const auto first = f.result("watch")["session_id"].toString();
    QVERIFY(f.service.manifest().contains(
        "watcher-title=\"Watch &lt;fixture&gt;\""));
    QVERIFY(f.service.manifest().contains("watcher-description=\"A &amp; B\""));
    QVERIFY(f.service.manifest().contains("watcher-expires-at="));
    f.service.execute("kill", "executeShellCommand",
                      {{"session_id", first},
                       {"explanation", "Cancel watcher"},
                       {"kill", true}});
    QVERIFY(!f.result("kill").isEmpty());
    QCOMPARE(wakes.size(), 0);
    f.service.execute("timeout", "createWatcherSession",
                      {{"cwd", "ws1"},
                       {"title", "Timeout fixture"},
                       {"command", "Start-Sleep -Seconds 60"},
                       {"timeout_ms", 10000}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("timeout").isEmpty(), 15000);
    QTRY_COMPARE_WITH_TIMEOUT(wakes.size(), 1, 12000);
    QCOMPARE(wakes[0][0].toJsonObject()["outcome"].toString(),
             QString("timed_out"));
    QCOMPARE(wakes[0][0].toJsonObject()["sessionId"].toString(),
             f.result("timeout")["session_id"].toString());
  }
  void fiveSessionLimitAndScopeChange() {
    Fixture f;
    for (int i = 0; i < 5; ++i) {
      const auto request = QString::number(i);
      f.service.execute(request, "createShellSession", {{"cwd", "ws1"}});
      QTRY_VERIFY_WITH_TIMEOUT(!f.result(request).isEmpty(), 15000);
    }
    f.service.execute("sixth", "createShellSession", {{"cwd", "ws1"}});
    QVERIFY(!f.failure("sixth").isEmpty());
    QCOMPARE(f.service.snapshot()["sessions"].toArray().size(), 5);
    QTemporaryDir next;
    QVERIFY(f.service.setMounts({{"ws1", next.path()}}));
    for (const auto &value : f.service.snapshot()["sessions"].toArray()) {
      QVERIFY(value.toObject()["exited"].toBool());
      QVERIFY(!value.toObject()["running"].toBool());
    }
    f.service.execute("next", "createShellSession", {{"cwd", "ws1"}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("next").isEmpty(), 15000);
    QVERIFY(f.service.setMounts({}));
  }
  void killingSessionStopsItsOwnedDescendant() {
    Fixture f;
    f.service.execute("create", "createShellSession", {{"cwd", "ws1"}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("create").isEmpty(), 15000);
    const auto id = f.result("create")["session_id"].toString();
    f.service.execute(
        "child", "executeShellCommand",
        {{"session_id", id},
         {"explanation", "Owned child fixture"},
         {"command", "$child = Start-Process -FilePath $env:ComSpec "
                     "-ArgumentList '/c ping -n 120 127.0.0.1 >nul' "
                     "-WindowStyle Hidden -PassThru; Write-Output ('CHILD_' + "
                     "'PID:' + $child.Id); Start-Sleep -Seconds 60"},
         {"wait_until", QJsonObject{{"timeout_ms", 5000},
                                    {"output_pattern", "CHILD_PID:[0-9]+"},
                                    {"idle_ms", 0}}}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("child").isEmpty(), 7000);
    const auto match = QRegularExpression("CHILD_PID:([0-9]+)")
                           .match(f.result("child")["output"].toString());
    QVERIFY2(match.hasMatch(),
             qPrintable(QJsonDocument(f.result("child")).toJson()));
    const auto childId = match.captured(1).toUInt();
    QVERIFY(childId != GetCurrentProcessId());
    HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, childId);
    QVERIFY(child);
    auto *process = f.service.findChild<ConPtyProcess *>();
    QVERIFY(process);
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, process->processId());
    QVERIFY(parent);
    f.service.execute("kill", "executeShellCommand",
                      {{"session_id", id},
                       {"explanation", "Finish owned test"},
                       {"kill", true}});
    QCOMPARE(WaitForSingleObject(child, 3000), DWORD(WAIT_OBJECT_0));
    QCOMPARE(WaitForSingleObject(parent, 3000), DWORD(WAIT_OBJECT_0));
    CloseHandle(child);
    CloseHandle(parent);
  }
  void terminalObserverCannotMatchOldBytesAgainstReplacement() {
    Fixture f;
    f.service.execute("create", "createShellSession", {{"cwd", "ws1"}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("create").isEmpty(), 15000);
    const auto id = f.result("create")["session_id"].toString();
    auto *model = f.service.findChild<TerminalModel *>();
    QVERIFY(model);
    bool acted = false;
    connect(model, &TerminalModel::changed, &f.service, [&] {
      if (acted || !model->plainText().contains("OLD_MARKER"))
        return;
      acted = true;
      f.service.execute(
          "replacement", "executeShellCommand",
          {{"session_id", id},
           {"explanation", "Replacement wait"},
           {"wait_until", QJsonObject{{"timeout_ms", 200},
                                      {"output_pattern", "OLD_MARKER"},
                                      {"idle_ms", 0}}}});
    });
    f.command("original", id,
              "Write-Output ('OLD_' + 'MARKER'); Start-Sleep -Seconds 60",
              5000);
    QTRY_VERIFY_WITH_TIMEOUT(acted, 7000);
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("replacement").isEmpty(), 3000);
    QCOMPARE(f.result("replacement")["resolved_by"].toString(),
             QString("timeout"));
    QCOMPARE(f.result("original")["resolved_by"].toString(), QString("abort"));
  }
  void exitedRetentionPreservesLogsActiveAndRecentViews() {
    Fixture f;
    f.service.execute("active", "createShellSession", {{"cwd", "ws1"}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("active").isEmpty(), 15000);
    const auto active = f.result("active")["session_id"].toString();
    QPointer<ConPtyProcess> activeProcess = f.service.sessionProcess(active);
    QString firstId, lastId;
    QPointer<TerminalModel> oldest, recent;
    std::unique_ptr<TerminalWidget> oldView, recentView;
    for (int i = 0; i < 33; ++i) {
      const auto create = "retained-" + QString::number(i);
      f.service.execute(create, "createShellSession", {{"cwd", "ws1"}});
      QTRY_VERIFY_WITH_TIMEOUT(!f.result(create).isEmpty(), 15000);
      lastId = f.result(create)["session_id"].toString();
      if (i == 0) {
        firstId = lastId;
        oldest = f.service.sessionModel(firstId);
        oldView = std::make_unique<TerminalWidget>(
            oldest, f.service.sessionProcess(firstId));
        f.command("old-log", firstId, "Write-Output 'RETAINED_FILE_LOG'");
        QTRY_VERIFY_WITH_TIMEOUT(!f.result("old-log").isEmpty(), 7000);
        QByteArray lines;
        for (int line = 0; line < 500; ++line)
          lines += "memory fixture\r\n";
        oldest->feed(lines);
        QVERIFY(oldest->historyLines() > 200);
      }
      if (i == 32) {
        recent = f.service.sessionModel(lastId);
        recentView = std::make_unique<TerminalWidget>(
            recent, f.service.sessionProcess(lastId));
      }
      f.service.execute("kill-" + QString::number(i), "executeShellCommand",
                        {{"session_id", lastId},
                         {"explanation", "Retire fixture"},
                         {"kill", true}});
      if (i == 0) {
        QVERIFY(oldest);
        QCOMPARE(oldest->historyLines(), 200);
        QCOMPARE(oldView->terminalModel(), oldest.data());
      }
    }
    QVERIFY(!oldest);
    QVERIFY(!oldView->terminalModel());
    QVERIFY(!f.service.ownsSession(firstId));
    QVERIFY(recent);
    QCOMPARE(recentView->terminalModel(), recent.data());
    QVERIFY(activeProcess && activeProcess->isRunning());
    QVERIFY(f.service.ownsSession(active));
    QCOMPARE(f.service.snapshot()["sessions"].toArray().size(), 33);
    QCOMPARE(f.logs->list().size(), 34);
    QVERIFY(f.logs->rawRead("shells/" + firstId + ".shell.log", 1024 * 1024)
                .contains("RETAINED_FILE_LOG"));
    f.service.execute("after-retention", "createShellSession",
                      {{"cwd", "ws1"}});
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("after-retention").isEmpty(), 15000);
  }
  void explicitEnvironmentAndUnicode() {
    const auto old = qgetenv("ADE_FIXTURE_SECRET");
    qputenv("ADE_FIXTURE_SECRET", "fake-local-secret");
    Fixture f;
    f.service.execute("create", "createShellSession", {{"cwd", "ws1"}});
    if (old.isNull())
      qunsetenv("ADE_FIXTURE_SECRET");
    else
      qputenv("ADE_FIXTURE_SECRET", old);
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("create").isEmpty(), 15000);
    const auto id = f.result("create")["session_id"].toString();
    const auto marker = QString::fromUtf8("UNICODE_λ🙂_END");
    f.command("unicode", id,
              "Write-Output '" + marker +
                  "'; Write-Output ('SECRET_PRESENT:' + (Test-Path "
                  "Env:ADE_FIXTURE_SECRET))");
    QTRY_VERIFY_WITH_TIMEOUT(!f.result("unicode").isEmpty(), 7000);
    const auto output = f.result("unicode")["output"].toString();
    QVERIFY2(output.contains(marker), qPrintable(output));
    QVERIFY(output.contains("SECRET_PRESENT:False"));
    QVERIFY(!output.contains(QChar::ReplacementCharacter));
    QVERIFY(f.logs->rawRead("shells/" + id + ".shell.log", 1024 * 1024)
                .contains(marker.toUtf8()));
  }
};
QTEST_MAIN(ShellServiceTest)
#include "test_shell_service.moc"
