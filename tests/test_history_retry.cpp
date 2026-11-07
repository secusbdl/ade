#include "QuestionFixture.h"
#include "core/AgentPresentation.h"
#include "core/TurnFailure.h"
#include <QUuid>
// Failed-turn Retry (agent-history design slice 1). Every test calls the new
// AgentEngine Retry API, so none can build or pass against the frozen cohort.
namespace {
void protocols() {
  QTest::addColumn<QString>("protocol");
  for (const auto *p : {"openai", "responses", "anthropic", "google"})
    QTest::newRow(p) << QString(p);
}
int lastUserIndex(const QJsonArray &history) {
  for (int i = history.size() - 1; i >= 0; --i)
    if (history[i].toObject()["role"] == "user")
      return i;
  return -1;
}
QString displayId(const QJsonObject &row) {
  return row["metadata"].toObject()["display"].toObject()["id"].toString();
}
QString lastUserId(const QJsonArray &history) {
  const int index = lastUserIndex(history);
  return index < 0 ? QString() : displayId(history[index].toObject());
}
QString projectedLastUserId(const QJsonArray &rows) {
  for (int i = rows.size() - 1; i >= 0; --i)
    if (rows[i].toObject()["role"] == "user")
      return rows[i].toObject()["id"].toString();
  return {};
}
QByteArray partial(const QString &protocol) {
  if (protocol == "anthropic")
    return sseEvent({{"type", "content_block_start"},
                     {"index", 0},
                     {"content_block",
                      QJsonObject{{"type", "text"}, {"text", "PARTIAL"}}}});
  if (protocol == "responses")
    return sseEvent({{"type", "response.output_text.delta"},
                     {"output_index", 0},
                     {"content_index", 0},
                     {"delta", "PARTIAL"}});
  if (protocol == "google")
    return sseEvent(
        {{"candidates",
          QJsonArray{QJsonObject{
              {"content",
               QJsonObject{{"role", "model"},
                           {"parts", QJsonArray{QJsonObject{
                                         {"text", "PARTIAL"}}}}}}}}}});
  return sseEvent({{"choices", QJsonArray{QJsonObject{
                                   {"delta", QJsonObject{{"content",
                                                          "PARTIAL"}}}}}}});
}
QByteArray signedAnthropic() {
  return sseEvent({{"type", "content_block_start"},
                   {"index", 0},
                   {"content_block", QJsonObject{{"type", "thinking"},
                                                 {"thinking", "THINK"}}}}) +
         sseEvent({{"type", "content_block_delta"},
                   {"index", 0},
                   {"delta", QJsonObject{{"type", "signature_delta"},
                                         {"signature", "SIGNED_FIXTURE"}}}}) +
         sseEvent({{"type", "content_block_stop"}, {"index", 0}}) +
         sseEvent({{"type", "content_block_start"},
                   {"index", 1},
                   {"content_block",
                    QJsonObject{{"type", "text"}, {"text", "DONE"}}}}) +
         sseEvent({{"type", "content_block_stop"}, {"index", 1}}) +
         sseEvent({{"type", "message_delta"},
                   {"delta", QJsonObject{{"stop_reason", "end_turn"}}}}) +
         sseEvent({{"type", "message_stop"}});
}
QByteArray externalCall() {
  const QJsonObject call{
      {"index", 0},
      {"id", "ext-call"},
      {"type", "function"},
      {"function", QJsonObject{{"name", "fixtureWrite"}, {"arguments", "{}"}}}};
  return sseEvent({{"choices",
                    QJsonArray{QJsonObject{
                        {"delta", QJsonObject{{"tool_calls", QJsonArray{call}}}},
                        {"finish_reason", "tool_calls"}}}}}) +
         "data: [DONE]\n\n";
}
QJsonArray externalTools() {
  return {QJsonObject{
      {"type", "function"},
      {"function", QJsonObject{{"name", "fixtureWrite"},
                               {"description", "Fixture write"},
                               {"parameters", QJsonObject{{"type", "object"}}}}}}};
}
QJsonObject mutationRecord() {
  return {{"mutations",
           QJsonArray{QJsonObject{{"agentId", ""},
                                  {"mutationId", "m1"},
                                  {"committed", true},
                                  {"recoveryRequired", false}}}}};
}
// Completes FIRST, then accepts RETRY_ME and fails it with an empty-output
// HTTP 400. Returns the failed user row's display ID.
QString failSecondTurn(Server &server, AgentEngine &engine,
                       const QJsonArray &attachments = {},
                       const QJsonObject &references = {},
                       const QJsonObject &metadata = {}) {
  if (!engine.sendUserMessage("FIRST"))
    return {};
  if (!QTest::qWaitFor([&] { return server.requests.size() == 1; }, 5000))
    return {};
  server.finish(0);
  if (!QTest::qWaitFor([&] { return !engine.isBusy(); }, 5000) ||
      !engine.lastTurnSucceeded())
    return {};
  if (!engine.sendUserMessage("RETRY_ME", attachments, references, metadata))
    return {};
  if (!QTest::qWaitFor([&] { return server.requests.size() == 2; }, 5000))
    return {};
  server.reply(1, "{}", 400);
  if (!QTest::qWaitFor([&] { return !engine.isBusy(); }, 5000))
    return {};
  return lastUserId(engine.messages());
}
} // namespace
class HistoryRetryTest : public QObject {
  Q_OBJECT
private slots:
  // Before: no failure identity existed and nothing could re-dispatch the
  // accepted user row without duplicating it through a new send.
  void retryAfterEmptyOutputFailureSendsAcceptedUserOnce_data() { protocols(); }
  void retryAfterEmptyOutputFailureSendsAcceptedUserOnce() {
    QFETCH(QString, protocol);
    Server server;
    server.protocol = protocol;
    AgentEngine engine;
    engine.setProvider(server.provider());
    const QJsonArray attachments{QJsonObject{{"path", "att/original.txt"},
                                             {"originalFileName", "a.txt"}}};
    const QJsonObject references{{"ref-a", "att/original.txt"}};
    const QJsonObject metadata{
        {"att/original.txt", QJsonObject{{"size", 4}, {"kind", "text"}}}};
    const auto failed =
        failSecondTurn(server, engine, attachments, references, metadata);
    QVERIFY(!failed.isEmpty());
    QCOMPARE(engine.turnFailure()["userRowId"].toString(), failed);
    QVERIFY(AgentEngine::validTurnFailureShape(engine.turnFailure()));
    QVERIFY(engine.turnFailure()["error"].toString().contains("HTTP 400"));
    QCOMPARE(lastUserIndex(engine.messages()), engine.messages().size() - 1);
    QString error;
    QVERIFY2(engine.retryFailedTurn(failed, &error), qPrintable(error));
    QVERIFY(engine.turnFailure().isEmpty());
    const auto history = engine.messages();
    QCOMPARE(userTexts(history), (QStringList{"FIRST", "RETRY_ME"}));
    const auto user = history.last().toObject();
    const auto meta = user["metadata"].toObject();
    QCOMPARE(meta["attachments"].toArray(), attachments);
    QCOMPARE(meta["pathReferences"].toObject(), references);
    QCOMPARE(meta["pathMetadata"].toObject(), metadata);
    QVERIFY(displayId(user) != failed);
    QCOMPARE(projectedLastUserId(engine.displayMessages()), displayId(user));
    QTRY_COMPARE(server.requests.size(), 3);
    const auto body =
        QJsonDocument(server.requests[2]).toJson(QJsonDocument::Compact);
    QCOMPARE(body.count("RETRY_ME"), 1);
    QVERIFY(!body.contains("HTTP 400"));
    QVERIFY(!body.contains("Provider request failed"));
    QCOMPARE(server.requests[2], server.requests[1]);
    server.finish(2);
    QTRY_VERIFY(!engine.isBusy());
    QVERIFY(engine.lastTurnSucceeded());
    QVERIFY(engine.turnFailure().isEmpty());
  }
  // Before: a retained partial assistant row stayed in history, so any new
  // send replayed PARTIAL to the provider.
  void retryRemovesPartialSuffixAndPreservesSignedPrefix_data() { protocols(); }
  void retryRemovesPartialSuffixAndPreservesSignedPrefix() {
    QFETCH(QString, protocol);
    Server server;
    server.protocol = protocol;
    AgentEngine engine;
    engine.setProvider(server.provider());
    QVERIFY(engine.sendUserMessage("FIRST"));
    QTRY_COMPARE(server.requests.size(), 1);
    if (protocol == "anthropic")
      server.reply(0, signedAnthropic());
    else
      server.finish(0);
    QTRY_VERIFY(!engine.isBusy());
    QVERIFY(engine.lastTurnSucceeded());
    QVERIFY(engine.sendUserMessage("RETRY_ME"));
    QTRY_COMPARE(server.requests.size(), 2);
    server.reply(1, partial(protocol));
    QTRY_VERIFY(!engine.isBusy());
    QVERIFY(!engine.lastTurnSucceeded());
    const auto before = engine.messages();
    const int index = lastUserIndex(before);
    QCOMPARE(index, before.size() - 2);
    QCOMPARE(before.last().toObject()["content"].toString(),
             QString("PARTIAL"));
    QString error;
    QVERIFY2(engine.retryFailedTurn(lastUserId(before), &error),
             qPrintable(error));
    const auto after = engine.messages();
    QCOMPARE(after.size(), index + 1);
    for (int i = 0; i < index; ++i)
      QCOMPARE(after[i], before[i]);
    QTRY_COMPARE(server.requests.size(), 3);
    const auto body =
        QJsonDocument(server.requests[2]).toJson(QJsonDocument::Compact);
    QVERIFY(!body.contains("PARTIAL"));
    QCOMPARE(body.count("RETRY_ME"), 1);
    QCOMPARE(server.requests[2], server.requests[1]);
    if (protocol == "anthropic")
      QVERIFY(body.contains("SIGNED_FIXTURE"));
    server.finish(2);
    QTRY_VERIFY(!engine.isBusy());
  }
  // Before: no commit kind existed, so the host could not tell a replacing
  // accepted turn from a running-turn save; the queue survived.
  void retryClearsQueueInSameCommit() {
    Server server;
    AgentEngine engine;
    struct Call {
      QJsonArray history, queue;
      AgentEngine::CommitKind kind;
    };
    QList<Call> calls;
    engine.setQueueCommitter(
        [&](const QJsonArray &history, const QJsonArray &queue, QString *) {
          calls.append({history, queue, engine.commitKind()});
          return true;
        });
    engine.setProvider(server.provider());
    QVERIFY(engine.sendUserMessage("FIRST"));
    QTRY_COMPARE(server.requests.size(), 1);
    server.finish(0);
    QTRY_VERIFY(!engine.isBusy());
    QVERIFY(engine.sendUserMessage("RETRY_ME"));
    QTRY_COMPARE(server.requests.size(), 2);
    QCOMPARE(calls.last().kind, AgentEngine::CommitKind::AcceptUser);
    QVERIFY(engine.sendUserMessage("QUEUED_A"));
    QCOMPARE(engine.queuedMessages().size(), 1);
    QCOMPARE(calls.last().kind, AgentEngine::CommitKind::Update);
    server.reply(1, "{}", 400);
    QTRY_VERIFY(!engine.isBusy());
    QCOMPARE(engine.queuedMessages().size(), 1);
    const auto before = engine.messages();
    calls.clear();
    QSignalSpy queueChanged(&engine, &AgentEngine::queuedMessagesChanged);
    QVERIFY(engine.retryFailedTurn(lastUserId(before)));
    QCOMPARE(calls.size(), 1);
    QCOMPARE(calls[0].kind, AgentEngine::CommitKind::ReplaceAndAcceptUser);
    QVERIFY(calls[0].queue.isEmpty());
    QCOMPARE(calls[0].history, engine.messages());
    QCOMPARE(engine.commitKind(), AgentEngine::CommitKind::Update);
    QVERIFY(engine.queuedMessages().isEmpty());
    QVERIFY(queueChanged.size() >= 1);
    QTRY_COMPARE(server.requests.size(), 3);
    QVERIFY(!QJsonDocument(server.requests[2]).toJson().contains("QUEUED_A"));
    server.finish(2);
    QTRY_VERIFY(!engine.isBusy());
  }
  // Before (design draft): fail() pauses the queue; a direct restart left it
  // paused and the later message was never auto-dispatched.
  void retryUnpausesQueueForLaterMessages() {
    Server server;
    AgentEngine engine;
    engine.setProvider(server.provider());
    const auto failed = failSecondTurn(server, engine);
    QVERIFY(!failed.isEmpty());
    QVERIFY(engine.retryFailedTurn(failed));
    QTRY_COMPARE(server.requests.size(), 3);
    QVERIFY(engine.sendUserMessage("LATER"));
    QCOMPARE(engine.queuedMessages().size(), 1);
    server.finish(2);
    QTRY_COMPARE(server.requests.size(), 4);
    QVERIFY(QJsonDocument(server.requests[3]).toJson().contains("LATER"));
    QVERIFY(engine.queuedMessages().isEmpty());
    server.finish(3);
    QTRY_VERIFY(!engine.isBusy());
  }
  void rejectedCommitLeavesEverythingUnchanged() {
    Server server;
    AgentEngine engine;
    bool reject = false;
    engine.setQueueCommitter(
        [&](const QJsonArray &, const QJsonArray &, QString *error) {
          if (reject) {
            *error = "Synthetic store lock";
            return false;
          }
          return true;
        });
    engine.setProvider(server.provider());
    QVERIFY(engine.sendUserMessage("FIRST"));
    QTRY_COMPARE(server.requests.size(), 1);
    server.finish(0);
    QTRY_VERIFY(!engine.isBusy());
    QVERIFY(engine.sendUserMessage("RETRY_ME"));
    QTRY_COMPARE(server.requests.size(), 2);
    QVERIFY(engine.sendUserMessage("QUEUED_A"));
    server.reply(1, "{}", 400);
    QTRY_VERIFY(!engine.isBusy());
    const auto messages = engine.messages();
    const auto queue = engine.queuedMessages();
    const auto failure = engine.turnFailure();
    QVERIFY(!failure.isEmpty());
    reject = true;
    QSignalSpy errors(&engine, &AgentEngine::queueError);
    QString error;
    QVERIFY(!engine.retryFailedTurn(lastUserId(messages), &error));
    QVERIFY2(error.contains("Synthetic store lock"), qPrintable(error));
    QCOMPARE(errors.size(), 1);
    QCOMPARE(engine.messages(), messages);
    QCOMPARE(engine.queuedMessages(), queue);
    QCOMPARE(engine.turnFailure(), failure);
    QVERIFY(!engine.isBusy());
    QTest::qWait(50);
    QCOMPARE(server.requests.size(), 2);
    // The queue stays paused, exactly as after the failure itself.
    reject = false;
    QVERIFY(engine.sendUserMessage("EXTRA", {}, {}, {}, true));
    QTest::qWait(50);
    QCOMPARE(server.requests.size(), 2);
    QVERIFY(engine.retryFailedTurn(lastUserId(messages)));
    QTRY_COMPARE(server.requests.size(), 3);
    QVERIFY(engine.queuedMessages().isEmpty());
    server.finish(2);
    QTRY_VERIFY(!engine.isBusy());
  }
  void successfulCanceledAndPreacceptanceTurnsAreNotRetryable() {
    Server server;
    AgentEngine engine;
    engine.setProvider(server.provider());
    QVERIFY(engine.sendUserMessage("DONE_TURN"));
    QTRY_COMPARE(server.requests.size(), 1);
    server.finish(0);
    QTRY_VERIFY(!engine.isBusy());
    QVERIFY(engine.turnFailure().isEmpty());
    QVERIFY(!engine.retryFailedTurn(lastUserId(engine.messages())));
    QVERIFY(engine.sendUserMessage("CANCELED_TURN"));
    QTRY_COMPARE(server.requests.size(), 2);
    engine.cancel();
    QVERIFY(engine.turnFailure().isEmpty());
    QVERIFY(!engine.retryFailedTurn(lastUserId(engine.messages())));
    auto invalid = server.provider();
    invalid.model.clear();
    engine.setProvider(invalid);
    const auto history = engine.messages();
    QVERIFY(!engine.sendUserMessage("NEVER_ACCEPTED"));
    QCOMPARE(engine.messages(), history);
    QVERIFY(engine.turnFailure().isEmpty());
  }
  // Design correction: only a busy cancel clears the record.
  void idleCancelKeepsFailure() {
    Server server;
    AgentEngine engine;
    engine.setProvider(server.provider());
    const auto failed = failSecondTurn(server, engine);
    QVERIFY(!failed.isEmpty());
    const auto failure = engine.turnFailure();
    engine.cancel();
    QCOMPARE(engine.turnFailure(), failure);
    auto invalid = server.provider();
    invalid.model.clear();
    engine.setProvider(invalid);
    QVERIFY(!engine.sendUserMessage("PREACCEPTANCE"));
    QCOMPARE(engine.turnFailure(), failure);
    engine.setProvider(server.provider());
    QVERIFY(engine.retryFailedTurn(failed));
    QTRY_COMPARE(server.requests.size(), 3);
    server.finish(2);
    QTRY_VERIFY(!engine.isBusy());
  }
  void staleRowBusyAndPostFailureSendRejected() {
    Server server;
    AgentEngine engine;
    engine.setProvider(server.provider());
    const auto failed = failSecondTurn(server, engine);
    QVERIFY(!failed.isEmpty());
    QString error;
    QVERIFY(!engine.retryFailedTurn("not-the-failed-row", &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(server.requests.size(), 2);
    QVERIFY(engine.sendUserMessage("NEXT"));
    QTRY_COMPARE(server.requests.size(), 3);
    QVERIFY(engine.turnFailure().isEmpty());
    QVERIFY(!engine.retryFailedTurn(failed, &error));
    server.finish(2);
    QTRY_VERIFY(!engine.isBusy());
    QVERIFY(!engine.retryFailedTurn(failed, &error));
    QTest::qWait(50);
    QCOMPARE(server.requests.size(), 3);
    QCOMPARE(userTexts(engine.messages()),
             (QStringList{"FIRST", "RETRY_ME", "NEXT"}));
  }
  // Before: a late settlement for a removed invocation row is silently dropped.
  void unsettledSuffixMutationBlocksRetry_data() {
    QTest::addColumn<bool>("settle");
    QTest::newRow("unsettled") << false;
    QTest::newRow("settled-but-dirty") << true;
  }
  void unsettledSuffixMutationBlocksRetry() {
    QFETCH(bool, settle);
    Server server;
    AgentEngine engine;
    engine.setProvider(server.provider());
    engine.setAlwaysAllow(true);
    engine.setExternalTools(externalTools());
    const auto invocation = QUuid::createUuid().toString(QUuid::WithoutBraces);
    connect(&engine, &AgentEngine::externalToolRequested, &engine,
            [&](const QString &request, const QString &, const QJsonObject &) {
              QVERIFY(engine.registerExternalMutation(invocation, request));
              // Without a committer, a settlement leaves the history dirty.
              if (settle)
                engine.settleExternalMutation(invocation, mutationRecord());
              engine.completeExternalTool(request, "written", false);
            });
    QVERIFY(engine.sendUserMessage("FIRST"));
    QTRY_COMPARE(server.requests.size(), 1);
    server.finish(0);
    QTRY_VERIFY(!engine.isBusy());
    QVERIFY(engine.sendUserMessage("RETRY_ME"));
    QTRY_COMPARE(server.requests.size(), 2);
    server.reply(1, externalCall());
    QTRY_COMPARE(server.requests.size(), 3);
    server.reply(2, "{}", 400);
    QTRY_VERIFY(!engine.isBusy());
    const auto failed = engine.turnFailure()["userRowId"].toString();
    QVERIFY(!failed.isEmpty());
    const auto history = engine.messages();
    QString error;
    QVERIFY(!engine.retryFailedTurn(failed, &error));
    QVERIFY2(error.contains("still being recorded"), qPrintable(error));
    QCOMPARE(engine.messages(), history);
    QTest::qWait(50);
    QCOMPARE(server.requests.size(), 3);
  }
  // Before: a settlement the journal committed after the last reconcile was
  // never applied while idle, so Retry reported "still being recorded" forever.
  void journalSettledSuffixMutationIsReconciledBeforeRetry() {
    Server server;
    AgentEngine engine;
    engine.setQueueCommitter(
        [](const QJsonArray &, const QJsonArray &, QString *) { return true; });
    const auto invocation = QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool journalSettled = false;
    QList<QStringList> recoveryRequests;
    engine.setMutationContext(
        "agent-1", [&](const QStringList &ids, QString *) {
          recoveryRequests.append(ids);
          if (!journalSettled)
            return QJsonArray{};
          return QJsonArray{QJsonObject{{"invocationId", invocation},
                                        {"agentId", "agent-1"},
                                        {"mutationId", "m1"},
                                        {"committed", true},
                                        {"recoveryRequired", false},
                                        {"output", QJsonObject{}}}};
        });
    engine.setProvider(server.provider());
    engine.setAlwaysAllow(true);
    engine.setExternalTools(externalTools());
    connect(&engine, &AgentEngine::externalToolRequested, &engine,
            [&](const QString &request, const QString &, const QJsonObject &) {
              QVERIFY(engine.registerExternalMutation(invocation, request));
              engine.completeExternalTool(request, "written", false);
            });
    QVERIFY(engine.sendUserMessage("FIRST"));
    QTRY_COMPARE(server.requests.size(), 1);
    server.finish(0);
    QTRY_VERIFY(!engine.isBusy());
    QVERIFY(engine.sendUserMessage("RETRY_ME"));
    QTRY_COMPARE(server.requests.size(), 2);
    server.reply(1, externalCall());
    QTRY_COMPARE(server.requests.size(), 3);
    server.reply(2, "{}", 400);
    QTRY_VERIFY(!engine.isBusy());
    const auto failed = engine.turnFailure()["userRowId"].toString();
    QVERIFY(!failed.isEmpty());
    // The journal has no settlement yet: Retry reconciles, finds nothing and
    // still refuses without changing history or dispatching.
    recoveryRequests.clear();
    const auto history = engine.messages();
    QString error;
    QVERIFY(!engine.retryFailedTurn(failed, &error));
    QVERIFY2(error.contains("still being recorded"), qPrintable(error));
    QCOMPARE(recoveryRequests.size(), 1);
    QCOMPARE(recoveryRequests[0], QStringList{invocation});
    QCOMPARE(engine.messages(), history);
    // The journal now holds the committed settlement the history lacks.
    journalSettled = true;
    recoveryRequests.clear();
    QVERIFY2(engine.retryFailedTurn(failed, &error), qPrintable(error));
    QCOMPARE(recoveryRequests.size(), 1);
    QCOMPARE(recoveryRequests[0], QStringList{invocation});
    QTRY_COMPARE(server.requests.size(), 4);
    QVERIFY(!QJsonDocument(engine.messages()).toJson().contains(invocation.toUtf8()));
    server.finish(3);
    QTRY_VERIFY(!engine.isBusy());
  }
  void dirtyPrefixOnlyDoesNotBlock() {
    Server server;
    AgentEngine engine;
    engine.setProvider(server.provider());
    engine.setAlwaysAllow(true);
    engine.setExternalTools(externalTools());
    const auto invocation = QUuid::createUuid().toString(QUuid::WithoutBraces);
    connect(&engine, &AgentEngine::externalToolRequested, &engine,
            [&](const QString &request, const QString &, const QJsonObject &) {
              QVERIFY(engine.registerExternalMutation(invocation, request));
              engine.settleExternalMutation(invocation, mutationRecord());
              engine.completeExternalTool(request, "written", false);
            });
    QVERIFY(engine.sendUserMessage("FIRST"));
    QTRY_COMPARE(server.requests.size(), 1);
    server.reply(0, externalCall());
    QTRY_COMPARE(server.requests.size(), 2);
    server.finish(1);
    QTRY_VERIFY(!engine.isBusy());
    QVERIFY(engine.lastTurnSucceeded());
    QVERIFY(engine.sendUserMessage("RETRY_ME"));
    QTRY_COMPARE(server.requests.size(), 3);
    server.reply(2, "{}", 400);
    QTRY_VERIFY(!engine.isBusy());
    QString error;
    QVERIFY2(engine.retryFailedTurn(engine.turnFailure()["userRowId"].toString(),
                                    &error),
             qPrintable(error));
    QTRY_COMPARE(server.requests.size(), 4);
    server.finish(3);
    QTRY_VERIFY(!engine.isBusy());
  }
  void restoreTurnFailureValidatesShapeAndIdentity() {
    Server server;
    AgentEngine source;
    source.setProvider(server.provider());
    const auto failed = failSecondTurn(server, source);
    QVERIFY(!failed.isEmpty());
    const auto history = source.messages();
    const auto record = source.turnFailure();
    AgentEngine restored;
    restored.setProvider(server.provider());
    restored.setMessages(history);
    QVERIFY(restored.restoreTurnFailure(record));
    QCOMPARE(restored.turnFailure(), record);
    QVERIFY(restored.restoreTurnFailure({}));
    QVERIFY(restored.turnFailure().isEmpty());
    auto wrong = record;
    wrong["userRowId"] = "another-row";
    QVERIFY(!restored.restoreTurnFailure(wrong));
    QVERIFY(restored.turnFailure().isEmpty());
    auto later = history;
    later.append(QueuedMessages::create("LATER", {}, {}, {}));
    QVERIFY(!AgentEngine::validTurnFailure(later, record));
    auto mismatched = history;
    auto user = mismatched.last().toObject();
    auto metadata = user["metadata"].toObject();
    auto display = metadata["display"].toObject();
    auto parts = display["parts"].toArray();
    auto part = parts[0].toObject();
    part["text"] = "NOT_THE_CONTENT";
    parts[0] = part;
    display["parts"] = parts;
    metadata["display"] = display;
    user["metadata"] = metadata;
    mismatched[mismatched.size() - 1] = user;
    QVERIFY(!AgentEngine::validTurnFailure(mismatched, record));
    QVERIFY(AgentEngine::validTurnFailure(history, record));
    auto extra = record;
    extra["note"] = "x";
    auto version = record;
    version["version"] = 2;
    auto negative = record;
    negative["failedAt"] = -1;
    auto fractional = record;
    fractional["failedAt"] = 1.5;
    auto ascii = record;
    ascii["error"] = QString(2001, 'x');
    auto multibyte = record;
    multibyte["error"] = QString(2000, QChar(0x00e9));
    auto control = record;
    control["error"] = QString(1000, QChar(0x0001));
    auto badId = record;
    badId["userRowId"] = "bad id";
    for (const auto &invalid : {extra, version, negative, fractional, ascii,
                                multibyte, control, badId}) {
      QVERIFY(!AgentEngine::validTurnFailureShape(invalid));
      QVERIFY(!restored.restoreTurnFailure(invalid));
    }
    // Store loader and writer share the predicate.
    QTemporaryDir data;
    Conversation chat;
    chat.id = "chat";
    chat.title = "Chat";
    chat.messages = history;
    chat.turnFailure = multibyte;
    QString error;
    QVERIFY(!SessionStore(data.path()).save({chat}, &error));
    chat.turnFailure = {};
    QVERIFY(SessionStore(data.path()).save({chat}, &error));
    QFile file(data.filePath("sessions.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(!file.readAll().contains("turnFailure"));
    file.close();
    chat.turnFailure = record;
    SessionStore store(data.path());
    store.load();
    QVERIFY2(store.save({chat}, &error), qPrintable(error));
    const auto loaded = SessionStore(data.path()).load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded[0].turnFailure, record);
    for (const auto &bad : {QJsonValue("text"), QJsonValue(QJsonObject{}),
                            QJsonValue(extra)}) {
      QVERIFY(file.open(QIODevice::ReadOnly));
      auto document = QJsonDocument::fromJson(file.readAll()).object();
      file.close();
      auto sessions = document["sessions"].toArray();
      auto saved = sessions[0].toObject();
      saved["turnFailure"] = bad;
      sessions[0] = saved;
      document["sessions"] = sessions;
      QTemporaryDir copy;
      QFile target(copy.filePath("sessions.json"));
      QVERIFY(target.open(QIODevice::WriteOnly));
      target.write(QJsonDocument(document).toJson());
      target.close();
      QString loadError;
      QVERIFY(SessionStore(copy.path()).load(&loadError).isEmpty());
      QVERIFY2(loadError.contains("Original data was preserved"),
               qPrintable(loadError));
    }
  }
  // Before: fail() bounded the error to 2000 characters only, which can exceed
  // the store's byte bound and make the next load refuse every chat.
  void failureRecordFitsShapeForMultibyteError() {
    Server server;
    AgentEngine engine;
    engine.setProvider(server.provider());
    QVERIFY(engine.sendUserMessage("FIRST"));
    QTRY_COMPARE(server.requests.size(), 1);
    QString message;
    for (int i = 0; i < 2000; ++i)
      message.append(i % 2 ? QChar(0x0001) : QChar(0x00e9));
    server.reply(0, sseEvent({{"error", QJsonObject{{"message", message}}}}));
    QTRY_VERIFY(!engine.isBusy());
    const auto record = engine.turnFailure();
    QVERIFY(AgentEngine::validTurnFailureShape(record));
    QCOMPARE(record["userRowId"].toString(), lastUserId(engine.messages()));
    QVERIFY(!record["error"].toString().isEmpty());
    QVERIFY(QJsonDocument(record).toJson(QJsonDocument::Compact).size() <=
            TurnFailure::MaximumCompactBytes);
    const auto unpaired = [](const QString &text) {
      for (int i = 0; i < text.size(); ++i) {
        if (text.at(i).isHighSurrogate()) {
          if (i + 1 >= text.size() || !text.at(i + 1).isLowSurrogate())
            return true;
          ++i;
        } else if (text.at(i).isLowSurrogate())
          return true;
      }
      return false;
    };
    const auto emoji = QString::fromUtf8("\xF0\x9F\x8C\x8D");
    // 2000 characters whose escaped control tail exceeds the byte bound, so
    // make() must shrink. The first cut (2000 * 3 / 4 = 1500) falls between
    // the halves of the pair at 1499..1500; the remaining prefix then fits.
    QString pair = "a";
    for (int i = 0; i < 750; ++i)
      pair.append(emoji);
    while (pair.size() < TurnFailure::MaximumErrorCharacters)
      pair.append(QChar(0x0001));
    QCOMPARE(pair.size(), TurnFailure::MaximumErrorCharacters);
    QVERIFY(pair.at(1499).isHighSurrogate());
    QVERIFY(!TurnFailure::validShape({{"version", 1},
                                      {"userRowId", "row-1"},
                                      {"error", pair},
                                      {"failedAt", 5.0}}));
    const auto shrunk = TurnFailure::make("row-1", pair, 5);
    QVERIFY(TurnFailure::validShape(shrunk));
    const auto text = shrunk["error"].toString();
    QVERIFY(text.size() < pair.size());
    QCOMPARE(text, pair.left(1499));
    QVERIFY(!unpaired(text));
    // The initial character bound must not split a pair either.
    QString bounded(TurnFailure::MaximumErrorCharacters - 1, QChar('b'));
    bounded.append(emoji);
    const auto cut = TurnFailure::make("row-1", bounded, 5);
    QVERIFY(TurnFailure::validShape(cut));
    QCOMPARE(cut["error"].toString(), bounded.left(bounded.size() - 2));
    QVERIFY(!unpaired(cut["error"].toString()));
    QTemporaryDir data;
    Conversation chat;
    chat.id = "chat";
    chat.title = "Chat";
    chat.messages = engine.messages();
    chat.turnFailure = record;
    QString error;
    QVERIFY2(SessionStore(data.path()).save({chat}, &error), qPrintable(error));
    const auto loaded = SessionStore(data.path()).load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.value(0).turnFailure, record);
  }
  void ownerDeletedDuringRetryPublication() {
    Server server;
    auto *engine = new AgentEngine;
    QPointer<AgentEngine> guard(engine);
    engine->setProvider(server.provider());
    const auto failed = failSecondTurn(server, *engine);
    QVERIFY(!failed.isEmpty());
    connect(engine, &AgentEngine::turnAboutToStart, this,
            [engine](const QJsonObject &) { delete engine; });
    QVERIFY(engine->retryFailedTurn(failed));
    QVERIFY(!guard);
    QTest::qWait(50);
    QCOMPARE(server.requests.size(), 2);
  }
  void duplicateRetryIsIdempotent() {
    Server server;
    AgentEngine engine;
    engine.setProvider(server.provider());
    const auto failed = failSecondTurn(server, engine);
    QVERIFY(!failed.isEmpty());
    QVERIFY(engine.retryFailedTurn(failed));
    QString error;
    QVERIFY(!engine.retryFailedTurn(failed, &error));
    QTRY_COMPARE(server.requests.size(), 3);
    server.finish(2);
    QTRY_VERIFY(!engine.isBusy());
    QVERIFY(!engine.retryFailedTurn(failed, &error));
    QTest::qWait(50);
    QCOMPARE(server.requests.size(), 3);
    QCOMPARE(userTexts(engine.messages()),
             (QStringList{"FIRST", "RETRY_ME"}));
  }
};
QTEST_GUILESS_MAIN(HistoryRetryTest)
#include "test_history_retry.moc"
