#include "core/AgentEngine.h"
#include "mutations/FileMutationJournal.h"
#include "mutations/FileMutationStore.h"
#include "runtime/AgentToolHost.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
struct Call {
  QString id, name;
  QJsonObject args;
};
QByteArray event(const QJsonObject &object) {
  return "data: " + QJsonDocument(object).toJson(QJsonDocument::Compact) +
         "\n\n";
}
QByteArray response(const QString &protocol, const QList<Call> &calls = {}) {
  if (protocol == "responses") {
    QJsonArray output;
    for (const auto &call : calls)
      output.append(QJsonObject{
          {"type", "function_call"},
          {"id", "item-" + call.id},
          {"call_id", call.id},
          {"name", call.name},
          {"arguments", QString::fromUtf8(QJsonDocument(call.args).toJson(
                            QJsonDocument::Compact))}});
    if (calls.isEmpty())
      output.append(QJsonObject{
          {"type", "message"},
          {"id", "message"},
          {"role", "assistant"},
          {"content", QJsonArray{QJsonObject{{"type", "output_text"},
                                             {"text", "DONE"}}}}});
    return event({{"type", "response.completed"},
                  {"response",
                   QJsonObject{{"status", "completed"}, {"output", output}}}});
  }
  if (protocol == "anthropic") {
    QByteArray body = event({{"type", "message_start"},
                             {"message", QJsonObject{{"role", "assistant"}}}});
    int index = 0;
    for (const auto &call : calls) {
      body += event({{"type", "content_block_start"},
                     {"index", index},
                     {"content_block", QJsonObject{{"type", "tool_use"},
                                                   {"id", call.id},
                                                   {"name", call.name},
                                                   {"input", QJsonObject{}}}}});
      body += event(
          {{"type", "content_block_delta"},
           {"index", index},
           {"delta",
            QJsonObject{{"type", "input_json_delta"},
                        {"partial_json",
                         QString::fromUtf8(QJsonDocument(call.args).toJson(
                             QJsonDocument::Compact))}}}});
      body += event({{"type", "content_block_stop"}, {"index", index++}});
    }
    if (calls.isEmpty())
      body += event(
          {{"type", "content_block_start"},
           {"index", 0},
           {"content_block", QJsonObject{{"type", "text"}, {"text", "DONE"}}}});
    return body +
           event({{"type", "message_delta"},
                  {"delta",
                   QJsonObject{{"stop_reason",
                                calls.isEmpty() ? "end_turn" : "tool_use"}}}}) +
           event({{"type", "message_stop"}});
  }
  if (protocol == "google") {
    QJsonArray parts;
    for (const auto &call : calls)
      parts.append(
          QJsonObject{{"functionCall", QJsonObject{{"id", call.id},
                                                   {"name", call.name},
                                                   {"args", call.args}}}});
    if (calls.isEmpty())
      parts.append(QJsonObject{{"text", "DONE"}});
    const QJsonObject candidate{
        {"content", QJsonObject{{"role", "model"}, {"parts", parts}}},
        {"finishReason", "STOP"}};
    return event({{"candidates", QJsonArray{candidate}}});
  }
  QJsonObject delta;
  if (calls.isEmpty())
    delta["content"] = "DONE";
  else {
    QJsonArray tools;
    int index = 0;
    for (const auto &call : calls)
      tools.append(QJsonObject{
          {"index", index++},
          {"id", call.id},
          {"type", "function"},
          {"function",
           QJsonObject{
               {"name", call.name},
               {"arguments", QString::fromUtf8(QJsonDocument(call.args).toJson(
                                 QJsonDocument::Compact))}}}});
    delta["tool_calls"] = tools;
  }
  return event({{"choices", QJsonArray{QJsonObject{
                                {"delta", delta},
                                {"finish_reason",
                                 calls.isEmpty() ? "stop" : "tool_calls"}}}}}) +
         "data: [DONE]\n\n";
}
class Model : public QObject {
public:
  QTcpServer server;
  QString protocol = "openai";
  QList<QByteArray> replies;
  QList<QJsonObject> requests;
  Model() {
    server.listen(QHostAddress::LocalHost, 0);
    connect(&server, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = server.nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
          if (socket->property("handled").toBool()) {
            socket->readAll();
            return;
          }
          const auto bytes =
              socket->property("request").toByteArray() + socket->readAll();
          socket->setProperty("request", bytes);
          const auto split = bytes.indexOf("\r\n\r\n");
          if (split < 0)
            return;
          int length = 0;
          for (const auto &line : bytes.left(split).split('\n'))
            if (line.toLower().startsWith("content-length:"))
              length = line.mid(15).trimmed().toInt();
          if (bytes.size() < split + 4 + length)
            return;
          socket->setProperty("handled", true);
          requests.append(
              QJsonDocument::fromJson(bytes.mid(split + 4, length)).object());
          const auto body =
              replies.isEmpty() ? response(protocol) : replies.takeFirst();
          socket->write(
              "HTTP/1.1 200 OK\r\nContent-Type: "
              "text/event-stream\r\nConnection: close\r\nContent-Length: " +
              QByteArray::number(body.size()) + "\r\n\r\n" + body);
          socket->disconnectFromHost();
        });
      }
    });
  }
  ProviderConfig provider() const {
    return {"fixture",
            "Fixture",
            QString("http://127.0.0.1:%1/v1").arg(server.serverPort()),
            "fixture-only",
            "gemini-3.1-flash-lite",
            protocol};
  }
};
bool write(const QString &path, const QByteArray &body) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(body) == body.size();
}
QByteArray read(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
} // namespace

