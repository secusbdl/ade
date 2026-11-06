#include "AgentEngine.h"
#include "ProviderProtocol.h"
#include "ProviderStore.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class CodexEngineTest : public QObject {
  Q_OBJECT
  ProviderConfig provider() const {
    ProviderConfig p;
    p.id = "codex";
    p.name = "Codex";
    p.kind = "codex";
    p.protocol = "codex-app-server";
    p.model = "model-a";
    p.harnessExecutable = QDir(QCoreApplication::applicationDirPath())
                              .filePath("harness_fixture.exe");
    return p;
  }
  void configure(AgentEngine &engine, const QString &path,
                 const QString &id = "chat-a") {
    engine.setProvider(provider());
    engine.setHarnessContext(id, path);
  }
private slots:
  void cleanup() {
    qunsetenv("ADE_HARNESS_FIXTURE_SCENARIO");
    qunsetenv("ADE_HARNESS_FIXTURE_REQUESTS");
  }
  void priorTranscriptOnlyForNewSession() {
    QTemporaryDir dir;
    const auto path = dir.filePath("requests.jsonl");
    qputenv("ADE_HARNESS_FIXTURE_REQUESTS", path.toUtf8());
    auto turns = [&] {
      QJsonArray result;
      QFile file(path);
      if (!file.open(QIODevice::ReadOnly))
        return result;
      for (const auto &line : file.readAll().split('\n')) {
        const auto row = QJsonDocument::fromJson(line).object();
        if (row["method"] == "turn/start")
          result.append(row["params"]);
      }
      return result;
    };
    QJsonArray original{
        QJsonObject{{"role", "user"}, {"content", "OLD_BEFORE_BOUNDARY"}},
        QJsonObject{{"role", "assistant"}, {"content", "OLD_ANSWER"}},
        QJsonObject{{"role", "user"},
                    {"content", "KEPT_USER"},
                    {"ade_compressedHistory", "COMPRESSED_BRIEFING"}},
        QJsonObject{
            {"role", "assistant"},
            {"content", "KEPT_ASSISTANT"},
            {"tool_calls",
             QJsonArray{
                 QJsonObject{{"id", "old-call"},
                             {"type", "function"},
                             {"function", QJsonObject{{"name", "read"},
                                                      {"arguments", "{}"}}}}}},
            {"ade_native",
             QJsonObject{{"protocol", "opaque-fixture"},
                         {"content", QJsonArray{"SIGNED_DO_NOT_COPY"}}}},
            {"metadata",
             QJsonObject{{"privateSentinel", "METADATA_DO_NOT_COPY"}}}},
        QJsonObject{{"role", "tool"},
                    {"tool_call_id", "old-call"},
                    {"content", "TOOL_DO_NOT_COPY"}}};
    AgentEngine engine;
    configure(engine, dir.path());
    engine.setMessages(original);
    QSignalSpy done(&engine, &AgentEngine::finished);
    engine.submit("CURRENT_ONE");
    QTRY_COMPARE(done.size(), 1);
    auto sent = turns();
    QCOMPARE(sent.size(), 1);
    const auto first = QJsonDocument(sent.first().toObject()).toJson();
    QVERIFY(first.contains("COMPRESSED_BRIEFING"));
    QVERIFY(first.contains("KEPT_USER"));
    QVERIFY(first.contains("KEPT_ASSISTANT"));
    QVERIFY(!first.contains("OLD_BEFORE_BOUNDARY"));
    QVERIFY(!first.contains("TOOL_DO_NOT_COPY"));
    QVERIFY(!first.contains("SIGNED_DO_NOT_COPY"));
    QVERIFY(!first.contains("METADATA_DO_NOT_COPY"));
    engine.submit("CURRENT_TWO");
    QTRY_COMPARE(done.size(), 2);
    sent = turns();
    QCOMPARE(sent.size(), 2);
    const auto second = QJsonDocument(sent.last().toObject()).toJson();
    QVERIFY(second.contains("CURRENT_TWO"));
    QVERIFY(!second.contains("CURRENT_ONE"));
    QVERIFY(!second.contains("conversation-history"));
    AgentEngine restarted;
    configure(restarted, dir.path());
    restarted.setMessages(engine.messages());
    QSignalSpy restartedDone(&restarted, &AgentEngine::finished);
    restarted.submit("RESTARTED");
    QTRY_COMPARE(restartedDone.size(), 1);
    sent = turns();
    QVERIFY(!QJsonDocument(sent.last().toObject())
                 .toJson()
                 .contains("conversation-history"));
    AgentEngine fork;
    configure(fork, dir.path(), "chat-fork");
    fork.setMessages(engine.messages());
    QSignalSpy forkDone(&fork, &AgentEngine::finished);
    fork.submit("FORKED");
    QTRY_COMPARE(forkDone.size(), 1);
    sent = turns();
    QVERIFY(!QJsonDocument(sent.last().toObject())
                 .toJson()
                 .contains("conversation-history"));
    QTemporaryDir other;
    engine.setWorkspace(other.path());
    engine.submit("MOVED_WORKSPACE");
    QTRY_COMPARE(done.size(), 3);
    sent = turns();
    const auto moved = QJsonDocument(sent.last().toObject()).toJson();
    QVERIFY(moved.contains("conversation-history"));
    QVERIFY(moved.contains("CURRENT_TWO"));
    QVERIFY(!moved.contains("TOOL_DO_NOT_COPY"));
  }
  void failedResumeRebuildsCanonicalHistoryOnce() {
    QTemporaryDir dir;
    AgentEngine source;
    configure(source, dir.path());
    QSignalSpy sourceDone(&source, &AgentEngine::finished);
    source.submit("PRIOR_USER");
    QTRY_COMPARE(sourceDone.size(), 1);
    const auto original = source.messages();
    const auto log = dir.filePath("recovery.jsonl");
    qputenv("ADE_HARNESS_FIXTURE_REQUESTS", log.toUtf8());
    qputenv("ADE_HARNESS_FIXTURE_SCENARIO", "resume-error");
    AgentEngine restored;
    configure(restored, dir.path());
    restored.setMessages(original);
    QSignalSpy done(&restored, &AgentEngine::finished);
    restored.submit("CURRENT_RECOVERY");
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(restored.lastTurnSucceeded());
    QCOMPARE(source.messages(), original);
    QFile file(log);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QStringList methods;
    QJsonArray turns;
    for (const auto &line : file.readAll().split('\n')) {
      const auto row = QJsonDocument::fromJson(line).object();
      const auto method = row["method"].toString();
      if (method.startsWith("thread/"))
        methods.append(method);
      if (method == "turn/start")
        turns.append(row["params"]);
    }
    QCOMPARE(methods, QStringList({"thread/resume", "thread/start"}));
    QCOMPARE(turns.size(), 1);
    const auto wire = QJsonDocument(turns).toJson();
    QCOMPARE(wire.count("PRIOR_USER"), 1);
    QCOMPARE(wire.count("HARNESS_OK"), 1);
    QCOMPARE(wire.count("CURRENT_RECOVERY"), 1);
    QVERIFY(!wire.contains("workspaceIdentity"));
    QVERIFY(!wire.contains("harnessUsage"));
    QCOMPARE(restored.messages().size(), original.size() + 2);
  }
  void nonsecretProviderRoundtrip() {
    QTemporaryDir dir;
    const auto path = dir.filePath("providers.json");
    QString error;
    QVERIFY2(ProviderStore::save(path, {provider()}, &error),
             qPrintable(error));
    auto rows = ProviderStore::load(path, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(rows.size(), 1);
    QVERIFY(rows.first().isCodexHarness());
    QVERIFY(rows.first().apiKey.isEmpty());
    QCOMPARE(rows.first().harnessExecutable, provider().harnessExecutable);
  }
  void mixedCredentialRejectedWithoutOverwrite() {
    QTemporaryDir dir;
    const auto path = dir.filePath("providers.json");
    QString error;
    QVERIFY(ProviderStore::save(path, {provider()}, &error));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto original = file.readAll();
    file.close();
    auto invalid = provider();
    invalid.apiKey = "synthetic-key";
    QVERIFY(!ProviderStore::save(path, {invalid}, &error));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original);
  }
  void streamsPersistsAndRestarts() {
    QTemporaryDir dir;
    AgentEngine engine;
    configure(engine, dir.path());
    QSignalSpy finished(&engine, &AgentEngine::finished),
        text(&engine, &AgentEngine::textDelta);
    QVERIFY(engine.submitWithAttachments("hi", {}));
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY(engine.lastTurnSucceeded());
    QVERIFY(!text.isEmpty());
    QCOMPARE(engine.messages().last().toObject()["content"].toString(),
             QString("HARNESS_OK"));
    const auto history = engine.messages();
    const auto metadata = history.last().toObject()["metadata"].toObject();
    QCOMPARE(metadata["harness"].toObject()["ownerId"].toString(),
             QString("chat-a"));
    QVERIFY(!metadata["display"].toObject()["parts"].toArray().isEmpty());
    AgentEngine restarted;
    configure(restarted, dir.path());
    restarted.setMessages(history);
    QSignalSpy second(&restarted, &AgentEngine::finished);
    restarted.submit("again");
    QTRY_COMPARE(second.size(), 1);
    QVERIFY(restarted.lastTurnSucceeded());
    QCOMPARE(restarted.messages()
                 .last()
                 .toObject()["metadata"]
                 .toObject()["harness"]
                 .toObject()["threadId"]
                 .toString(),
             QString("thread-1"));
    ProviderConfig api;
    api.protocol = "openai";
    api.baseUrl = "http://127.0.0.1:1";
    api.model = "fake";
    const auto wire =
        ProviderProtocol::prepare(api, restarted.messages(), "", {}).body;
    const auto bytes = QJsonDocument(wire).toJson();
    QVERIFY(!bytes.contains("harnessExecutable"));
    QVERIFY(!bytes.contains("thread-1"));
    QVERIFY(!bytes.contains("ownerId"));
  }
  void forkHistoryDoesNotResumeSourceThread() {
    QTemporaryDir dir;
    AgentEngine source;
    configure(source, dir.path());
    QSignalSpy done(&source, &AgentEngine::finished);
    source.submit("hi");
    QTRY_COMPARE(done.size(), 1);
    const auto original = source.messages();
    AgentEngine fork;
    configure(fork, dir.path(), "chat-b");
    fork.setMessages(original);
    QSignalSpy forkDone(&fork, &AgentEngine::finished);
    fork.submit("fork");
    QTRY_COMPARE(forkDone.size(), 1);
    QVERIFY(fork.lastTurnSucceeded());
    QCOMPARE(fork.messages()
                 .last()
                 .toObject()["metadata"]
                 .toObject()["harness"]
                 .toObject()["threadId"]
                 .toString(),
             QString("thread-fork"));
    QCOMPARE(source.messages(), original);
  }
  void actualApprovalUsesNativeEngineControl() {
    qputenv("ADE_HARNESS_FIXTURE_SCENARIO", "approval");
    QTemporaryDir dir;
    AgentEngine engine;
    configure(engine, dir.path());
    QSignalSpy approvals(&engine, &AgentEngine::approvalRequired),
        done(&engine, &AgentEngine::finished);
    engine.submit("tool");
    QTRY_COMPARE(approvals.size(), 1);
    const auto id = approvals.first()[0].toString();
    QVERIFY(id.startsWith("codex/"));
    engine.approveTool(id, true);
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(engine.lastTurnSucceeded());
    QVERIFY(!engine.displayMessages()
                 .last()
                 .toObject()["parts"]
                 .toArray()
                 .isEmpty());
  }
  void actualQuestionUsesNativeForm() {
    qputenv("ADE_HARNESS_FIXTURE_SCENARIO", "question");
    QTemporaryDir dir;
    AgentEngine engine;
    configure(engine, dir.path());
    engine.setQuestionToolEnabled(true);
    QSignalSpy done(&engine, &AgentEngine::finished);
    engine.submit("question");
    QTRY_VERIFY(!engine.pendingQuestion().isEmpty());
    const auto form = engine.pendingQuestion();
    QVERIFY(engine.submitQuestionStep(form["id"].toString(), 0,
                                      {{"choice", "One"}}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(engine.lastTurnSucceeded());
    QVERIFY(engine.pendingQuestion().isEmpty());
  }
  void queuedMessagesDrainAfterSuccessfulHarnessTurn() {
    QTemporaryDir dir;
    AgentEngine engine;
    configure(engine, dir.path());
    QSignalSpy done(&engine, &AgentEngine::finished);
    int commits = 0;
    engine.setQueueCommitter(
        [&](const QJsonArray &, const QJsonArray &, QString *) {
          ++commits;
          return true;
        });
    QVERIFY(engine.sendUserMessage("one"));
    QVERIFY(engine.sendUserMessage("two"));
    QTRY_COMPARE(done.size(), 2);
    QVERIFY(engine.queuedMessages().isEmpty());
    QVERIFY(commits >= 2);
    QVERIFY(engine.lastTurnSucceeded());
  }
  void cancelAtReasoningRetainsOnePartial() {
    QTemporaryDir dir;
    AgentEngine engine;
    configure(engine, dir.path());
    QSignalSpy done(&engine, &AgentEngine::finished);
    connect(&engine, &AgentEngine::reasoningDelta, &engine,
            &AgentEngine::cancel);
    engine.submit("cancel");
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(!engine.lastTurnSucceeded());
    QCOMPARE(engine.messages().size(), 2);
    QCOMPARE(engine.displayMessages().size(), 2);
    QVERIFY(!engine.displayMessages()
                 .last()
                 .toObject()["parts"]
                 .toArray()
                 .isEmpty());
  }
  void deletedEngineDuringStream() {
    QTemporaryDir dir;
    QPointer<AgentEngine> engine = new AgentEngine;
    configure(*engine, dir.path());
    connect(engine, &AgentEngine::textDelta, engine,
            [&] { delete engine.data(); });
    engine->submit("delete");
    QTRY_VERIFY(engine.isNull());
  }
};
QTEST_GUILESS_MAIN(CodexEngineTest)
#include "test_codex_engine.moc"
