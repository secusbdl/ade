#include "harness/CursorExtension.h"
#include "harness/CursorHarnessSession.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class CursorTest : public QObject {
  Q_OBJECT
  CursorHarnessOptions options(const QString &path) {
    CursorHarnessOptions o;
    o.executable = QDir(QCoreApplication::applicationDirPath())
                       .filePath("cursor_fixture.exe");
    o.providerId = "cursor-instance";
    o.ownerId = "chat-one";
    o.workspace = path;
    o.model = "model-b";
    o.historyContext = "PRIOR_HISTORY";
    return o;
  }
  QJsonArray input() {
    return {QJsonObject{{"type", "text"}, {"text", "CURRENT_PROMPT"}}};
  }
  QJsonArray log(const QString &path, const QString &method) {
    QFile f(path);
    QJsonArray r;
    if (f.open(QIODevice::ReadOnly))
      for (const auto &line : f.readAll().split('\n')) {
        if (line.trimmed().isEmpty())
          continue;
        const auto o = QJsonDocument::fromJson(line).object();
        if (o["method"].toString() == method)
          r.append(o);
      }
    return r;
  }
  void autoAuth(CursorHarnessSession &s) {
    connect(&s, &CursorHarnessSession::authenticationRequested, &s,
            [&s](const QString &t, const QJsonArray &) { s.authenticate(t); });
  }
