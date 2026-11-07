#include "core/AgentEngine.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
// Failed-turn Retry for external coding agents (agent-history slice 1). A
// failed harness turn binds nothing, so the prefix still proves the earlier
// session resumable; Retry must strip bindings and start a fresh session with
// the canonical transcript. The API under test does not exist before the change.
using namespace ade;
namespace {
ProviderConfig provider(const QString &kind) {
  ProviderConfig p;
  p.id = kind + "-fixture";
  p.name = kind;
  p.kind = kind;
  p.model = kind == "claude-code" ? "default"
            : kind == "cursor"    ? "model-b"
                                  : "model-a";
  p.protocol = kind == "codex"      ? "codex-app-server"
               : kind == "opencode" ? "opencode-acp"
               : kind == "cursor"   ? "cursor-acp"
                                    : "claude-control";
  if (kind == "cursor")
    p.templateId = "cursor";
  const auto exe = kind == "codex"      ? "harness_fixture.exe"
                   : kind == "opencode" ? "acp_fixture.exe"
                   : kind == "cursor"   ? "cursor_fixture.exe"
                                        : "claude_fixture.exe";
  p.harnessExecutable =
      QDir(QCoreApplication::applicationDirPath()).filePath(exe);
  return p;
}
void kinds() {
  QTest::addColumn<QString>("kind");
  for (const auto *kind : {"codex", "opencode", "claude-code", "cursor"})
    QTest::newRow(kind) << QString(kind);
}
void logging(const QString &directory) {
  qputenv("ADE_HARNESS_FIXTURE_REQUESTS",
          (directory + "/codex.jsonl").toUtf8());
  qputenv("ADE_ACP_LOG", (directory + "/opencode.jsonl").toUtf8());
  qputenv("ADE_CLAUDE_LOG", (directory + "/claude-code.jsonl").toUtf8());
  qputenv("ADE_CLAUDE_ARGS_LOG", (directory + "/claude-args.jsonl").toUtf8());
  qputenv("ADE_CURSOR_LOG", (directory + "/cursor.jsonl").toUtf8());
}
// fail() kills the external process; the retried turn starts a new process
// that rereads its scenario, so the scenario is set only for the failing turn.
void scenario(const QString &kind, bool failing) {
  const char *name = kind == "codex"      ? "ADE_HARNESS_FIXTURE_SCENARIO"
                     : kind == "opencode" ? "ADE_ACP_SCENARIO"
                     : kind == "cursor"   ? "ADE_CURSOR_SCENARIO"
                                          : "ADE_CLAUDE_FIXTURE";
  const QByteArray value = kind == "codex"      ? "thread-error"
                           : kind == "opencode" ? "session-error"
                           : kind == "cursor"   ? "missing-model"
                                                : "error";
  if (failing)
    qputenv(name, value);
  else
    qunsetenv(name);
}
QList<QJsonObject> rows(const QString &path) {
  QFile file(path);
  QList<QJsonObject> out;
  if (!file.open(QIODevice::ReadOnly))
    return out;
  for (const auto &line : file.readAll().split('\n'))
    if (!line.trimmed().isEmpty())
      out.append(QJsonDocument::fromJson(line).object());
  return out;
}
QStringList methods(const QString &directory, const QString &kind) {
  QStringList out;
  for (const auto &row : rows(directory + "/" + kind + ".jsonl"))
    if (row.value("method").isString())
      out.append(row.value("method").toString());
  return out;
}
QStringList claudeArguments(const QString &directory) {
  QFile file(directory + "/claude-args.jsonl");
  QStringList out;
  if (file.open(QIODevice::ReadOnly))
    for (const auto &line : file.readAll().split('\n'))
      if (!line.trimmed().isEmpty())
        out.append(QString::fromUtf8(line));
  return out;
}
QByteArray lastWire(const QString &directory, const QString &kind) {
  QJsonObject last;
  for (const auto &row : rows(directory + "/" + kind + ".jsonl")) {
    if ((kind == "codex" && row.value("method") == "turn/start") ||
        ((kind == "opencode" || kind == "cursor") &&
         row.value("method") == "session/prompt"))
      last = row.value("params").toObject();
    if (kind == "claude-code" && row.value("type") == "user")
      last = row;
  }
  return QJsonDocument(last).toJson(QJsonDocument::Compact);
}
bool hasContinuity(const QJsonArray &history) {
  for (const auto &value : history)
    if (value.toObject()["metadata"].toObject()["harness"].toObject().contains(
            "continuity"))
      return true;
  return false;
}
void configure(AgentEngine &engine, const QString &kind,
               const QString &workspace) {
  engine.setProvider(provider(kind));
  engine.setHarnessContext("owner", workspace);
  if (kind == "cursor")
    engine.setWorkspace(workspace);
  // Cursor asks once per process to use the signed-in CLI account.
  QObject::connect(&engine, &AgentEngine::approvalRequired, &engine,
                   [&engine](const QString &id, const QString &name,
                             const QJsonObject &) {
                     if (name == "Connect Cursor account")
                       engine.approveTool(id, true);
                   });
}
bool turn(AgentEngine &engine, const QString &text) {
  QSignalSpy done(&engine, &AgentEngine::finished);
  if (!engine.submitWithAttachments(text, {}))
    return false;
  if (done.isEmpty() && !done.wait(8000))
    return false;
  return engine.lastTurnSucceeded();
}
} // namespace
class HistoryRetryHarnessTest final : public QObject {
  Q_OBJECT
private slots:
  void cleanup() {
    for (const auto *name :
         {"ADE_HARNESS_FIXTURE_REQUESTS", "ADE_ACP_LOG", "ADE_CLAUDE_LOG",
          "ADE_CLAUDE_ARGS_LOG", "ADE_CURSOR_LOG",
          "ADE_HARNESS_FIXTURE_SCENARIO", "ADE_ACP_SCENARIO",
          "ADE_CLAUDE_FIXTURE", "ADE_CURSOR_SCENARIO"})
      qunsetenv(name);
  }
  void retryStartsFreshSessionWithCanonicalTranscript_data() { kinds(); }
  void retryStartsFreshSessionWithCanonicalTranscript() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    logging(dir.path());
    QJsonArray history;
    {
      AgentEngine first;
      configure(first, kind, dir.path());
      QVERIFY(turn(first, "KEPT_FIRST"));
      history = first.messages();
    }
    QVERIFY(hasContinuity(history));
    // A restarted engine starts a new process that reads the failing scenario.
    scenario(kind, true);
    AgentEngine engine;
    configure(engine, kind, dir.path());
    engine.setMessages(history);
    QVERIFY(!turn(engine, "FAILED_TURN"));
    const auto failed = engine.turnFailure()["userRowId"].toString();
    QVERIFY(!failed.isEmpty());
    scenario(kind, false);
    const auto before = methods(dir.path(), kind).size();
    const auto argumentsBefore = claudeArguments(dir.path()).size();
    QSignalSpy done(&engine, &AgentEngine::finished);
    QString error;
    QVERIFY2(engine.retryFailedTurn(failed, &error), qPrintable(error));
    QVERIFY(!hasContinuity(engine.messages()));
    QVERIFY(done.size() > 0 || done.wait(8000));
    QVERIFY(engine.lastTurnSucceeded());
    const auto wire = lastWire(dir.path(), kind);
    QVERIFY2(wire.contains("conversation-history"), wire.constData());
    QCOMPARE(wire.count("KEPT_FIRST"), 1);
    QCOMPARE(wire.count("FAILED_TURN"), 1);
    if (kind == "claude-code") {
      const auto arguments = claudeArguments(dir.path()).mid(argumentsBefore);
      QVERIFY(!arguments.isEmpty());
      for (const auto &line : arguments)
        QVERIFY2(!line.contains("--resume="), qPrintable(line));
    } else {
      const auto after = methods(dir.path(), kind).mid(before);
      const auto fresh = kind == "codex" ? "thread/start" : "session/new";
      QVERIFY2(after.contains(fresh), qPrintable(after.join(',')));
      for (const auto *resume :
           {"thread/resume", "session/resume", "session/load"})
        QVERIFY2(!after.contains(resume), qPrintable(after.join(',')));
    }
    QStringList users;
    for (const auto &value : engine.messages())
      if (value.toObject()["role"] == "user")
        users.append(value.toObject()["content"].toString());
    QCOMPARE(users, (QStringList{"KEPT_FIRST", "FAILED_TURN"}));
  }
};
QTEST_GUILESS_MAIN(HistoryRetryHarnessTest)
#include "test_history_retry_harness.moc"