class MutationFindingsTest : public QObject {
  Q_OBJECT
private slots:

  void oldSettlementCannotClearReplacementCancellation() {
    QTemporaryDir data, root;
    FileMutationJournal journal(data.path(), "chat");
    QVERIFY(journal.bindMount("ws1", root.path(), true));
    struct Hold {
      QSemaphore firstEntered, firstRelease, secondEntered, secondRelease;
      std::atomic_bool sawCancel = false;
    };
    auto hold = std::make_shared<Hold>();
    bool firstReleased = false, secondReleased = false;
    auto guard = qScopeGuard([&] {
      if (!firstReleased)
        hold->firstRelease.release();
      if (!secondReleased)
        hold->secondRelease.release();
    });
    journal.setBoundaryObserverForTests([hold](const QString &point, int) {
      if (point == "after-commit") {
        hold->firstEntered.release();
        hold->firstRelease.acquire();
      }
    });
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    tools.setFileMutationExecutor(journal.executor());
    QSignalSpy settled(&tools, &ToolExecutor::mutationSettled),
        changed(&tools, &ToolExecutor::fileChanged),
        done(&tools, &ToolExecutor::completed);
    tools.execute("same-id", "write",
                  {{"path", "ws1/first.txt"}, {"content", "committed"}});
    QTRY_VERIFY(hold->firstEntered.available());
    tools.cancel();
    const auto secondPath = root.filePath("second.txt");
    tools.setFileMutationExecutor(
        [hold, secondPath](const FileMutationInvocation &,
                           const std::atomic_bool *canceled) {
          hold->secondEntered.release();
          hold->secondRelease.acquire();
          hold->sawCancel = canceled && canceled->load();
          if (!hold->sawCancel)
            write(secondPath, "WRONG");
          return FileMutationOutcome{hold->sawCancel ? "Cancelled"
                                                     : "Unexpected commit",
                                     hold->sawCancel,
                                     {},
                                     {}};
        });
    tools.execute("same-id", "write",
                  {{"path", "ws1/second.txt"}, {"content", "replacement"}});
    QTRY_VERIFY(hold->secondEntered.available());
    firstReleased = true;
    hold->firstRelease.release();
    QTRY_COMPARE(settled.size(), 1);
    QCOMPARE(changed.size(), 1);
    tools.cancel();
    secondReleased = true;
    hold->secondRelease.release();
    QTRY_COMPARE(settled.size(), 2);
    QVERIFY(hold->sawCancel.load());
    QVERIFY(!QFile::exists(secondPath));
    QCOMPARE(done.size(), 2);
    for (const auto &entry : done)
      QVERIFY(entry[2].toBool());
    QCOMPARE(read(root.filePath("first.txt")), QByteArray("committed"));
  }
  void committedChangeObserverCanDeleteExecutor() {
    QTemporaryDir data, root;
    QVERIFY(write(root.filePath("source.txt"), "source"));
    QVERIFY(write(root.filePath("target.txt"), "target"));
    FileMutationJournal journal(data.path(), "chat");
    QVERIFY(journal.bindMount("ws1", root.path(), true));
    QPointer<ToolExecutor> tools = new ToolExecutor;
    QVERIFY(tools->setWorkspace(root.path()));
    tools->setFileMutationExecutor(journal.executor());
    QSignalSpy changed(tools, &ToolExecutor::fileChanged),
        settled(tools, &ToolExecutor::mutationSettled);
    connect(tools, &ToolExecutor::fileChanged, this, [&] { delete tools; });
    tools->execute("move", "copy",
                   {{"input_path", "ws1/source.txt"},
                    {"output_path", "ws1/target.txt"},
                    {"move", true}});
    QTRY_VERIFY(tools.isNull());
    QCOMPARE(settled.size(), 1);
    QCOMPARE(changed.size(), 1);
    QVERIFY(!QFile::exists(root.filePath("source.txt")));
    QCOMPARE(read(root.filePath("target.txt")), QByteArray("source"));
    QCOMPARE(journal.history().size(), 1);
  }
  void recordedLeafCaseSurvivesStoreRestart() {
    QTemporaryDir data, root;
    QVERIFY(write(root.filePath("README.md"), "before"));
    QString error;
    {
      mutations::FileMutationStore store(data.path(), "chat");
      QVERIFY(store.open(&error));
      QVERIFY(store.bindMount("ws1", root.path(), true, &error));
      mutations::Request r;
      r.agentId = "chat";
      r.nonce = "remove";
      r.toolCallId = "call";
      r.tool = "delete";
      r.path = "ws1/readme.md";
      QVERIFY(store.execute(r).success);
    }
    {
      mutations::FileMutationStore store(data.path(), "chat");
      QVERIFY(store.open(&error));
      QVERIFY(store.bindMount("ws1", root.path(), true, &error));
      QVERIFY(store.undo("chat", "remove", "restore").success);
      QVERIFY(QDir(root.path()).entryList(QDir::Files).contains("README.md"));
      mutations::Request r;
      r.agentId = "chat";
      r.nonce = "new";
      r.toolCallId = "call";
      r.tool = "write";
      r.path = "ws1/NewMixedCase.md";
      r.content = "new";
      QVERIFY(store.execute(r).success);
      QVERIFY(
          QDir(root.path()).entryList(QDir::Files).contains("NewMixedCase.md"));
    }
  }
  void plansMkdirDoesNotCreateWorkspaceShadow() {
    QTemporaryDir root;
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    QSignalSpy done(&tools, &ToolExecutor::completed);
    tools.execute("reserved", "mkdir", {{"path", "plans/sub"}});
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(done[0][2].toBool());
    QVERIFY(!QFile::exists(root.filePath("plans")));
    tools.execute("explicit", "mkdir", {{"path", "ws1/plans/sub"}});
    QTRY_COMPARE(done.size(), 2);
    QVERIFY(!done[1][2].toBool());
    QVERIFY(QDir(root.filePath("plans/sub")).exists());
  }
  void modelWireOmitsUiDiffButRetainsJournal_data() {
    QTest::addColumn<QString>("protocol");
    for (const auto *p : {"openai", "responses", "anthropic", "google"})
      QTest::newRow(p) << QString(p);
  }
  void modelWireOmitsUiDiffButRetainsJournal() {
    QFETCH(QString, protocol);
    QTemporaryDir data, root;
    QVERIFY(write(root.filePath("file.txt"), "before\n"));
    Model model;
    model.protocol = protocol;
    model.replies = {response(
        protocol, {{"call",
                    "write",
                    {{"path", "ws1/file.txt"}, {"content", "after\n"}}}})};
    AgentEngine engine;
    engine.setProvider(model.provider());
    engine.setAlwaysAllow(true);
    engine.setWorkspace(root.path());
    AgentToolHost host(&engine, nullptr, "chat", data.path());
    QVERIFY(host.setWorkspace(root.path()));
    QSignalSpy done(&engine, &AgentEngine::finished);
    engine.submit("SYNTHETIC_WRITE");
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(engine.lastTurnSucceeded());
    QVERIFY(model.requests.size() >= 2);
    const auto wire = QJsonDocument(model.requests.last()).toJson();
    QVERIFY(!wire.contains("_diff"));
    QVERIFY(!wire.contains("fileDiff"));
    QVERIFY(!wire.contains("mutationId"));
    QJsonObject result;
    for (const auto &row : engine.messages())
      if (row.toObject().value("role") == "tool")
        result = row.toObject();
    QVERIFY(!result.isEmpty());
    QVERIFY(!result.value("metadata")
                 .toObject()
                 .value("fileDiff")
                 .toObject()
                 .isEmpty());
    QCOMPARE(
        result.value("metadata").toObject().value("mutations").toArray().size(),
        1);
    QCOMPARE(host.mutationHistory().size(), 1);
    QCOMPARE(read(root.filePath("file.txt")), QByteArray("after\n"));
  }
  void restartReconciliationAlsoOmitsUiDiff() {
    QTemporaryDir data, root;
    Model model;
    model.replies = {response(
        "openai",
        {{"call", "write", {{"path", "file.txt"}, {"content", "durable"}}}})};
    auto *engine = new AgentEngine;
    engine->setProvider(model.provider());
    engine->setAlwaysAllow(true);
    engine->setWorkspace(root.path());
    auto *host =
        new AgentToolHost(engine, nullptr, "chat", data.path(), engine);
    QVERIFY(host->setWorkspace(root.path()));
    QJsonArray persisted;
    engine->setQueueCommitter(
        [&](const QJsonArray &history, const QJsonArray &, QString *) {
          persisted = history;
          return true;
        });
    FileMutationJournal journal(data.path(), "chat");
    QVERIFY(journal.bindMount("ws1", root.path(), true));
    QVERIFY(
        journal.bindMount("apps", host->apps()->agentAppsDirectory(), true));
    struct Hold {
      QSemaphore entered, release, finished;
    };
    auto hold = std::make_shared<Hold>();
    auto release = qScopeGuard([hold] { hold->release.release(); });
    const auto apply = journal.executor();
    engine->setFileMutationExecutor(
        [apply, hold](const FileMutationInvocation &call,
                      const std::atomic_bool *canceled) {
          auto result = apply(call, canceled);
          hold->entered.release();
          hold->release.acquire();
          hold->finished.release();
          return result;
        });
    engine->submit("SYNTHETIC_WRITE");
    QTRY_VERIFY(hold->entered.available());
    delete engine;
    hold->release.release();
    QTRY_VERIFY(hold->finished.available());
    AgentEngine restored;
    restored.setMessages(persisted);
    AgentToolHost recovered(&restored, nullptr, "chat", data.path());
    QJsonObject result;
    for (const auto &row : restored.messages())
      if (row.toObject().value("role") == "tool")
        result = row.toObject();
    QVERIFY(!result.isEmpty());
    QVERIFY(!result.value("content").toString().contains("_diff"));
    QCOMPARE(
        result.value("metadata").toObject().value("mutations").toArray().size(),
        1);
    QCOMPARE(read(root.filePath("file.txt")), QByteArray("durable"));
  }
  void directModelMutationNotifiesActualHost_data() {
    QTest::addColumn<QString>("tool");
    QTest::newRow("write") << QString("write");
    QTest::newRow("multiEdit") << QString("multiEdit");
  }
  void directModelMutationNotifiesActualHost() {
    QFETCH(QString, tool);
    QTemporaryDir data, root;
    QVERIFY(write(root.filePath("file.txt"), "before"));
    Model model;
    QJsonObject args{{"path", "ws1/file.txt"}};
    if (tool == "write")
      args["content"] = "after";
    else
      args["edits"] = QJsonArray{
          QJsonObject{{"old_string", "before"}, {"new_string", "after"}}};
    model.replies = {response("openai", {{"call", tool, args}})};
    AgentEngine engine;
    engine.setProvider(model.provider());
    engine.setAlwaysAllow(true);
    engine.setWorkspace(root.path());
    AgentToolHost host(&engine, nullptr, "chat", data.path());
    QVERIFY(host.setWorkspace(root.path()));
    QSignalSpy changed(&host, &AgentToolHost::fileChanged),
        done(&engine, &AgentEngine::finished);
    engine.submit("SYNTHETIC_FILE_MUTATION");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
    QVERIFY(engine.lastTurnSucceeded());
    QCOMPARE(read(root.filePath("file.txt")), QByteArray("after"));
    QCOMPARE(host.mutationHistory().size(), 1);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(changed[0][1].toByteArray(), QByteArray("before"));
    QCOMPARE(changed[0][2].toByteArray(), QByteArray("after"));
  }
  void cancelAfterActualCommitStillNotifies() {
    QTemporaryDir data, root;
    FileMutationJournal journal(data.path(), "chat");
    QString error;
    QVERIFY(journal.bindMount("ws1", root.path(), true, &error));
    QSemaphore entered, release;
    bool released = false;
    auto guard = qScopeGuard([&] {
      if (!released)
        release.release();
    });
    journal.setBoundaryObserverForTests([&](const QString &point, int) {
      if (point == "after-commit") {
        entered.release();
        release.acquire();
      }
    });
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    tools.setFileMutationExecutor(journal.executor());
    QSignalSpy changed(&tools, &ToolExecutor::fileChanged),
        settled(&tools, &ToolExecutor::mutationSettled),
        done(&tools, &ToolExecutor::completed);
    tools.execute("original", "write",
                  {{"path", "ws1/file.txt"}, {"content", "committed"}});
    QTRY_VERIFY_WITH_TIMEOUT(entered.available() > 0, 5000);
    tools.cancel();
    released = true;
    release.release();
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 1, 5000);
    QCOMPARE(read(root.filePath("file.txt")), QByteArray("committed"));
    QCOMPARE(journal.history().size(), 1);
    QVERIFY(settled[0][4]
                .toJsonObject()
                .value("mutations")
                .toArray()[0]
                .toObject()
                .value("committed")
                .toBool());
    QCOMPARE(changed.size(), 1);
    QCOMPARE(done.size(), 1);
    QVERIFY(done[0][2].toBool());
  }
  void settlementObserverCancelStillNotifies() {
    QTemporaryDir data, root;
    FileMutationJournal journal(data.path(), "chat");
    QVERIFY(journal.bindMount("ws1", root.path(), true));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    tools.setFileMutationExecutor(journal.executor());
    connect(&tools, &ToolExecutor::mutationSettled, &tools,
            [&] { tools.cancel(); });
    QSignalSpy changed(&tools, &ToolExecutor::fileChanged),
        settled(&tools, &ToolExecutor::mutationSettled);
    tools.execute("original", "write",
                  {{"path", "ws1/file.txt"}, {"content", "committed"}});
    QTRY_COMPARE(settled.size(), 1);
    QCOMPARE(read(root.filePath("file.txt")), QByteArray("committed"));
    QCOMPARE(changed.size(), 1);
  }
  void existingLeafCaseSurvivesMutationAndUndo_data() {
    QTest::addColumn<QString>("tool");
    for (const auto *tool : {"write", "multiEdit", "copy", "delete"})
      QTest::newRow(tool) << QString(tool);
  }
  void existingLeafCaseSurvivesMutationAndUndo() {
    QFETCH(QString, tool);
    QTemporaryDir data, root;
    QVERIFY(write(root.filePath("README.md"), "before"));
    QVERIFY(write(root.filePath("source.txt"), "after"));
    mutations::FileMutationStore store(data.path(), "chat");
    QString error;
    QVERIFY(store.open(&error));
    QVERIFY(store.bindMount("ws1", root.path(), true, &error));
    mutations::Request request;
    request.agentId = "chat";
    request.nonce = "mutation";
    request.toolCallId = "call";
    request.tool = tool;
    request.path = "ws1/readme.md";
    request.content = "after";
    request.edits = {{"before", "after", false}};
    if (tool == "copy") {
      request.path = "ws1/source.txt";
      request.outputPath = "ws1/readme.md";
    }
    const auto result = store.execute(request);
    QVERIFY2(result.success, qPrintable(result.error));
    if (tool != "delete") {
      QCOMPARE(read(root.filePath("README.md")), QByteArray("after"));
      QVERIFY(QDir(root.path()).entryList(QDir::Files).contains("README.md"));
      QVERIFY(!QDir(root.path()).entryList(QDir::Files).contains("readme.md"));
    }
    const auto undo = store.undo("chat", "mutation", "undo");
    QVERIFY2(undo.success, qPrintable(undo.error));
    QCOMPARE(read(root.filePath("README.md")), QByteArray("before"));
    QVERIFY(QDir(root.path()).entryList(QDir::Files).contains("README.md"));
  }
  void ordinaryHexLikeFolderNamesRemainWorkspaceRelative_data() {
    QTest::addColumn<QString>("folder");
    QTest::addColumn<QString>("tool");
    for (const auto *name : {"web", "wd", "wade"})
      for (const auto *tool : {"read", "write", "multiEdit"})
        QTest::newRow(qPrintable(QString(name) + "-" + tool))
            << QString(name) << QString(tool);
  }
  void ordinaryHexLikeFolderNamesRemainWorkspaceRelative() {
    QFETCH(QString, folder);
    QFETCH(QString, tool);
    QTemporaryDir data, root;
    QVERIFY(QDir(root.path()).mkdir(folder));
    QVERIFY(write(root.filePath(folder + "/file.txt"), "before"));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    FileMutationJournal journal(data.path(), "chat");
    QVERIFY(journal.bindMount("ws1", root.path(), true));
    tools.setFileMutationExecutor(journal.executor());
    QSignalSpy done(&tools, &ToolExecutor::completed);
    QJsonObject args{{"path", folder + "/file.txt"}};
    if (tool == "read")
      args["raw"] = true;
    else if (tool == "write")
      args["content"] = "after";
    else
      args["edits"] = QJsonArray{
          QJsonObject{{"old_string", "before"}, {"new_string", "after"}}};
    tools.execute("call", tool, args);
    QTRY_COMPARE(done.size(), 1);
    QVERIFY2(!done[0][2].toBool(), qPrintable(done[0][1].toString()));
    QCOMPARE(read(root.filePath(folder + "/file.txt")),
             tool == "read" ? QByteArray("before") : QByteArray("after"));
  }
  void unknownMountsRemainUnavailable_data() {
    QTest::addColumn<QString>("prefix");
    for (const auto *name :
         {"wdead", "wcafe", "w2", "ws2", "att", "logs", "plans"})
      QTest::newRow(name) << QString(name);
  }
  void unknownMountsRemainUnavailable() {
    QFETCH(QString, prefix);
    QTemporaryDir data, root;
    QVERIFY(QDir(root.path()).mkdir(prefix));
    QVERIFY(write(root.filePath(prefix + "/file.txt"), "before"));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    QString error;
    QVERIFY(tools.resolvePath(prefix + "/file.txt", &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(
        !tools.resolvePath("ws1/" + prefix + "/file.txt", &error).isEmpty());
    FileMutationJournal journal(data.path(), "chat");
    QVERIFY(journal.bindMount("ws1", root.path(), true));
    FileMutationInvocation call{
        "call",
        "write",
        tools.workspace(),
        {},
        {{"path", prefix + "/file.txt"}, {"content", "after"}},
        "invocation"};
    QVERIFY(journal.executor()(call, nullptr).error);
    QCOMPARE(read(root.filePath(prefix + "/file.txt")), QByteArray("before"));
  }
};
QTEST_MAIN(MutationFindingsTest)
#include "test_mutation_findings.moc"
