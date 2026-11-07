#include "core/AgentEngine.h"
#include "core/HarnessHistory.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
ProviderConfig provider(const QString &kind) {
  ProviderConfig p;
  p.id = kind + "-fixture";
  p.name = kind;
  p.kind = kind;
  p.model = kind == "claude-code" ? "default" : "model-a";
  p.protocol = kind == "codex"      ? "codex-app-server"
               : kind == "opencode" ? "opencode-acp"
                                    : "claude-control";
  const auto exe = kind == "codex"      ? "harness_fixture.exe"
                   : kind == "opencode" ? "acp_fixture.exe"
                                        : "claude_fixture.exe";
  p.harnessExecutable =
      QDir(QCoreApplication::applicationDirPath()).filePath(exe);
  return p;
}
void kinds() {
  QTest::addColumn<QString>("kind");
  for (const auto &kind : {"codex", "opencode", "claude-code"})
    QTest::newRow(kind) << QString(kind);
}
void logging(const QString &directory) {
  qputenv("ADE_HARNESS_FIXTURE_REQUESTS",
          (directory + "/codex.jsonl").toUtf8());
  qputenv("ADE_ACP_LOG", (directory + "/opencode.jsonl").toUtf8());
  qputenv("ADE_CLAUDE_LOG", (directory + "/claude-code.jsonl").toUtf8());
}
QJsonArray prompts(const QString &directory, const QString &kind) {
  QFile file(directory + "/" + kind + ".jsonl");
  QJsonArray out;
  if (!file.open(QIODevice::ReadOnly))
    return out;
  for (const auto &line : file.readAll().split('\n')) {
    const auto row = QJsonDocument::fromJson(line).object();
    if ((kind == "codex" && row.value("method") == "turn/start") ||
        (kind == "opencode" && row.value("method") == "session/prompt"))
      out.append(row.value("params"));
    if (kind == "claude-code" && row.value("type") == "user")
      out.append(row);
  }
  return out;
}
QByteArray lastWire(const QString &directory, const QString &kind) {
  const auto sent = prompts(directory, kind);
  return sent.isEmpty() ? QByteArray{}
                        : QJsonDocument(sent.last().toObject())
                              .toJson(QJsonDocument::Compact);
}
bool turn(AgentEngine &engine, const QString &text) {
  QSignalSpy done(&engine, &AgentEngine::finished);
  if (!engine.submitWithAttachments(text, {}))
    return false;
  if (done.isEmpty() && !done.wait(8000))
    return false;
  return engine.lastTurnSucceeded();
}
void configure(AgentEngine &engine, const QString &kind,
               const QString &workspace, const QString &owner = "owner") {
  engine.setProvider(provider(kind));
  engine.setHarnessContext(owner, workspace);
}
QJsonObject mapping(const AgentEngine &engine) {
  return engine.messages()
      .last()
      .toObject()
      .value("metadata")
      .toObject()
      .value("harness")
      .toObject();
}
class LocalProvider final : public QTcpServer {
public:
  int requests = 0;
  LocalProvider() {
    connect(this, &QTcpServer::newConnection, this, [this] {
      while (hasPendingConnections()) {
        auto *socket = nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, socket,
                [this, socket, bytes = QByteArray{}]() mutable {
                  bytes += socket->readAll();
                  const auto split = bytes.indexOf("\r\n\r\n");
                  if (split < 0)
                    return;
                  qint64 length = 0;
                  for (const auto &line : bytes.left(split).split('\n'))
                    if (line.toLower().startsWith("content-length:"))
                      length = line.mid(15).trimmed().toLongLong();
                  if (bytes.size() < split + 4 + length)
                    return;
                  if (socket->property("answered").toBool())
                    return;
                  socket->setProperty("answered", true);
                  ++requests;
                  const QByteArray body =
                      "data: "
                      "{\"choices\":[{\"delta\":{\"content\":\"HTTP_"
                      "INTERVENING_REPLY\"}}]}\n\ndata: [DONE]\n\n";
                  socket->write("HTTP/1.1 200 OK\r\nContent-Type: "
                                "text/event-stream\r\nContent-Length: " +
                                QByteArray::number(body.size()) +
                                "\r\nConnection: close\r\n\r\n" + body);
                  socket->disconnectFromHost();
                });
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
      }
    });
    listen(QHostAddress::LocalHost);
  }
  ProviderConfig config() const {
    ProviderConfig p;
    p.id = "http-fixture";
    p.name = p.id;
    p.protocol = "openai";
    p.model = "fixture";
    p.baseUrl = QString("http://127.0.0.1:%1/v1").arg(serverPort());
    return p;
  }
};
} // namespace
class HarnessContinuityTest final : public QObject {
  Q_OBJECT
private slots:
  void cleanup() {
    for (const auto *name : {"ADE_HARNESS_FIXTURE_REQUESTS", "ADE_ACP_LOG",
                             "ADE_CLAUDE_LOG", "ADE_HARNESS_FIXTURE_SCENARIO",
                             "ADE_ACP_SCENARIO", "ADE_CLAUDE_FIXTURE"})
      qunsetenv(name);
  }
  void httpSwitchRebuildsCanonicalPrefix_data() { kinds(); }
  void httpSwitchRebuildsCanonicalPrefix() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    logging(dir.path());
    AgentEngine e;
    configure(e, kind, dir.path());
    QVERIFY(turn(e, "FIRST_HARNESS_USER"));
    LocalProvider http;
    QVERIFY(http.isListening());
    e.setProvider(http.config());
    QVERIFY(turn(e, "HTTP_INTERVENING_USER"));
    QCOMPARE(http.requests, 1);
    e.setProvider(provider(kind));
    QVERIFY(turn(e, "RETURN_CURRENT"));
    const auto wire = lastWire(dir.path(), kind);
    QVERIFY2(wire.contains("HTTP_INTERVENING_USER"), wire.constData());
    QVERIFY(wire.contains("HTTP_INTERVENING_REPLY"));
    QCOMPARE(wire.count("FIRST_HARNESS_USER"), 1);
    QCOMPARE(wire.count("RETURN_CURRENT"), 1);
  }
  void otherHarnessRebuildsCanonicalPrefix_data() { kinds(); }
  void otherHarnessRebuildsCanonicalPrefix() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    logging(dir.path());
    AgentEngine e;
    configure(e, kind, dir.path());
    QVERIFY(turn(e, "FIRST_HARNESS_USER"));
    const auto other = kind == "codex" ? QString("opencode") : QString("codex");
    e.setProvider(provider(other));
    QVERIFY(turn(e, "OTHER_HARNESS_USER"));
    e.setProvider(provider(kind));
    QVERIFY(turn(e, "RETURN_CURRENT"));
    const auto wire = lastWire(dir.path(), kind);
    QCOMPARE(wire.count("OTHER_HARNESS_USER"), 1);
    QCOMPARE(wire.count("FIRST_HARNESS_USER"), 1);
  }
  void unchangedLiveRestartAndFork_data() { kinds(); }
  void unchangedLiveRestartAndFork() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    logging(dir.path());
    AgentEngine e;
    configure(e, kind, dir.path());
    QVERIFY(turn(e, "FIRST_USER"));
    QVERIFY(turn(e, "SECOND_USER"));
    QVERIFY(!lastWire(dir.path(), kind).contains("conversation-history"));
    const auto history = e.messages();
    AgentEngine restored;
    configure(restored, kind, dir.path());
    restored.setMessages(history);
    QVERIFY(turn(restored, "RESTART_USER"));
    QVERIFY(!lastWire(dir.path(), kind).contains("conversation-history"));
    AgentEngine fork;
    configure(fork, kind, dir.path(), "child");
    fork.setMessages(history);
    QVERIFY(turn(fork, "FORK_USER"));
    QCOMPARE(lastWire(dir.path(), kind).contains("conversation-history"),
             kind == "opencode");
    QCOMPARE(mapping(fork).value("ownerId").toString(), QString("child"));
    QCOMPARE(e.messages(), history);
  }
  void destructiveReplacementCannotResumeAdvancedSession_data() { kinds(); }
  void destructiveReplacementCannotResumeAdvancedSession() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    logging(dir.path());
    AgentEngine e;
    configure(e, kind, dir.path());
    QVERIFY(turn(e, "KEPT_FIRST"));
    const auto checkpoint = e.messages();
    QVERIFY(turn(e, "REMOVED_SECOND"));
    e.setMessages(checkpoint);
    QVERIFY(turn(e, "AFTER_REVERT"));
    const auto wire = lastWire(dir.path(), kind);
    QVERIFY(wire.contains("conversation-history"));
    QCOMPARE(wire.count("KEPT_FIRST"), 1);
    QVERIFY(!wire.contains("REMOVED_SECOND"));
    QCOMPARE(checkpoint.size(), 2);
  }
  void metadataOnlyKeepsCanonicalContinuity_data() { kinds(); }
  void metadataOnlyKeepsCanonicalContinuity() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    logging(dir.path());
    AgentEngine e;
    configure(e, kind, dir.path());
    QVERIFY(turn(e, "KEPT_USER"));
    auto history = e.messages();
    auto row = history.last().toObject();
    auto metadata = row.value("metadata").toObject();
    auto display = metadata.value("display").toObject();
    display["reviewNote"] = "UI_DO_NOT_REPLAY";
    metadata["display"] = display;
    metadata["toolPresentationReview"] = "PRESENTATION_DO_NOT_REPLAY";
    row["metadata"] = metadata;
    row["ade_native"] =
        QJsonObject{{"protocol", "opaque-fixture"},
                    {"content", QJsonArray{"SIGNED_DO_NOT_REPLAY"}}};
    history[history.size() - 1] = row;
    e.setMessages(history);
    QVERIFY(turn(e, "AFTER_DISPLAY_EDIT"));
    const auto wire = lastWire(dir.path(), kind);
    QVERIFY(!wire.contains("conversation-history"));
    QVERIFY(!wire.contains("UI_DO_NOT_REPLAY"));
    QVERIFY(!wire.contains("SIGNED_DO_NOT_REPLAY"));
    QVERIFY(!wire.contains("PRESENTATION_DO_NOT_REPLAY"));
  }
  void legacyMappingRebuildsOnlyOnce_data() { kinds(); }
  void legacyMappingRebuildsOnlyOnce() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    logging(dir.path());
    AgentEngine e;
    configure(e, kind, dir.path());
    QVERIFY(turn(e, "LEGACY_USER"));
    auto history = e.messages();
    for (int i = 0; i < history.size(); ++i) {
      auto row = history[i].toObject();
      auto metadata = row.value("metadata").toObject();
      auto state = metadata.value("harness").toObject();
      state.remove("continuity");
      if (!state.isEmpty()) {
        metadata["harness"] = state;
        row["metadata"] = metadata;
        history[i] = row;
      }
    }
    AgentEngine restored;
    configure(restored, kind, dir.path());
    restored.setMessages(history);
    QVERIFY(turn(restored, "AFTER_LEGACY"));
    QVERIFY(lastWire(dir.path(), kind).contains("LEGACY_USER"));
    QVERIFY(turn(restored, "NEXT_NEW_TURN"));
    QVERIFY(!lastWire(dir.path(), kind).contains("conversation-history"));
  }
  void canceledPartialDoesNotCertifyResume_data() { kinds(); }
  void canceledPartialDoesNotCertifyResume() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    logging(dir.path());
    AgentEngine e;
    configure(e, kind, dir.path());
    bool canceled = false;
    const auto connection = connect(&e, &AgentEngine::textDelta, &e, [&] {
      if (!canceled) {
        canceled = true;
        e.cancel();
      }
    });
    QSignalSpy done(&e, &AgentEngine::finished);
    QVERIFY(e.submitWithAttachments("INTERRUPTED_USER", {}));
    QTRY_VERIFY_WITH_TIMEOUT(!e.isBusy(), 8000);
    QVERIFY(canceled);
    QVERIFY(!e.lastTurnSucceeded());
    disconnect(connection);
    QVERIFY(turn(e, "RETRY_NEW_USER"));
    const auto wire = lastWire(dir.path(), kind);
    QCOMPARE(wire.count("INTERRUPTED_USER"), 1);
    QCOMPARE(wire.count("RETRY_NEW_USER"), 1);
    QVERIFY(wire.contains("conversation-history"));
  }
  void compressedReplacementReplaysOnlySummary_data() { kinds(); }
  void compressedReplacementReplaysOnlySummary() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    logging(dir.path());
    AgentEngine e;
    configure(e, kind, dir.path());
    QVERIFY(turn(e, "OLD_BEFORE_SUMMARY"));
    QVERIFY(turn(e, "KEPT_AFTER_SUMMARY"));
    auto history = e.messages();
    auto boundary = history[2].toObject();
    boundary["ade_compressedHistory"] = "SUMMARY_CANONICAL_MARKER";
    history[2] = boundary;
    auto last = history.last().toObject();
    last["ade_native"] =
        QJsonObject{{"protocol", "opaque-fixture"},
                    {"content", QJsonArray{"NATIVE_SIGNATURE_SECRET"}}};
    last["metadata"] = QJsonObject{{"notForModel", "PRIVATE_DISPLAY_SECRET"}};
    history[history.size() - 1] = last;
    e.setMessages(history);
    QVERIFY(turn(e, "AFTER_COMPRESSION"));
    const auto wire = lastWire(dir.path(), kind);
    QCOMPARE(wire.count("SUMMARY_CANONICAL_MARKER"), 1);
    QCOMPARE(wire.count("KEPT_AFTER_SUMMARY"), 1);
    QVERIFY(!wire.contains("OLD_BEFORE_SUMMARY"));
    QVERIFY(!wire.contains("NATIVE_SIGNATURE_SECRET"));
    QVERIFY(!wire.contains("PRIVATE_DISPLAY_SECRET"));
  }
  void rejectedBindingCannotBorrowAnotherOwner_data() { kinds(); }
  void rejectedBindingCannotBorrowAnotherOwner() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    logging(dir.path());
    AgentEngine original;
    configure(original, kind, dir.path());
    QVERIFY(turn(original, "OWNED_USER"));
    auto history = original.messages();
    auto row = history.last().toObject();
    auto metadata = row.value("metadata").toObject();
    auto state = metadata.value("harness").toObject();
    QVERIFY(!state.value("continuity").toObject().isEmpty());
    state["ownerId"] = "different-unverified-owner";
    metadata["harness"] = state;
    row["metadata"] = metadata;
    history[history.size() - 1] = row;
    AgentEngine restored;
    configure(restored, kind, dir.path());
    restored.setMessages(history);
    QVERIFY(turn(restored, "AFTER_BAD_BINDING"));
    QCOMPARE(lastWire(dir.path(), kind).count("OWNED_USER"), 1);
    QCOMPARE(mapping(restored).value("ownerId").toString(), QString("owner"));
  }
  void freshReplayExcludesToolNativeAndUiPayload_data() { kinds(); }
  void freshReplayExcludesToolNativeAndUiPayload() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    logging(dir.path());
    AgentEngine e;
    configure(e, kind, dir.path());
    const QJsonArray history{
        QJsonObject{{"role", "user"}, {"content", "VISIBLE_USER"}},
        QJsonObject{
            {"role", "assistant"},
            {"content", "VISIBLE_ASSISTANT"},
            {"tool_calls",
             QJsonArray{
                 QJsonObject{{"id", "prior-call"},
                             {"type", "function"},
                             {"function", QJsonObject{{"name", "read"},
                                                      {"arguments", "{}"}}}}}},
            {"ade_native",
             QJsonObject{{"protocol", "opaque-fixture"},
                         {"content", QJsonArray{"SIGNED_PAYLOAD_EXCLUDED"}}}},
            {"metadata",
             QJsonObject{{"presentationOnly", "UI_PAYLOAD_EXCLUDED"}}}},
        QJsonObject{{"role", "tool"},
                    {"tool_call_id", "prior-call"},
                    {"content", "TOOL_RESULT_EXCLUDED"}}};
    e.setMessages(history);
    QVERIFY(turn(e, "CURRENT_VISIBLE"));
    const auto wire = lastWire(dir.path(), kind);
    QCOMPARE(wire.count("VISIBLE_USER"), 1);
    QCOMPARE(wire.count("VISIBLE_ASSISTANT"), 1);
    QVERIFY(!wire.contains("SIGNED_PAYLOAD_EXCLUDED"));
    QVERIFY(!wire.contains("UI_PAYLOAD_EXCLUDED"));
    QVERIFY(!wire.contains("TOOL_RESULT_EXCLUDED"));
    QVERIFY(!wire.contains("continuity"));
    QVERIFY(!wire.contains("workspaceIdentity"));
  }
  void canonicalDigestBindsLogicalInputsButNotUi() {
    QJsonObject user{
        {"role", "user"},
        {"content", "same text"},
        {"metadata",
         QJsonObject{
             {"attachments", QJsonArray{QJsonObject{{"path", "att/a.bin"},
                                                    {"hash", "original"}}}},
             {"pathReferences", QJsonObject{{"att/a.bin", "same-name"}}}}}};
    const auto original = HarnessHistory::canonicalDigest({user});
    auto metadata = user.value("metadata").toObject();
    metadata["display"] = QJsonObject{{"theme", "dark"}};
    user["metadata"] = metadata;
    QCOMPARE(HarnessHistory::canonicalDigest({user}), original);
    metadata["attachments"] =
        QJsonArray{QJsonObject{{"path", "att/b.bin"}, {"hash", "different"}}};
    user["metadata"] = metadata;
    QVERIFY(HarnessHistory::canonicalDigest({user}) != original);
  }
};
QTEST_GUILESS_MAIN(HarnessContinuityTest)
#include "test_harness_continuity.moc"