private slots:
  void cleanup() {
    qunsetenv("ADE_CURSOR_SCENARIO");
    qunsetenv("ADE_CURSOR_LOG");
  }
  void explicitAuthenticationBeforeAnySession() {
    QTemporaryDir d;
    const auto path = d.filePath("calls.jsonl");
    qputenv("ADE_CURSOR_LOG", path.toUtf8());
    CursorHarnessSession s;
    QSignalSpy auth(&s, &CursorHarnessSession::authenticationRequested),
        done(&s, &CursorHarnessSession::finished);
    QVERIFY(s.startTurn(options(d.path()), input()));
    QTRY_COMPARE(auth.size(), 1);
    QTest::qWait(50);
    QCOMPARE(log(path, "authenticate").size(), 0);
    QCOMPARE(log(path, "session/new").size(), 0);
    QVERIFY(!s.authenticate("stale"));
    QVERIFY(s.rejectAuthentication(auth[0][0].toString()));
    QCOMPARE(done.size(), 1);
    QCOMPARE(done[0][0].toJsonObject()["status"].toString(),
             QString("interrupted"));
  }
  void actualProcessStreamingConfigurationAndResume() {
    QTemporaryDir d;
    const auto path = d.filePath("calls.jsonl");
    qputenv("ADE_CURSOR_LOG", path.toUtf8());
    CursorHarnessSession s;
    autoAuth(s);
    QSignalSpy done(&s, &CursorHarnessSession::finished),
        events(&s, &CursorHarnessSession::event);
    auto o = options(d.path());
    o.mode = "plan";
    QVERIFY(s.startTurn(o, input()));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][0].toJsonObject()["status"].toString(),
             QString("completed"));
    QCOMPARE(log(path, "session/set_config_option").size(), 2);
    const auto text = QJsonDocument(done[0][0].toJsonObject()).toJson();
    QVERIFY(text.contains("Hello"));
    QVERIFY(!text.contains("FOREIGN"));
    const auto saved = s.sessionState();
    s.shutdown();
    QVERIFY(s.startTurn(o, input(), saved));
    QTRY_COMPARE(done.size(), 2);
    QCOMPARE(log(path, "session/load").size(), 1);
    QCOMPARE(log(path, "authenticate").size(), 2);
    const auto prompts = log(path, "session/prompt");
    QCOMPARE(prompts.size(), 2);
    QVERIFY(QJsonDocument(prompts[0].toObject())
                .toJson()
                .contains("PRIOR_HISTORY"));
    QVERIFY(!QJsonDocument(prompts[1].toObject())
                 .toJson()
                 .contains("PRIOR_HISTORY"));
    QVERIFY(!QJsonDocument(done[1][0].toJsonObject())
                 .toJson()
                 .contains("REPLAY_MUST_NOT_DUPLICATE"));
  }
  void sameProcessRequiresSavedContinuity_data() {
    QTest::addColumn<bool>("emptyState");
    QTest::newRow("edited-history") << true;
    QTest::newRow("foreign-owner") << false;
  }
  void sameProcessRequiresSavedContinuity() {
    QFETCH(bool, emptyState);
    QTemporaryDir d;
    const auto path = d.filePath("calls.jsonl");
    qputenv("ADE_CURSOR_LOG", path.toUtf8());
    CursorHarnessSession s;
    autoAuth(s);
    QSignalSpy done(&s, &CursorHarnessSession::finished);
    QVERIFY(s.startTurn(options(d.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    auto state = emptyState ? QJsonObject{} : s.sessionState();
    if (!emptyState)
      state["ownerId"] = "foreign";
    QVERIFY(s.startTurn(options(d.path()), input(), state));
    QTRY_COMPARE(done.size(), 2);
    QCOMPARE(log(path, "session/new").size(), 2);
    QCOMPARE(log(path, "session/load").size(), 0);
  }
  void continuityRejectsForeignState_data() {
    QTest::addColumn<QString>("key");
    QTest::addColumn<QJsonValue>("value");
    QTest::newRow("owner") << QString("ownerId") << QJsonValue("other");
    QTest::newRow("provider") << QString("providerId") << QJsonValue("other");
    QTest::newRow("root") << QString("workspaceIdentity")
                          << QJsonValue("other");
    QTest::newRow("executable")
        << QString("executableIdentity") << QJsonValue("other");
    QTest::newRow("fork") << QString("fork") << QJsonValue(true);
    QTest::newRow("kind") << QString("kind") << QJsonValue("opencode");
  }
  void continuityRejectsForeignState() {
    QFETCH(QString, key);
    QFETCH(QJsonValue, value);
    QTemporaryDir d;
    const auto path = d.filePath("calls.jsonl");
    qputenv("ADE_CURSOR_LOG", path.toUtf8());
    CursorHarnessSession s;
    autoAuth(s);
    QSignalSpy done(&s, &CursorHarnessSession::finished);
    QVERIFY(s.startTurn(options(d.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    auto saved = s.sessionState();
    saved[key] = value;
    s.shutdown();
    QVERIFY(s.startTurn(options(d.path()), input(), saved));
    QTRY_COMPARE(done.size(), 2);
    QCOMPARE(log(path, "session/load").size(), 0);
    QCOMPARE(log(path, "session/new").size(), 2);
  }
  void loadFailurePreservesHistoryAndDoesNotSilentlyFork() {
    QTemporaryDir d;
    const auto path = d.filePath("calls.jsonl");
    qputenv("ADE_CURSOR_LOG", path.toUtf8());
    CursorHarnessSession s;
    autoAuth(s);
    QSignalSpy done(&s, &CursorHarnessSession::finished);
    QVERIFY(s.startTurn(options(d.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    const auto saved = s.sessionState();
    s.shutdown();
    qputenv("ADE_CURSOR_SCENARIO", "load-error");
    QVERIFY(s.startTurn(options(d.path()), input(), saved));
    QTRY_COMPARE(done.size(), 2);
    QCOMPARE(done[1][0].toJsonObject()["status"].toString(), QString("failed"));
    QCOMPARE(log(path, "session/new").size(), 1);
    QCOMPARE(log(path, "session/prompt").size(), 1);
  }
  void permissionPausesAndEarlyResponseDoesNotFinish() {
    QTemporaryDir d;
    qputenv("ADE_CURSOR_SCENARIO", "permission-early-return");
    CursorHarnessSession s;
    autoAuth(s);
    QSignalSpy approval(&s, &CursorHarnessSession::approvalRequired),
        done(&s, &CursorHarnessSession::finished);
    auto o = options(d.path());
    o.promptTimeoutMs = 200;
    QVERIFY(s.startTurn(o, input()));
    QTRY_COMPARE(approval.size(), 1);
    QTest::qWait(350);
    QCOMPARE(done.size(), 0);
    QVERIFY(!s.answerApproval("other", true));
    QVERIFY(s.answerApproval(approval[0][0].toString(), false));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][0].toJsonObject()["status"].toString(),
             QString("completed"));
  }
  void questionAndPlanReplies_data() {
    QTest::addColumn<QString>("scenario");
    QTest::newRow("single") << QString("question");
    QTest::newRow("multi") << QString("question-multiple");
    QTest::newRow("cancel") << QString("question-cancel");
    QTest::newRow("plan") << QString("plan");
  }
  void questionAndPlanReplies() {
    QFETCH(QString, scenario);
    QTemporaryDir d;
    qputenv("ADE_CURSOR_SCENARIO", scenario.toUtf8());
    const auto path = d.filePath("calls.jsonl");
    qputenv("ADE_CURSOR_LOG", path.toUtf8());
    CursorHarnessSession s;
    autoAuth(s);
    QSignalSpy questions(&s, &CursorHarnessSession::questionRequired),
        plans(&s, &CursorHarnessSession::planRequired),
        done(&s, &CursorHarnessSession::finished);
    auto o = options(d.path());
    o.alwaysAllow = true;
    QVERIFY(s.startTurn(o, input()));
    if (scenario == "plan") {
      QTRY_COMPARE(plans.size(), 1);
      QCOMPARE(done.size(), 0);
      QVERIFY(s.answerPlan(plans[0][0].toString(), false));
    } else {
      QTRY_COMPARE(questions.size(), 1);
      const auto token = questions[0][0].toString();
      QVERIFY(!s.answerQuestion(token, {{"mode", "foreign"}}));
      if (scenario == "question-cancel")
        QVERIFY(s.cancelQuestion(token));
      else
        QVERIFY(s.answerQuestion(
            token, {{"mode", scenario == "question-multiple"
                                 ? QJsonValue(QJsonArray{"a", "b"})
                                 : QJsonValue("a")}}));
    }
    QTRY_COMPARE(done.size(), 1);
    const auto rows = log(path, "");
    QVERIFY(!rows.isEmpty());
    const auto outcome =
        rows.last().toObject()["result"].toObject()["outcome"].toObject();
    QCOMPARE(outcome["outcome"].toString(),
             scenario == "plan"              ? QString("rejected")
             : scenario == "question-cancel" ? QString("cancelled")
                                             : QString("answered"));
  }
  void malformedQuestionRejectedWithoutForm() {
    QTemporaryDir d;
    qputenv("ADE_CURSOR_SCENARIO", "question-invalid");
    CursorHarnessSession s;
    autoAuth(s);
    QSignalSpy questions(&s, &CursorHarnessSession::questionRequired),
        done(&s, &CursorHarnessSession::finished);
    QVERIFY(s.startTurn(options(d.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(questions.size(), 0);
  }
  void authFailureDoesNotEchoPrivateDiagnostic() {
    QTemporaryDir d;
    qputenv("ADE_CURSOR_SCENARIO", "auth-error");
    CursorHarnessSession s;
    autoAuth(s);
    QSignalSpy done(&s, &CursorHarnessSession::finished);
    QVERIFY(s.startTurn(options(d.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(!QJsonDocument(done[0][0].toJsonObject())
                 .toJson()
                 .contains("SYNTHETIC_PRIVATE"));
  }
  void cancellationAndTokenCannotCrossReplacement() {
    QTemporaryDir d;
    qputenv("ADE_CURSOR_SCENARIO", "question");
    CursorHarnessSession s;
    autoAuth(s);
    QSignalSpy q(&s, &CursorHarnessSession::questionRequired),
        done(&s, &CursorHarnessSession::finished);
    QVERIFY(s.startTurn(options(d.path()), input()));
    QTRY_COMPARE(q.size(), 1);
    const auto old = q[0][0].toString();
    s.interrupt();
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(s.startTurn(options(d.path()), input()));
    QTRY_COMPARE(q.size(), 2);
    QVERIFY(!s.answerQuestion(old, {{"mode", "a"}}));
    QVERIFY(s.answerQuestion(q[1][0].toString(), {{"mode", "b"}}));
    QTRY_COMPARE(done.size(), 2);
  }
  void authObserverMayDeleteOwner() {
    QTemporaryDir d;
    QPointer<CursorHarnessSession> s = new CursorHarnessSession;
    connect(s, &CursorHarnessSession::authenticationRequested, this,
            [&](const QString &, const QJsonArray &) { delete s.data(); });
    QVERIFY(s->startTurn(options(d.path()), input()));
    QTRY_VERIFY(s.isNull());
  }
  void eventObserverMayReplaceTurn() {
    QTemporaryDir d;
    CursorHarnessSession s;
    autoAuth(s);
    QSignalSpy done(&s, &CursorHarnessSession::finished);
    bool replaced = false;
    connect(&s, &CursorHarnessSession::event, this, [&](const QJsonObject &) {
      if (replaced)
        return;
      replaced = true;
      s.shutdown();
      auto o = options(d.path());
      o.ownerId = "replacement";
      s.startTurn(o, input());
    });
    QVERIFY(s.startTurn(options(d.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(s.sessionState()["ownerId"].toString(), QString("replacement"));
  }
  void activeTimeoutAndMissingModel_data() {
    QTest::addColumn<QString>("scenario");
    QTest::newRow("timeout") << QString("timeout");
    QTest::newRow("model") << QString("missing-model");
    QTest::newRow("version") << QString("bad-version");
  }
  void activeTimeoutAndMissingModel() {
    QFETCH(QString, scenario);
    QTemporaryDir d;
    qputenv("ADE_CURSOR_SCENARIO", scenario.toUtf8());
    CursorHarnessSession s;
    autoAuth(s);
    QSignalSpy done(&s, &CursorHarnessSession::finished);
    auto o = options(d.path());
    o.promptTimeoutMs = 150;
    QVERIFY(s.startTurn(o, input()));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][0].toJsonObject()["status"].toString(), QString("failed"));
  }
  void vendorBundleResolutionNeverRunsLauncher() {
    QTemporaryDir d;
    QDir().mkpath(d.filePath("versions/2026.11.05-aabb"));
    const auto root = d.filePath("versions/2026.11.05-aabb/");
    for (const auto &name : {"node.exe", "index.js"}) {
      QFile f(root + name);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("fixture");
    }
    QFile script(d.filePath("agent.ps1"));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("throw 'MUST NOT EXECUTE'");
    script.close();
    QString error;
    const auto exe = CursorExecutable::resolve(script.fileName(), &error);
    QVERIFY2(exe.valid(), qPrintable(error));
    QCOMPARE(exe.arguments.size(), 2);
    QCOMPARE(exe.arguments[1], QString("acp"));
    QVERIFY(exe.program.endsWith("node.exe"));
    QVERIFY(exe.arguments[0].endsWith("index.js"));
    QVERIFY(!CursorExecutable::resolve(exe.program).valid());
  }
};
QTEST_GUILESS_MAIN(CursorTest)
#include "test_cursor.moc"
