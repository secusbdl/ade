#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class ClaudeEngineTest : public QObject {
  Q_OBJECT
  ProviderConfig provider() const {
    ProviderConfig p;
    p.id = "claude-fixture";
    p.name = "Claude";
    p.kind = "claude-code";
    p.protocol = "claude-control";
    p.model = "default";
    p.harnessExecutable = QDir(QCoreApplication::applicationDirPath())
                              .filePath("claude_fixture.exe");
    return p;
  }
  void configure(AgentEngine &e, const QString &path,
                 const QString &owner = "owner") {
    e.setProvider(provider());
    e.setHarnessContext(owner, path);
  }
  QJsonArray prompts(const QString &path) const {
    QFile f(path);
    QJsonArray out;
    if (f.open(QIODevice::ReadOnly))
      for (const auto &line : f.readAll().split('\n')) {
        const auto row = QJsonDocument::fromJson(line).object();
        if (row.value("type") == "user")
          out.append(row);
      }
    return out;
  }
private slots:
  void cleanup() {
    qunsetenv("ADE_CLAUDE_FIXTURE");
    qunsetenv("ADE_CLAUDE_LOG");
  }
  void noUrlOrKeyUsesRealNativeProcessAndRetainsDisplay() {
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, dir.path());
    QSignalSpy done(&e, &AgentEngine::finished),
        text(&e, &AgentEngine::textDelta);
    QVERIFY(e.submitWithAttachments("Hello", {}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(e.lastTurnSucceeded());
    QCOMPARE(e.messages().last().toObject().value("content").toString(),
             QString::fromUtf8("TEXT_😀"));
    QString deltas;
    for (const auto &row : text)
      deltas += row[0].toString();
    QCOMPARE(deltas, QString::fromUtf8("TEXT_😀"));
    const auto metadata =
        e.messages().last().toObject().value("metadata").toObject();
    QCOMPARE(metadata.value("harness").toObject().value("kind").toString(),
             QString("claude-code"));
    QVERIFY(!metadata.value("display").toObject().isEmpty());
  }
  void newResumeForkAndProviderReplacementPreserveCanonicalHistory() {
    QTemporaryDir dir;
    const auto log = dir.filePath("calls");
    qputenv("ADE_CLAUDE_LOG", log.toUtf8());
    AgentEngine e;
    configure(e, dir.path());
    e.setMessages({QJsonObject{{"role", "user"}, {"content", "PRIOR_USER"}},
                   QJsonObject{{"role", "assistant"},
                               {"content", "PRIOR_ASSISTANT"},
                               {"metadata",
                                QJsonObject{{"doNotForward",
                                             "PRIVATE_METADATA_SENTINEL"}}}}});
    e.setSystemContext("WRONG_NATIVE_API_SENTINEL readContext apps/");
    e.setExternalHarnessContext("RIGHT_EXTERNAL_CONTEXT");
    QSignalSpy done(&e, &AgentEngine::finished);
    QVERIFY(e.submitWithAttachments("FIRST", {}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(e.submitWithAttachments("SECOND", {}));
    QTRY_COMPARE(done.size(), 2);
    auto sent = prompts(log);
    QCOMPARE(sent.size(), 2);
    QVERIFY(QJsonDocument(sent[0].toObject()).toJson().contains("PRIOR_USER"));
    QVERIFY(!QJsonDocument(sent[1].toObject()).toJson().contains("PRIOR_USER"));
    const auto saved = e.messages();
    const auto oldId = saved.last()
                           .toObject()
                           .value("metadata")
                           .toObject()
                           .value("harness")
                           .toObject()
                           .value("sessionId");
    AgentEngine reopened;
    configure(reopened, dir.path());
    reopened.setMessages(saved);
    QSignalSpy again(&reopened, &AgentEngine::finished);
    QVERIFY(reopened.submitWithAttachments("REOPEN", {}));
    QTRY_COMPARE(again.size(), 1);
    QCOMPARE(reopened.messages()
                 .last()
                 .toObject()
                 .value("metadata")
                 .toObject()
                 .value("harness")
                 .toObject()
                 .value("sessionId"),
             oldId);
    AgentEngine fork;
    configure(fork, dir.path(), "child");
    fork.setMessages(saved);
    QSignalSpy forkDone(&fork, &AgentEngine::finished);
    QVERIFY(fork.submitWithAttachments("FORK", {}));
    QTRY_COMPARE(forkDone.size(), 1);
    QVERIFY(fork.messages()
                .last()
                .toObject()
                .value("metadata")
                .toObject()
                .value("harness")
                .toObject()
                .value("sessionId") != oldId);
    QCOMPARE(e.messages(), saved);
    sent = prompts(log);
    QCOMPARE(sent.size(), 4);
    for (int i = 1; i < sent.size(); ++i)
      QVERIFY(!QJsonDocument(sent[i].toObject())
                   .toJson()
                   .contains("conversation-history"));
    const auto bytes = QJsonDocument(sent).toJson();
    QVERIFY(bytes.contains("RIGHT_EXTERNAL_CONTEXT"));
    QVERIFY(!bytes.contains("WRONG_NATIVE_API_SENTINEL"));
    QVERIFY(!bytes.contains("PRIVATE_METADATA_SENTINEL"));
    auto http = provider();
    http.kind = "api";
    http.protocol = "openai";
    http.harnessExecutable = {};
    http.baseUrl = "https://example.invalid/v1";
    e.setProvider(http);
    QCOMPARE(e.messages(), saved);
  }
  void permissionAnswerSharesActualToolPart_data() {
    QTest::addColumn<bool>("allow");
    QTest::newRow("allow") << true;
    QTest::newRow("deny") << false;
  }
  void permissionAnswerSharesActualToolPart() {
    QFETCH(bool, allow);
    qputenv("ADE_CLAUDE_FIXTURE", "permission-turn");
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, dir.path());
    QSignalSpy done(&e, &AgentEngine::finished),
        started(&e, &AgentEngine::toolStarted),
        finished(&e, &AgentEngine::toolFinished);
    connect(&e, &AgentEngine::approvalRequired, &e, [&](const QString &id) {
      QVERIFY(id.startsWith("claude-code/"));
      e.approveTool(id, allow);
    });
    QVERIFY(e.submitWithAttachments("Read with permission", {}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(e.lastTurnSucceeded());
    QCOMPARE(started.size(), 1);
    QCOMPARE(finished.size(), 1);
    QCOMPARE(started[0][0], finished[0][0]);
    QCOMPARE(finished[0][2].toBool(), !allow);
    const auto metadata =
        e.messages().last().toObject().value("metadata").toObject();
    QCOMPARE(
        metadata.value("harnessUsage").toObject().value("input_tokens").toInt(),
        7);
  }
  void answerFromDisplayChangedDoesNotEmitStaleApproval() {
    qputenv("ADE_CLAUDE_FIXTURE", "permission-turn");
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, dir.path());
    QSignalSpy done(&e, &AgentEngine::finished),
        asks(&e, &AgentEngine::approvalRequired);
    connect(&e, &AgentEngine::displayChanged, &e, [&] {
      for (const auto &row : e.displayMessages())
        for (const auto &value : row.toObject().value("parts").toArray()) {
          const auto part = value.toObject();
          if (part.value("state") == "approval-requested")
            e.approveTool(part.value("toolCallId").toString(), true);
        }
    });
    QVERIFY(e.submitWithAttachments("Read", {}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(e.lastTurnSucceeded());
    QCOMPARE(asks.size(), 0);
  }
  void questionAnswerCancelAndStaleTokens() {
    qputenv("ADE_CLAUDE_FIXTURE", "question-turn");
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, dir.path());
    e.setQuestionToolEnabled(true);
    QSignalSpy done(&e, &AgentEngine::finished);
    QVERIFY(e.submitWithAttachments("Question", {}));
    QTRY_VERIFY(!e.pendingQuestion().isEmpty());
    const auto token = e.pendingQuestion().value("id").toString();
    QVERIFY(!e.submitQuestionStep(token, 0,
                                  {{"q0", "o9"}, {"q1", QJsonArray{"o1"}}}));
    QCOMPARE(e.pendingQuestion().value("id").toString(), token);
    QVERIFY(e.submitQuestionStep(
        token, 0, {{"q0", "__other__:typed"}, {"q1", QJsonArray{"o0", "o1"}}}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(e.lastTurnSucceeded());
    QVERIFY(e.pendingQuestion().isEmpty());
    QVERIFY(e.submitWithAttachments("Again", {}));
    QTRY_VERIFY(!e.pendingQuestion().isEmpty());
    QVERIFY(!e.cancelQuestion(token));
    QVERIFY(e.cancelQuestion(e.pendingQuestion().value("id").toString()));
    QTRY_COMPARE(done.size(), 2);
  }
  void wireCancellationClosesOnlyCurrentQuestion() {
    qputenv("ADE_CLAUDE_FIXTURE", "cancel-question");
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, dir.path());
    e.setQuestionToolEnabled(true);
    QSignalSpy changes(&e, &AgentEngine::questionChanged),
        done(&e, &AgentEngine::finished);
    QVERIFY(e.submitWithAttachments("Question", {}));
    QTRY_VERIFY(changes.size() >= 2);
    QVERIFY(e.pendingQuestion().isEmpty());
    e.cancel();
    QCOMPARE(done.size(), 1);
  }
  void queueSurvivesStopAndRestartUntilExplicitResume() {
    qputenv("ADE_CLAUDE_FIXTURE", "hold-turn");
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, dir.path());
    QSignalSpy done(&e, &AgentEngine::finished);
    QVERIFY(e.sendUserMessage("Working"));
    QVERIFY(e.sendUserMessage("Queued"));
    QCOMPARE(e.queuedMessages().size(), 1);
    e.cancel();
    QCOMPARE(done.size(), 1);
    const auto history = e.messages(), queue = e.queuedMessages();
    qunsetenv("ADE_CLAUDE_FIXTURE");
    AgentEngine reopened;
    configure(reopened, dir.path());
    reopened.setMessages(history);
    QVERIFY(reopened.setQueuedMessages(queue));
    QSignalSpy resumed(&reopened, &AgentEngine::finished);
    QTest::qWait(50);
    QCOMPARE(resumed.size(), 0);
    QVERIFY(reopened.resumeQueue());
    QTRY_COMPARE(resumed.size(), 1);
    QVERIFY(reopened.lastTurnSucceeded());
    QVERIFY(reopened.queuedMessages().isEmpty());
  }
  void imageBytesUseNativeContentAndKeepReferenceHistory() {
    QTemporaryDir dir;
    const auto log = dir.filePath("calls");
    qputenv("ADE_CLAUDE_LOG", log.toUtf8());
    AgentEngine e;
    configure(e, dir.path());
    e.setMessagePreparer(
        [](const QJsonArray &messages, const ProviderConfig &, QString *) {
          auto prepared = messages;
          auto row = prepared.last().toObject();
          row["content"] = QJsonArray{
              QJsonObject{{"type", "text"}, {"text", "Image prompt"}},
              QJsonObject{
                  {"type", "image_url"},
                  {"image_url",
                   QJsonObject{{"url", "data:image/png;base64,AQIDBA=="}}}}};
          prepared[prepared.size() - 1] = row;
          return prepared;
        });
    const QJsonArray refs{
        QJsonObject{{"path", "att/image.png"}, {"mimeType", "image/png"}}};
    QSignalSpy done(&e, &AgentEngine::finished);
    QVERIFY(e.submitWithAttachments("Image", refs));
    QTRY_COMPARE(done.size(), 1);
    const auto content = prompts(log)
                             .first()
                             .toObject()
                             .value("message")
                             .toObject()
                             .value("content")
                             .toArray();
    QCOMPARE(content.last().toObject().value("source").toObject(),
             (QJsonObject{{"type", "base64"},
                          {"media_type", "image/png"},
                          {"data", "AQIDBA=="}}));
    QCOMPARE(e.messages()
                 .first()
                 .toObject()
                 .value("metadata")
                 .toObject()
                 .value("attachments")
                 .toArray(),
             refs);
    QVERIFY(!QJsonDocument(e.messages()).toJson().contains("AQIDBA"));
  }
  void rejectionObserverDeletionDoesNotDispatch() {
    QTemporaryDir dir;
    QPointer<AgentEngine> e = new AgentEngine;
    auto p = provider();
    p.harnessExecutable = dir.filePath("missing.exe");
    e->setProvider(p);
    e->setHarnessContext("owner", dir.path());
    connect(e, &AgentEngine::failed, e, [&] { delete e.data(); });
    QVERIFY(!e->submitWithAttachments("Hello", {}));
    QVERIFY(!e);
  }
  void streamedObserverMayCancelOrDelete_data() {
    QTest::addColumn<bool>("destroy");
    QTest::newRow("cancel") << false;
    QTest::newRow("delete") << true;
  }
  void streamedObserverMayCancelOrDelete() {
    QFETCH(bool, destroy);
    QTemporaryDir dir;
    QPointer<AgentEngine> e = new AgentEngine;
    configure(*e, dir.path());
    int done = 0;
    connect(e, &AgentEngine::finished, qApp, [&] { ++done; });
    connect(e, &AgentEngine::textDelta, e, [&] {
      if (destroy)
        delete e.data();
      else
        e->cancel();
    });
    QVERIFY(e->submitWithAttachments("Hello", {}));
    QTRY_VERIFY(!e || done == 1);
    if (e) {
      QVERIFY(!e->lastTurnSucceeded());
      delete e.data();
    }
    QTest::qWait(30);
    QCOMPARE(done, destroy ? 0 : 1);
  }
  void encryptedStoreRoundTripAndNoApiCredentialAccepted() {
    QTemporaryDir dir;
    QString error;
    auto p = provider();
    p.disabledModelIds = {"hidden"};
    QVERIFY(ProviderStore::save(dir.filePath("providers.json"), {p}, &error));
    const auto loaded =
        ProviderStore::load(dir.filePath("providers.json"), &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded[0].kind, QString("claude-code"));
    QCOMPARE(loaded[0].disabledModelIds, QStringList{"hidden"});
    p.apiKey = "SYNTHETIC_KEY";
    QVERIFY(!ProviderStore::save(dir.filePath("providers.json"), {p}, &error));
  }
};
QTEST_GUILESS_MAIN(ClaudeEngineTest)
#include "test_claude_engine.moc"
