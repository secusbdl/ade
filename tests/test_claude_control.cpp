#include "ClaudeControlProcess.h"
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class ClaudeControlTest : public QObject {
  Q_OBJECT
  QString executable() const {
    return QDir(QCoreApplication::applicationDirPath())
        .filePath("claude_fixture.exe");
  }
  bool start(ClaudeControlProcess &p, const QString &directory) {
    return p.start(executable(), {}, directory,
                   ClaudeControlProcess::managedEnvironment());
  }
private slots:
  void cleanup() {
    qunsetenv("ADE_CLAUDE_FIXTURE");
    qunsetenv("ADE_CLAUDE_LOG");
  }
  void nativeControlShapeAndMultipleTurns() {
    QTemporaryDir dir;
    const auto log = dir.filePath("wire");
    qputenv("ADE_CLAUDE_LOG", log.toUtf8());
    ClaudeControlProcess p;
    QSignalSpy ready(&p, &ClaudeControlProcess::started),
        response(&p, &ClaudeControlProcess::controlResponse),
        events(&p, &ClaudeControlProcess::frame);
    QVERIFY(start(p, dir.path()));
    QTRY_COMPARE(ready.size(), 1);
    const auto id = p.control("initialize", {{"hooks", QJsonValue::Null}});
    QVERIFY(!id.isEmpty());
    QTRY_COMPARE(response.size(), 1);
    QCOMPARE(response[0][0].toString(), id);
    QVERIFY(response[0][2].toString().isEmpty());
    for (int turn = 0; turn < 2; ++turn) {
      QVERIFY(p.sendUser({QJsonObject{{"type", "text"}, {"text", "Hello"}}},
                         "session-fixture"));
      QTRY_COMPARE(events.size(), (turn + 1) * 5);
      QCOMPARE(events[turn * 5][0].toJsonObject().value("event").toObject().value("type").toString(), QString("message_start"));
      QCOMPARE(events[turn * 5 + 3][0].toJsonObject().value("type").toString(), QString("assistant"));
      QCOMPARE(events[turn * 5 + 4][0].toJsonObject().value("type").toString(), QString("result"));
      QVERIFY(p.isRunning());
    }
    QCOMPARE(events[2][0]
                 .toJsonObject()["event"]
                 .toObject()["delta"]
                 .toObject()["text"]
                 .toString(),
             QString::fromUtf8("TEXT_\xf0\x9f\x98\x80"));
    QFile file(log);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto lines = file.readAll().split('\n');
    const auto init = QJsonDocument::fromJson(lines.first()).object();
    QCOMPARE(init["type"].toString(), QString("control_request"));
    QVERIFY(!init.contains("jsonrpc"));
    QCOMPARE(init["request_id"].toString(), id);
    const auto user = QJsonDocument::fromJson(lines[1]).object();
    QCOMPARE(user["message"].toObject()["role"].toString(), QString("user"));
    QVERIFY(user["parent_tool_use_id"].isNull());
  }
  void permissionResponsesAreOneShot() {
    qputenv("ADE_CLAUDE_FIXTURE", "permission");
    QTemporaryDir dir;
    ClaudeControlProcess p;
    QSignalSpy ready(&p, &ClaudeControlProcess::started),
        asks(&p, &ClaudeControlProcess::controlRequest),
        events(&p, &ClaudeControlProcess::frame);
    QVERIFY(start(p, dir.path()));
    QTRY_COMPARE(ready.size(), 1);
    p.control("initialize");
    QTRY_COMPARE(asks.size(), 1);
    QCOMPARE(asks[0][1].toString(), QString("can_use_tool"));
    QVERIFY(!p.respond("wrong", {}));
    QVERIFY(p.respond(
        "permission-1",
        {{"behavior", "allow"},
         {"updatedInput", QJsonObject{{"file_path", "fixture.txt"}}}}));
    QVERIFY(!p.reject("permission-1", "No"));
    QTRY_COMPARE(events.size(), 1);
    QCOMPARE(events[0][0]
                 .toJsonObject()["decision"]
                 .toObject()["response"]
                 .toObject()["behavior"]
                 .toString(),
             QString("allow"));
  }
  void cancellationRevokesInboundResponse() {
    qputenv("ADE_CLAUDE_FIXTURE", "cancel-permission");
    QTemporaryDir dir;
    ClaudeControlProcess p;
    QSignalSpy ready(&p, &ClaudeControlProcess::started),
        cancel(&p, &ClaudeControlProcess::controlCanceled);
    QVERIFY(start(p, dir.path()));
    QTRY_COMPARE(ready.size(), 1);
    p.control("initialize");
    QTRY_COMPARE(cancel.size(), 1);
    QCOMPARE(cancel[0][0].toString(), QString("permission-1"));
    QVERIFY(!p.respond("permission-1", {{"behavior", "allow"}}));
  }
  void invalidProtocolFailsClosed_data() {
    QTest::addColumn<QString>("mode");
    for (const auto *v :
         {"invalid-json", "invalid-utf8", "oversize", "duplicate-permission"})
      QTest::newRow(v) << QString(v);
  }
  void invalidProtocolFailsClosed() {
    QFETCH(QString, mode);
    qputenv("ADE_CLAUDE_FIXTURE", mode.toUtf8());
    QTemporaryDir dir;
    ClaudeControlProcess p;
    QSignalSpy ready(&p, &ClaudeControlProcess::started),
        failed(&p, &ClaudeControlProcess::failed);
    QVERIFY(start(p, dir.path()));
    QTRY_COMPARE(ready.size(), 1);
    p.control("initialize");
    QTRY_COMPARE(failed.size(), 1);
    QVERIFY(!p.isRunning());
  }
  void timeoutAndErrorAreRedacted_data() {
    QTest::addColumn<QString>("mode");
    QTest::newRow("timeout") << QString("hold");
    QTest::newRow("error") << QString("error");
  }
  void timeoutAndErrorAreRedacted() {
    QFETCH(QString, mode);
    qputenv("ADE_CLAUDE_FIXTURE", mode.toUtf8());
    QTemporaryDir dir;
    ClaudeControlProcess p;
    QSignalSpy ready(&p, &ClaudeControlProcess::started),
        responses(&p, &ClaudeControlProcess::controlResponse);
    QVERIFY(start(p, dir.path()));
    QTRY_COMPARE(ready.size(), 1);
    const auto id = p.control("initialize", {}, 100);
    QTRY_COMPARE(responses.size(), 1);
    QCOMPARE(responses[0][0].toString(), id);
    QVERIFY(!responses[0][2].toString().isEmpty());
    QVERIFY(!responses[0][2].toString().contains("SYNTHETIC_PRIVATE_DETAIL"));
  }
  void nullableSuccessMatchesOfficialControlSchema() {
    qputenv("ADE_CLAUDE_FIXTURE", "null-success");
    QTemporaryDir dir;
    ClaudeControlProcess p;
    QSignalSpy ready(&p, &ClaudeControlProcess::started),
        responses(&p, &ClaudeControlProcess::controlResponse);
    QVERIFY(start(p, dir.path()));
    QTRY_COMPARE(ready.size(), 1);
    const auto id = p.control("interrupt");
    QTRY_COMPARE(responses.size(), 1);
    QCOMPARE(responses[0][0].toString(), id);
    QVERIFY(responses[0][1].toJsonObject().isEmpty());
    QVERIFY(responses[0][2].toString().isEmpty());
  }
  void subscriptionEnvironmentDoesNotImpersonateParentOrForwardKeys() {
    qputenv("CLAUDECODE", "1");
    qputenv("ADE_FAKE_API_KEY", "fixture-only");
    qputenv("ADE_FAKE_TOKEN", "fixture-only");
    const auto environment = ClaudeControlProcess::managedEnvironment();
    qunsetenv("CLAUDECODE");
    qunsetenv("ADE_FAKE_API_KEY");
    qunsetenv("ADE_FAKE_TOKEN");
    QVERIFY(!environment.contains("CLAUDECODE"));
    QVERIFY(!environment.contains("ADE_FAKE_API_KEY"));
    QVERIFY(!environment.contains("ADE_FAKE_TOKEN"));
  }
  void unknownResponseCannotCompleteOwnedRequest() {
    qputenv("ADE_CLAUDE_FIXTURE", "unknown-id");
    QTemporaryDir dir;
    ClaudeControlProcess p;
    QSignalSpy ready(&p, &ClaudeControlProcess::started),
        responses(&p, &ClaudeControlProcess::controlResponse);
    QVERIFY(start(p, dir.path()));
    QTRY_COMPARE(ready.size(), 1);
    const auto id = p.control("initialize");
    QTRY_COMPARE(responses.size(), 1);
    QCOMPARE(responses[0][0].toString(), id);
  }
  void deletionFromFrameAndPermissionIsSafe_data() {
    QTest::addColumn<bool>("permission");
    QTest::newRow("stream") << false;
    QTest::newRow("permission") << true;
  }
  void deletionFromFrameAndPermissionIsSafe() {
    QFETCH(bool, permission);
    if (permission)
      qputenv("ADE_CLAUDE_FIXTURE", "permission");
    QTemporaryDir dir;
    QPointer<ClaudeControlProcess> p = new ClaudeControlProcess;
    QSignalSpy ready(p, &ClaudeControlProcess::started);
    if (permission)
      connect(p, &ClaudeControlProcess::controlRequest, p,
              [&] { delete p.data(); });
    else
      connect(p, &ClaudeControlProcess::frame, p, [&] { delete p.data(); });
    QVERIFY(start(*p, dir.path()));
    QTRY_COMPARE(ready.size(), 1);
    if (permission)
      p->control("initialize");
    else
      QVERIFY(p->sendUser({QJsonObject{{"type", "text"}, {"text", "delete"}}}));
    QTRY_VERIFY(!p);
  }
  void stopRetiresPendingAndReplacementUsesFreshIdentity() {
    qputenv("ADE_CLAUDE_FIXTURE", "hold");
    QTemporaryDir dir;
    ClaudeControlProcess p;
    QSignalSpy ready(&p, &ClaudeControlProcess::started),
        responses(&p, &ClaudeControlProcess::controlResponse);
    QVERIFY(start(p, dir.path()));
    QTRY_COMPARE(ready.size(), 1);
    const auto old = p.control("initialize", {}, 100);
    p.stop();
    QVERIFY(!p.isRunning());
    qunsetenv("ADE_CLAUDE_FIXTURE");
    QVERIFY(start(p, dir.path()));
    QTRY_COMPARE(ready.size(), 2);
    const auto fresh = p.control("initialize");
    QVERIFY(fresh != old);
    QTRY_COMPARE(responses.size(), 1);
    QCOMPARE(responses[0][0].toString(), fresh);
  }
  void invalidExecutableFailsWithoutLeakingInput() {
    ClaudeControlProcess p;
    QSignalSpy failed(&p, &ClaudeControlProcess::failed);
    QVERIFY(!p.start("Z:/SYNTHETIC_PRIVATE_DETAIL.exe", {}, {}, {}));
    QCOMPARE(failed.size(), 1);
    QVERIFY(!failed[0][0].toString().contains("SYNTHETIC_PRIVATE_DETAIL"));
  }
};
QTEST_GUILESS_MAIN(ClaudeControlTest)
#include "test_claude_control.moc"
