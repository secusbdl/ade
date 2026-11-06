#include "core/AgentEngine.h"
#include "core/ContextLoader.h"
#include "core/ProviderStore.h"
#include "ui/ModelSelection.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class AcpEngineTest : public QObject {
  Q_OBJECT
  ProviderConfig provider() {
    ProviderConfig p;
    p.id = "opencode-provider";
    p.name = "OpenCode";
    p.kind = "opencode";
    p.protocol = "opencode-acp";
    p.model = "model-b";
    p.templateId = "opencode";
    p.harnessExecutable = QDir(QCoreApplication::applicationDirPath())
                              .filePath("acp_fixture.exe");
    return p;
  }
  void configure(AgentEngine &e, const QString &path,
                 const QString &owner = "owner") {
    e.setProvider(provider());
    e.setHarnessContext(owner, path);
  }
  QJsonArray calls(const QString &path, const QString &method) {
    QFile f(path);
    QJsonArray out;
    if (f.open(QIODevice::ReadOnly))
      for (const auto &line : f.readAll().split('\n')) {
        const auto v = QJsonDocument::fromJson(line).object();
        if (v["method"] == method)
          out.append(v["params"]);
      }
    return out;
  }
private slots:
  void cleanup() {
    qunsetenv("ADE_ACP_SCENARIO");
    qunsetenv("ADE_ACP_LOG");
    qunsetenv("ADE_HARNESS_FIXTURE_REQUESTS");
  }
  void harnessContextExcludesNativeToolClaims_data() {
    QTest::addColumn<bool>("codex");
    QTest::newRow("opencode") << false;
    QTest::newRow("codex") << true;
  }
  void harnessContextExcludesNativeToolClaims() {
    QFETCH(bool, codex);
    QTemporaryDir dir;
    const auto log = dir.filePath("calls");
    qputenv(codex ? "ADE_HARNESS_FIXTURE_REQUESTS" : "ADE_ACP_LOG",
            log.toUtf8());
    auto p = provider();
    if (codex) {
      p.kind = "codex";
      p.protocol = "codex-app-server";
      p.harnessExecutable = QDir(QCoreApplication::applicationDirPath())
                                .filePath("harness_fixture.exe");
      p.model = "model-a";
    }
    AgentEngine e;
    e.setProvider(p);
    e.setHarnessContext("context-owner", dir.path());
    e.setSystemContext(
        "NATIVE_ONLY_SENTINEL use readContext on apps/ and att/");
    ContextSnapshot context;
    context.instructions.append(
        {"AGENTS.md", "workspace",
         "EXTERNAL_CONTEXT_SENTINEL respected local instructions", true,
         false});
    context.skills.append({"native-skill", "NOT_ADVERTISED_PLUGIN", "desc",
                           "plugin", "path", "plugins/native/SKILL.md",
                           "native", true, true});
    context.invokedSkills.append(
        {"invoked", "skill-ref", "INVOKED_BODY_SENTINEL"});
    e.setExternalHarnessContext(ContextLoader::externalHarnessContext(context));
    QSignalSpy done(&e, &AgentEngine::finished);
    QVERIFY(e.submitWithAttachments("context check", {}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(e.lastTurnSucceeded());
    QFile file(log);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto wire = file.readAll();
    QVERIFY(wire.contains("EXTERNAL_CONTEXT_SENTINEL"));
    QVERIFY(!wire.contains("NATIVE_ONLY_SENTINEL"));
    QVERIFY(!wire.contains("use readContext"));
    QVERIFY(wire.contains("INVOKED_BODY_SENTINEL"));
    QVERIFY(!wire.contains("NOT_ADVERTISED_PLUGIN"));
  }
  void noKeyOrUrlAcceptsAndRetainsPresentation() {
    qputenv("ADE_ACP_SCENARIO", "updates");
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, dir.path());
    QSignalSpy done(&e, &AgentEngine::finished),
        tools(&e, &AgentEngine::toolStarted);
    QVERIFY(e.submitWithAttachments("hi", {}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(e.lastTurnSucceeded());
    QCOMPARE(e.messages().last().toObject()["content"].toString(),
             QString("AB"));
    QVERIFY(!tools.isEmpty());
    const auto metadata = e.messages().last().toObject()["metadata"].toObject();
    QCOMPARE(metadata["harness"].toObject()["kind"].toString(),
             QString("opencode"));
    QCOMPARE(metadata["harnessUsage"].toObject()["used"].toInt(), 50);
    QVERIFY(!metadata["display"].toObject().isEmpty());
    QVERIFY(QJsonDocument(e.displayMessages()).toJson().contains("OUTPUT"));
  }
  void newResumeForkAndHttpPreserveHistory() {
    QTemporaryDir dir;
    const auto log = dir.filePath("calls");
    qputenv("ADE_ACP_LOG", log.toUtf8());
    AgentEngine source;
    configure(source, dir.path());
    source.setMessages(
        {QJsonObject{{"role", "user"}, {"content", "PRIOR_USER"}},
         QJsonObject{
             {"role", "assistant"},
             {"content", "PRIOR_ASSISTANT"},
             {"metadata", QJsonObject{{"secretMarker", "DO_NOT_FORWARD"}}}}});
    QSignalSpy done(&source, &AgentEngine::finished);
    QVERIFY(source.submitWithAttachments("FIRST", {}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(source.submitWithAttachments("SECOND", {}));
    QTRY_COMPARE(done.size(), 2);
    auto prompts = calls(log, "session/prompt");
    QCOMPARE(prompts.size(), 2);
    QVERIFY(QJsonDocument(prompts.first().toObject())
                .toJson()
                .contains("PRIOR_USER"));
    QVERIFY(!QJsonDocument(prompts.first().toObject())
                 .toJson()
                 .contains("DO_NOT_FORWARD"));
    QVERIFY(!QJsonDocument(prompts.last().toObject())
                 .toJson()
                 .contains("conversation-history"));
    const auto saved = source.messages();
    AgentEngine resumed;
    configure(resumed, dir.path());
    resumed.setMessages(saved);
    QSignalSpy resumedDone(&resumed, &AgentEngine::finished);
    QVERIFY(resumed.submitWithAttachments("RESTART", {}));
    QTRY_COMPARE(resumedDone.size(), 1);
    QCOMPARE(calls(log, "session/resume").size(), 1);
    AgentEngine fork;
    configure(fork, dir.path(), "fork");
    fork.setMessages(saved);
    QSignalSpy forkDone(&fork, &AgentEngine::finished);
    QVERIFY(fork.submitWithAttachments("FORK", {}));
    QTRY_COMPARE(forkDone.size(), 1);
    QCOMPARE(calls(log, "session/new").size(), 2);
    QCOMPARE(source.messages(), saved);
    auto http = provider();
    http.kind = "api";
    http.protocol = "openai";
    http.harnessExecutable = {};
    http.baseUrl = "https://example.invalid/v1";
    source.setProvider(http);
    QCOMPARE(source.messages(), saved);
  }
  void approvalDenialContinuesTurn() {
    qputenv("ADE_ACP_SCENARIO", "approval");
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, dir.path());
    QSignalSpy done(&e, &AgentEngine::finished);
    connect(&e, &AgentEngine::approvalRequired, &e, [&](const QString &id) {
      QVERIFY(id.startsWith("opencode/"));
      e.approveTool(id, false);
    });
    QVERIFY(e.submitWithAttachments("approval", {}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(e.lastTurnSucceeded());
    QVERIFY(
        QJsonDocument(e.displayMessages()).toJson().contains("output-error"));
  }
  void duplicatePendingToolIdentityKeepsFirstApprovalUsable() {
    qputenv("ADE_ACP_SCENARIO", "duplicate-approval");
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, dir.path());
    QSignalSpy ask(&e, &AgentEngine::approvalRequired),
        done(&e, &AgentEngine::finished);
    QVERIFY(e.submitWithAttachments("duplicate fixture", {}));
    QTRY_VERIFY(QJsonDocument(e.displayMessages())
                    .toJson()
                    .contains("DUPLICATE_SENT_SENTINEL"));
    QCOMPARE(ask.size(), 1);
    e.approveTool(ask[0][0].toString(), true);
    QTRY_COMPARE(ask.size(), 2);
    e.approveTool(ask[1][0].toString(), true);
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(e.lastTurnSucceeded());
  }
  void nativeQuestionAnswerAndCancel() {
    qputenv("ADE_ACP_SCENARIO", "question");
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, dir.path());
    e.setQuestionToolEnabled(true);
    QSignalSpy done(&e, &AgentEngine::finished);
    QVERIFY(e.submitWithAttachments("question", {}));
    QTRY_VERIFY(!e.pendingQuestion().isEmpty());
    const auto q = e.pendingQuestion();
    const auto token = q["id"].toString();
    QVERIFY(!token.isEmpty());
    QVERIFY(!e.submitQuestionStep(token, 0, {{"name", ""}}));
    QVERIFY(e.submitQuestionStep(token, 0, {{"name", "valid"}}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(e.lastTurnSucceeded());
    QVERIFY(e.pendingQuestion().isEmpty());
    QVERIFY(e.submitWithAttachments("question again", {}));
    QTRY_VERIFY(!e.pendingQuestion().isEmpty());
    QVERIFY(e.cancelQuestion(e.pendingQuestion()["id"].toString()));
    QTRY_COMPARE(done.size(), 2);
  }
  void integerQuestionRejectsFractionWithoutLosingForm() {
    qputenv("ADE_ACP_SCENARIO", "integer-question");
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, dir.path());
    e.setQuestionToolEnabled(true);
    QSignalSpy done(&e, &AgentEngine::finished);
    QVERIFY(e.submitWithAttachments("integer", {}));
    QTRY_VERIFY(!e.pendingQuestion().isEmpty());
    const auto id = e.pendingQuestion()["id"].toString();
    QVERIFY(!e.submitQuestionStep(id, 0, {{"name", 1.5}}));
    QCOMPARE(e.pendingQuestion()["id"].toString(), id);
    QVERIFY(e.submitQuestionStep(id, 0, {{"name", 2}}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(e.lastTurnSucceeded());
  }
  void approvalObserverCanCancelWithoutStaleCompletion() {
    qputenv("ADE_ACP_SCENARIO", "approval");
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, dir.path());
    QSignalSpy done(&e, &AgentEngine::finished);
    connect(&e, &AgentEngine::approvalRequired, &e, [&] { e.cancel(); });
    QVERIFY(e.submitWithAttachments("cancel approval", {}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(!e.isBusy());
    QVERIFY(!e.lastTurnSucceeded());
    QTest::qWait(50);
    QCOMPARE(done.size(), 1);
  }
  void cancelQueueResumeAndLateToken() {
    qputenv("ADE_ACP_SCENARIO", "hold");
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, dir.path());
    QSignalSpy done(&e, &AgentEngine::finished),
        reasoning(&e, &AgentEngine::reasoningDelta);
    QVERIFY(e.sendUserMessage("first"));
    QTRY_VERIFY(!reasoning.isEmpty());
    QVERIFY(e.sendUserMessage("queued"));
    QCOMPARE(e.queuedMessages().size(), 1);
    e.cancel();
    QTRY_COMPARE(done.size(), 1);
    const auto history = e.messages(), queue = e.queuedMessages();
    QVERIFY(!queue.isEmpty());
    qunsetenv("ADE_ACP_SCENARIO");
    AgentEngine restored;
    configure(restored, dir.path());
    restored.setMessages(history);
    QVERIFY(restored.setQueuedMessages(queue));
    QSignalSpy resumed(&restored, &AgentEngine::finished);
    QTest::qWait(50);
    QCOMPARE(resumed.size(), 0);
    QVERIFY(restored.resumeQueue());
    QTRY_COMPARE(resumed.size(), 1);
    QVERIFY(restored.lastTurnSucceeded());
    QVERIFY(restored.queuedMessages().isEmpty());
  }
  void displayObserverMayDeleteOwner() {
    QTemporaryDir dir;
    QPointer<AgentEngine> e = new AgentEngine;
    configure(*e, dir.path());
    connect(e, &AgentEngine::textDelta, e, [&] { delete e.data(); });
    QVERIFY(e->submitWithAttachments("delete", {}));
    QTRY_VERIFY(!e);
  }
  void modelSelectionKeepsExternalEfforts() {
    const auto p = provider();
    const auto catalog = cleanModelCatalog(
        p, {QJsonObject{{"id", "model-b"},
                        {"name", "Model B"},
                        {"inputTokenLimit", 1234},
                        {"reasoningEfforts", QJsonArray{"medium", "high"}},
                        {"inputModalities", QJsonArray{"text", "image"}}}});
    const auto item = catalog["models"].toArray().first().toObject();
    QCOMPARE(item["inputTokenLimit"].toInt(), 1234);
    QVERIFY(item["reasoningEfforts"].toArray().contains("high"));
    QVERIFY(item["inputModalities"].toArray().contains("image"));
  }
  void providerStoreRoundTripAndRouteIsolation() {
    QTemporaryDir dir;
    const auto file = dir.filePath("providers.json");
    auto p = provider();
    QString error;
    QVERIFY2(ProviderStore::save(file, {p}, &error), qPrintable(error));
    const auto loaded = ProviderStore::load(file, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.first().templateId, QString("opencode"));
    QVERIFY(loaded.first().isExternalHarness());
    p.apiKey = "synthetic";
    QVERIFY(!ProviderStore::save(file, {p}, &error));
    QCOMPARE(ProviderStore::load(file, &error).first().apiKey, QString{});
  }
};
QTEST_GUILESS_MAIN(AcpEngineTest)
#include "test_acp_engine.moc"
