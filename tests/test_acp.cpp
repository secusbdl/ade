#include "AcpForm.h"
#include "AcpHarnessSession.h"
#include "OpenCodeModelDiscovery.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class AcpTest : public QObject {
  Q_OBJECT
  AcpHarnessOptions options(const QString &path) {
    AcpHarnessOptions o;
    o.executable = QDir(QCoreApplication::applicationDirPath())
                       .filePath("acp_fixture.exe");
    o.providerId = "fixture-provider";
    o.workspace = path;
    o.model = "model-b";
    o.historyContext = "PRIOR_HISTORY";
    return o;
  }
  QJsonArray input() {
    return {QJsonObject{{"type", "text"}, {"text", "CURRENT_PROMPT"}}};
  }
  QJsonArray log(const QString &path, const QString &method) {
    QFile file(path);
    QJsonArray rows;
    if (!file.open(QIODevice::ReadOnly))
      return rows;
    for (const auto &line : file.readAll().split('\n')) {
      const auto row = QJsonDocument::fromJson(line).object();
      if (row["method"] == method)
        rows.append(row["params"]);
    }
    return rows;
  }
private slots:
  void cleanup() {
    qunsetenv("ADE_ACP_SCENARIO");
    qunsetenv("ADE_ACP_LOG");
    qunsetenv("OPENCODE_CONFIG_CONTENT");
  }
  void environmentPreservesConfigWithoutForwardingProviderKeys() {
    qputenv(
        "OPENCODE_CONFIG_CONTENT",
        R"({"theme":"fixture","permission":"deny","experimental":{"x":true}})");
    qputenv("ACP_FAKE_API_KEY", "synthetic");
    const auto env = AcpHarnessSession::environment(false);
    qunsetenv("ACP_FAKE_API_KEY");
    QVERIFY(!env.contains("ACP_FAKE_API_KEY"));
    const auto config =
        QJsonDocument::fromJson(env.value("OPENCODE_CONFIG_CONTENT").toUtf8())
            .object();
    QCOMPARE(config["theme"].toString(), QString("fixture"));
    QCOMPARE(config["permission"].toObject()["*"].toString(), QString("deny"));
    QCOMPARE(config["permission"].toObject()["bash"].toString(),
             QString("ask"));
    QVERIFY(config["experimental"].toObject()["x"].toBool());
    QCOMPARE(config["experimental"].toObject()["mcp_timeout"].toDouble(),
             43200000.);
  }
  void streamConfigResumeAndNoDuplicateHistory() {
    QTemporaryDir dir;
    const auto path = dir.filePath("calls.jsonl");
    qputenv("ADE_ACP_LOG", path.toUtf8());
    AcpHarnessSession session;
    QSignalSpy events(&session, &AcpHarnessSession::event),
        done(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][0].toJsonObject()["status"].toString(),
             QString("completed"));
    QCOMPARE(log(path, "session/set_config_option").size(), 1);
    QCOMPARE(events.first()[0].toJsonObject()["type"].toString(),
             QString("reasoning"));
    QVERIFY(
        !QJsonDocument(done[0][0].toJsonObject()).toJson().contains("FOREIGN"));
    const auto state = session.sessionState();
    QVERIFY(!state.isEmpty());
    session.shutdown();
    QVERIFY(session.startTurn(options(dir.path()), input(), state));
    QTRY_COMPARE(done.size(), 2);
    QCOMPARE(log(path, "session/resume").size(), 1);
    const auto prompts = log(path, "session/prompt");
    QCOMPARE(prompts.size(), 2);
    QCOMPARE(
        QJsonDocument(prompts[0].toObject()).toJson().count("PRIOR_HISTORY"),
        1);
    QVERIFY(!QJsonDocument(prompts[1].toObject())
                 .toJson()
                 .contains("PRIOR_HISTORY"));
  }
  void failedResumeCreatesFreshOnce() {
    QTemporaryDir dir;
    AcpHarnessSession session;
    QSignalSpy done(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    const auto state = session.sessionState();
    session.shutdown();
    qputenv("ADE_ACP_SCENARIO", "resume-error");
    const auto path = dir.filePath("calls");
    qputenv("ADE_ACP_LOG", path.toUtf8());
    QVERIFY(session.startTurn(options(dir.path()), input(), state));
    QTRY_COMPARE(done.size(), 2);
    QCOMPARE(log(path, "session/resume").size(), 1);
    QCOMPARE(log(path, "session/new").size(), 1);
    QCOMPARE(log(path, "session/prompt").size(), 1);
    QCOMPARE(done.last()[0].toJsonObject()["status"].toString(),
             QString("completed"));
  }
  void failuresAreBoundedAndRedacted_data() {
    QTest::addColumn<QString>("scenario");
    QTest::newRow("version") << "bad-version";
    QTest::newRow("auth") << "auth-required";
    QTest::newRow("session") << "session-error";
  }
  void failuresAreBoundedAndRedacted() {
    QFETCH(QString, scenario);
    qputenv("ADE_ACP_SCENARIO", scenario.toUtf8());
    QTemporaryDir dir;
    AcpHarnessSession session;
    QSignalSpy done(&session, &AcpHarnessSession::finished),
        auth(&session, &AcpHarnessSession::authenticationRequired);
    QVERIFY(session.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][0].toJsonObject()["status"].toString(), QString("failed"));
    QVERIFY(!QJsonDocument(done[0][0].toJsonObject())
                 .toJson()
                 .contains("SYNTHETIC_SECRET_ERROR"));
    QCOMPARE(auth.size(), scenario == "auth-required" ? 1 : 0);
  }
  void permissionsAreCorrelated_data() {
    QTest::addColumn<bool>("allow");
    QTest::newRow("allow") << true;
    QTest::newRow("deny") << false;
  }
  void permissionsAreCorrelated() {
    QFETCH(bool, allow);
    qputenv("ADE_ACP_SCENARIO", "approval");
    QTemporaryDir dir;
    AcpHarnessSession session;
    QSignalSpy ask(&session, &AcpHarnessSession::approvalRequired),
        done(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(ask.size(), 1);
    QVERIFY(!session.answerApproval("wrong", true));
    const auto token = ask[0][0].toString();
    QVERIFY(session.answerApproval(token, allow));
    QVERIFY(!session.answerApproval(token, !allow));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(!session.isBusy());
  }
  void duplicatePendingToolIdentityIsRejectedButSequentialReuseWorks() {
    qputenv("ADE_ACP_SCENARIO", "duplicate-approval");
    QTemporaryDir dir;
    const auto path = dir.filePath("calls");
    qputenv("ADE_ACP_LOG", path.toUtf8());
    AcpHarnessSession session;
    QSignalSpy ask(&session, &AcpHarnessSession::approvalRequired),
        events(&session, &AcpHarnessSession::event),
        done(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(options(dir.path()), input()));
    QTRY_VERIFY([&] {
      for (const auto &event : events)
        if (QJsonDocument(event[0].toJsonObject())
                .toJson()
                .contains("DUPLICATE_SENT_SENTINEL"))
          return true;
      return false;
    }());
    QCOMPARE(ask.size(), 1);
    QVERIFY(session.answerApproval(ask[0][0].toString(), true));
    QTRY_COMPARE(ask.size(), 2);
    QVERIFY(ask[0][0] != ask[1][0]);
    QVERIFY(session.answerApproval(ask[1][0].toString(), true));
    QTRY_COMPARE(done.size(), 1);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    bool duplicateRejected = false, firstAnswered = false,
         repeatAnswered = false;
    for (const auto &line : file.readAll().split('\n')) {
      const auto row = QJsonDocument::fromJson(line).object();
      if (row["id"] == "approval-2")
        duplicateRejected = row["error"].toObject()["code"] == -32600;
      if (row["id"] == "approval-1")
        firstAnswered =
            row["result"].toObject()["outcome"].toObject()["optionId"] == "yes";
      if (row["id"] == "approval-3")
        repeatAnswered =
            row["result"].toObject()["outcome"].toObject()["optionId"] == "yes";
    }
    QVERIFY(duplicateRejected);
    QVERIFY(firstAnswered);
    QVERIFY(repeatAnswered);
  }
  void questionAnswersAndCancelAreCorrelated_data() {
    QTest::addColumn<bool>("cancel");
    QTest::addColumn<QString>("scenario");
    QTest::newRow("answer") << false << "question";
    QTest::newRow("cancel") << true << "question";
    QTest::newRow("sessionless") << false << "sessionless-question";
  }
  void questionAnswersAndCancelAreCorrelated() {
    QFETCH(bool, cancel);
    QFETCH(QString, scenario);
    qputenv("ADE_ACP_SCENARIO", scenario.toUtf8());
    QTemporaryDir dir;
    AcpHarnessSession session;
    QSignalSpy ask(&session, &AcpHarnessSession::questionRequired),
        done(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(ask.size(), 1);
    const auto token = ask[0][0].toString();
    QVERIFY(!session.answerQuestion(token, {{"wrong", "value"}}));
    QVERIFY(!session.answerQuestion(token, {{"name", ""}}));
    QVERIFY(!session.answerQuestion(token, {{"name", 12}}));
    QVERIFY(cancel ? session.cancelQuestion(token)
                   : session.answerQuestion(token, {{"name", "valid"}}));
    QVERIFY(!session.cancelQuestion(token));
    QTRY_COMPARE(done.size(), 1);
  }
  void cancelRetainsPartialAndStopsOwnedProcess() {
    qputenv("ADE_ACP_SCENARIO", "hold");
    QTemporaryDir dir;
    AcpHarnessSession session;
    QSignalSpy events(&session, &AcpHarnessSession::event),
        done(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(options(dir.path()), input()));
    QTRY_VERIFY(!events.isEmpty());
    session.interrupt();
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][0].toJsonObject()["status"].toString(),
             QString("interrupted"));
    QVERIFY(!done[0][0].toJsonObject()["events"].toArray().isEmpty());
  }
  void deletionFromOutputIsSafe() {
    QTemporaryDir dir;
    QPointer<AcpHarnessSession> session = new AcpHarnessSession;
    connect(session, &AcpHarnessSession::event, session,
            [&] { delete session.data(); });
    QVERIFY(session->startTurn(options(dir.path()), input()));
    QTRY_VERIFY(!session);
  }
  void cancelFromOutputIsSafe() {
    QTemporaryDir dir;
    AcpHarnessSession session;
    QSignalSpy done(&session, &AcpHarnessSession::finished);
    connect(&session, &AcpHarnessSession::event, &session,
            [&] { session.interrupt(); });
    QVERIFY(session.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][0].toJsonObject()["status"].toString(),
             QString("interrupted"));
  }
  void foreignWorkspaceDoesNotResume() {
    QTemporaryDir a, b;
    AcpHarnessSession session;
    QSignalSpy done(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(options(a.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    const auto state = session.sessionState();
    session.shutdown();
    const auto path = b.filePath("calls");
    qputenv("ADE_ACP_LOG", path.toUtf8());
    QVERIFY(session.startTurn(options(b.path()), input(), state));
    QTRY_COMPARE(done.size(), 2);
    QVERIFY(log(path, "session/resume").isEmpty());
    QCOMPARE(log(path, "session/new").size(), 1);
  }
  void freshForkDoesNotReuseLiveSession() {
    QTemporaryDir dir;
    const auto path = dir.filePath("calls");
    qputenv("ADE_ACP_LOG", path.toUtf8());
    AcpHarnessSession session;
    QSignalSpy done(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    auto saved = session.sessionState();
    saved["fork"] = true;
    QVERIFY(session.startTurn(options(dir.path()), input(), saved));
    QTRY_COMPARE(done.size(), 2);
    QCOMPARE(log(path, "session/new").size(), 2);
    QCOMPARE(log(path, "session/resume").size(), 0);
    QCOMPARE(QJsonDocument(log(path, "session/prompt").last().toObject())
                 .toJson()
                 .count("PRIOR_HISTORY"),
             1);
  }
  void noResumeCapabilityUsesFreshHistory() {
    QTemporaryDir dir;
    AcpHarnessSession session;
    QSignalSpy done(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    const auto saved = session.sessionState();
    session.shutdown();
    qputenv("ADE_ACP_SCENARIO", "no-resume");
    const auto path = dir.filePath("calls");
    qputenv("ADE_ACP_LOG", path.toUtf8());
    QVERIFY(session.startTurn(options(dir.path()), input(), saved));
    QTRY_COMPARE(done.size(), 2);
    QCOMPARE(log(path, "session/resume").size(), 0);
    QCOMPARE(log(path, "session/new").size(), 1);
    QVERIFY(QJsonDocument(log(path, "session/prompt").first().toObject())
                .toJson()
                .contains("PRIOR_HISTORY"));
  }
  void mergedToolsMessageIdsAndUsageSnapshots() {
    qputenv("ADE_ACP_SCENARIO", "updates");
    QTemporaryDir dir;
    AcpHarnessSession session;
    QSignalSpy done(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    const auto result = done.first().first().toJsonObject();
    QCOMPARE(result["usage"].toObject()["used"].toInt(), 50);
    const auto events = result["events"].toArray();
    QString first, last;
    QJsonObject merged;
    int plan = 0;
    for (const auto &v : events) {
      const auto e = v.toObject();
      if (e["text"] == "A")
        first = e["id"].toString();
      if (e["text"] == "B")
        last = e["id"].toString();
      if (e["type"] == "tool")
        merged = e["update"].toObject();
      if (e["type"] == "plan")
        ++plan;
      QVERIFY(!e["turnId"].toString().isEmpty());
    }
    QCOMPARE(first, last);
    QVERIFY(!first.isEmpty());
    QCOMPARE(merged["title"].toString(), QString("Fixture command"));
    QCOMPARE(merged["rawInput"].toObject()["command"].toString(),
             QString("fixture"));
    QCOMPARE(merged["status"].toString(), QString("completed"));
    QCOMPARE(merged["_meta"]
                 .toObject()["terminal_output"]
                 .toObject()["data"]
                 .toString(),
             QString("OUTPUT"));
    QCOMPARE(plan, 2);
  }
  void thinkingOffUsesAdvertisedChoice() {
    qputenv("ADE_ACP_SCENARIO", "thinking-off");
    QTemporaryDir dir;
    const auto path = dir.filePath("calls");
    qputenv("ADE_ACP_LOG", path.toUtf8());
    AcpHarnessSession session;
    auto o = options(dir.path());
    o.thinkingDisabled = true;
    QSignalSpy done(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(o, input()));
    QTRY_COMPARE(done.size(), 1);
    const auto calls = log(path, "session/set_config_option");
    QCOMPARE(calls.size(), 2);
    QCOMPARE(calls.last().toObject()["value"].toString(), QString("none"));
    const auto caps = log(path, "initialize")
                          .first()
                          .toObject()["clientCapabilities"]
                          .toObject();
    QVERIFY(caps["session"]
                .toObject()["configOptions"]
                .toObject()["boolean"]
                .isObject());
    QVERIFY(!caps.contains("fs"));
    QVERIFY(!caps.contains("terminal"));
  }
  void cancellationHasBoundedKillFallback() {
    qputenv("ADE_ACP_SCENARIO", "ignore-cancel");
    QTemporaryDir dir;
    AcpHarnessSession session;
    QSignalSpy events(&session, &AcpHarnessSession::event),
        done(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(options(dir.path()), input()));
    QTRY_VERIFY(!events.isEmpty());
    QElapsedTimer elapsed;
    elapsed.start();
    session.interrupt();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QVERIFY(elapsed.elapsed() < 4500);
    QCOMPARE(done.first().first().toJsonObject()["status"].toString(),
             QString("interrupted"));
  }
  void pendingDecisionCanceledAndTokenCannotReachReplacement() {
    qputenv("ADE_ACP_SCENARIO", "approval");
    QTemporaryDir dir;
    AcpHarnessSession session;
    QSignalSpy ask(&session, &AcpHarnessSession::approvalRequired),
        done(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(ask.size(), 1);
    const auto old = ask.first().first().toString();
    session.interrupt();
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(!session.answerApproval(old, true));
    QVERIFY(session.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(ask.size(), 2);
    QVERIFY(!session.answerApproval(old, true));
    QVERIFY(session.answerApproval(ask.last().first().toString(), false));
    QTRY_COMPARE(done.size(), 2);
  }
  void decisionObserverCanDeleteSession() {
    qputenv("ADE_ACP_SCENARIO", "approval");
    QTemporaryDir dir;
    QPointer<AcpHarnessSession> session = new AcpHarnessSession;
    connect(session, &AcpHarnessSession::approvalRequired, session,
            [&] { delete session.data(); });
    QVERIFY(session->startTurn(options(dir.path()), input()));
    QTRY_VERIFY(!session);
  }
  void structuredFormsPreserveChoicesAndRejectInvalidAnswers() {
    QJsonObject props{
        {"count",
         QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 3}}},
        {"enabled", QJsonObject{{"type", "boolean"}}},
        {"choice",
         QJsonObject{
             {"type", "string"},
             {"oneOf",
              QJsonArray{QJsonObject{{"const", "a"}, {"title", "Alpha"}},
                         QJsonObject{{"const", "b"}, {"title", "Beta"}}}}}},
        {"many",
         QJsonObject{{"type", "array"},
                     {"items", QJsonObject{{"type", "string"},
                                           {"enum", QJsonArray{"x", "y"}}}}}}};
    const QJsonObject request{
        {"mode", "form"},
        {"message", "Question"},
        {"requestedSchema",
         QJsonObject{{"type", "object"},
                     {"properties", props},
                     {"required", QJsonArray{"count", "choice", "many"}}}}};
    const auto form = acpQuestionForm(request);
    QVERIFY(!form.isEmpty());
    const QJsonObject valid{{"count", 2},
                            {"enabled", false},
                            {"choice", "b"},
                            {"many", QJsonArray{"x", "y"}}};
    QVERIFY(acpQuestionAnswers(request, valid));
    auto invalid = valid;
    invalid["count"] = 1.5;
    QVERIFY(!acpQuestionAnswers(request, invalid));
    invalid = valid;
    invalid["many"] = QJsonArray{"x", "x"};
    QVERIFY(!acpQuestionAnswers(request, invalid));
    invalid = valid;
    invalid["choice"] = "c";
    QVERIFY(!acpQuestionAnswers(request, invalid));
  }
  void unsupportedImagesNeverReachPrompt() {
    QTemporaryDir dir;
    const auto path = dir.filePath("calls");
    qputenv("ADE_ACP_LOG", path.toUtf8());
    AcpHarnessSession session;
    QSignalSpy done(&session, &AcpHarnessSession::finished);
    QJsonArray images{QJsonObject{
        {"type", "image"}, {"data", "AA=="}, {"mimeType", "image/png"}}};
    QVERIFY(session.startTurn(options(dir.path()), images));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done.first().first().toJsonObject()["status"].toString(),
             QString("failed"));
    QVERIFY(log(path, "session/prompt").isEmpty());
  }
  void catalogMalformedEncodingAndMetadataAreRejected() {
    QString error;
    QVERIFY(OpenCodeModelDiscovery::parse(QByteArray("MODEL\n{\n}\n"), &error)
                .isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(OpenCodeModelDiscovery::parse(QByteArray("A\xe2\x82", 3), &error)
                .isEmpty());
    QVERIFY(error.contains("UTF-8"));
  }
  void actualDiscoveryUsesNativeProcessAndWhitelistedMetadata() {
    QTemporaryDir dir;
    OpenCodeModelDiscovery service;
    QSignalSpy done(&service, &OpenCodeModelDiscovery::completed);
    QVERIFY(service.start(options(dir.path()).executable));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY2(done.first()[1].toString().isEmpty(),
             qPrintable(done.first()[1].toString()));
    const auto models = done.first()[0].toJsonArray();
    QCOMPARE(models.size(), 2);
    QCOMPARE(models.first().toObject()["id"].toString(), QString("default"));
    const auto model = models.last().toObject();
    QCOMPARE(model["id"].toString(), QString("opencode/fixture"));
    QCOMPARE(model["inputTokenLimit"].toInt(), 128000);
    QVERIFY(model["inputModalities"].toArray().contains("image"));
    QVERIFY(
        !QJsonDocument(models).toJson().contains("SYNTHETIC_SECRET_CATALOG"));
  }
  void discoveryFailureIsRedactedAndBounded_data() {
    QTest::addColumn<QString>("scenario");
    QTest::newRow("exit") << "models-error";
    QTest::newRow("bound") << "models-large";
  }
  void discoveryFailureIsRedactedAndBounded() {
    QFETCH(QString, scenario);
    qputenv("ADE_ACP_SCENARIO", scenario.toUtf8());
    QTemporaryDir dir;
    OpenCodeModelDiscovery service;
    QSignalSpy done(&service, &OpenCodeModelDiscovery::completed);
    QVERIFY(service.start(options(dir.path()).executable));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(done.first()[0].toJsonArray().isEmpty());
    QVERIFY(!done.first()[1].toString().isEmpty());
    QVERIFY(!done.first()[1].toString().contains("SYNTHETIC_SECRET"));
  }
  void discoveryCancelAndObserverDeletion() {
    qputenv("ADE_ACP_SCENARIO", "models-hold");
    QTemporaryDir dir;
    OpenCodeModelDiscovery service;
    QSignalSpy done(&service, &OpenCodeModelDiscovery::completed);
    QVERIFY(service.start(options(dir.path()).executable));
    QTest::qWait(30);
    service.cancel();
    QVERIFY(!service.isBusy());
    QCOMPARE(done.size(), 0);
    qunsetenv("ADE_ACP_SCENARIO");
    QPointer<OpenCodeModelDiscovery> next = new OpenCodeModelDiscovery;
    connect(next, &OpenCodeModelDiscovery::completed, next,
            [&] { delete next.data(); });
    QVERIFY(next->start(options(dir.path()).executable));
    QTRY_VERIFY(!next);
  }
};
QTEST_GUILESS_MAIN(AcpTest)
#include "test_acp.moc"
