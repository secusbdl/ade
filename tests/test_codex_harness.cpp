#include "harness/CodexAppServer.h"
#include "harness/CodexHarnessSession.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
using namespace ade;
class CodexHarnessTest : public QObject {
  Q_OBJECT
  QString fixture() const {
    return QDir(QCoreApplication::applicationDirPath())
        .filePath("harness_fixture.exe");
  }
  CodexHarnessOptions options(const QString &path) const {
    return {fixture(), "provider", "model-a",
            "low",     path,       "Fixture instructions",
            false};
  }
private slots:
  void cleanup() {
    qunsetenv("ADE_HARNESS_FIXTURE_SCENARIO");
    qunsetenv("ADE_HARNESS_FIXTURE_PID");
    qunsetenv("ADE_HARNESS_FIXTURE_REQUESTS");
  }
  void managedEnvironmentDropsParentIdentityButKeepsAuthHome() {
    struct Restore {
      QList<QPair<QByteArray,QByteArray>> values;
      QSet<QByteArray> absent;
      void set(const QByteArray &key,const QByteArray &value) {
        if(!qEnvironmentVariableIsSet(key.constData()))absent.insert(key);
        values.append({key,qgetenv(key.constData())});qputenv(key.constData(),value);
      }
      ~Restore(){for(const auto &entry:values)if(absent.contains(entry.first))qunsetenv(entry.first.constData());else qputenv(entry.first.constData(),entry.second);}
    } restore;
    restore.set("CODEX_HOME","fixture-managed-auth-home");
    restore.set("CODEX_WINDOWS_REGISTERED_CORE","fixture-parent-identity");
    restore.set("CODEX_APP_TOOLS_PIPE_PATH","fixture-parent-pipe");
    restore.set("CODEX_THREAD_ID","fixture-parent-thread");
    restore.set("CODEX_PERMISSION_PROFILE","fixture-parent-profile");
    restore.set("CODEX_INTERNAL_ORIGINATOR_OVERRIDE","fixture-parent-origin");
    restore.set("CODEX_SHELL","fixture-packaged-shell");
    restore.set("SHELL","fixture-packaged-shell");
    const auto home=qgetenv("HOME"),profile=qgetenv("USERPROFILE");
    const auto env=JsonRpcProcess::managedEnvironment();
    QCOMPARE(env.value("CODEX_HOME"),QString("fixture-managed-auth-home"));
    QCOMPARE(env.value("HOME").toUtf8(),home);QCOMPARE(env.value("USERPROFILE").toUtf8(),profile);
    for(const auto &key:env.keys())if(key.startsWith("CODEX_",Qt::CaseInsensitive))QCOMPARE(key.toUpper(),QString("CODEX_HOME"));
#ifdef Q_OS_WIN
    QVERIFY(!env.contains("SHELL"));
#endif
  }
  void managedEnvironmentDoesNotForwardKeys() {
    qputenv("ADE_FIXTURE_API_KEY", "not-a-real-key");
    qputenv("ADE_FIXTURE_TOKEN", "not-a-real-token");
    const auto env = JsonRpcProcess::managedEnvironment();
    QVERIFY(!env.contains("ADE_FIXTURE_API_KEY"));
    QVERIFY(!env.contains("ADE_FIXTURE_TOKEN"));
    qunsetenv("ADE_FIXTURE_API_KEY");
    qunsetenv("ADE_FIXTURE_TOKEN");
  }
  void loginOrigins_data() {
    QTest::addColumn<QString>("url");
    QTest::addColumn<bool>("valid");
    QTest::newRow("official")
        << "https://auth.openai.com/oauth/authorize?state=fixture" << true;
    QTest::newRow("plain-http") << "http://auth.openai.com/login" << false;
    QTest::newRow("lookalike")
        << "https://auth.openai.com.evil.test/login" << false;
    QTest::newRow("userinfo") << "https://user@auth.openai.com/login" << false;
    QTest::newRow("port") << "https://auth.openai.com:444/login" << false;
  }
  void loginOrigins() {
    QFETCH(QString, url);
    QFETCH(bool, valid);
    QCOMPARE(CodexAppServer::validLoginUrl(QUrl(url)), valid);
  }
  void accountDiscoveryAndManagedLogin() {
    CodexAppServer server;
    QSignalSpy ready(&server, &CodexAppServer::ready),
        accounts(&server, &CodexAppServer::accountChanged),
        models(&server, &CodexAppServer::modelsReady),
        login(&server, &CodexAppServer::loginFinished),
        urls(&server, &CodexAppServer::loginUrlReady);
    QVERIFY(server.start(fixture()));
    QTRY_COMPARE(ready.size(), 1);
    server.readAccount();
    QTRY_COMPARE(accounts.size(), 1);
    QVERIFY(accounts.first()[0].toBool());
    QCOMPARE(accounts.first()[1].toString(), QString("plus"));
    server.discoverModels();
    QTRY_COMPARE(models.size(), 1);
    QCOMPARE(models.first()[0].toJsonArray().size(), 2);
    server.startLogin();
    QTRY_COMPARE(login.size(), 1);
    QVERIFY(login.first()[0].toBool());
    QCOMPARE(urls.size(), 1);
    QVERIFY(!QJsonDocument(models.first()[0].toJsonArray())
                 .toJson()
                 .contains("private@example.test"));
  }
  void cancelPendingLogin() {
    qputenv("ADE_HARNESS_FIXTURE_SCENARIO", "hold-login");
    CodexAppServer server;
    QSignalSpy ready(&server, &CodexAppServer::ready),
        login(&server, &CodexAppServer::loginFinished),
        urls(&server, &CodexAppServer::loginUrlReady);
    QVERIFY(server.start(fixture()));
    QTRY_COMPARE(ready.size(), 1);
    server.startLogin();
    server.cancelLogin();
    QTRY_COMPARE(login.size(), 1);
    QVERIFY(!login.first()[0].toBool());
    QCOMPARE(urls.size(), 0);
  }
  void invalidLoginUrlNeverOpens() {
    qputenv("ADE_HARNESS_FIXTURE_SCENARIO", "bad-url");
    CodexAppServer server;
    QSignalSpy ready(&server, &CodexAppServer::ready),
        login(&server, &CodexAppServer::loginFinished),
        urls(&server, &CodexAppServer::loginUrlReady);
    QVERIFY(server.start(fixture()));
    QTRY_COMPARE(ready.size(), 1);
    server.startLogin();
    QTRY_COMPARE(login.size(), 1);
    QVERIFY(!login.first()[0].toBool());
    QVERIFY(urls.isEmpty());
  }
  void malformedAndOversized_data() {
    QTest::addColumn<QByteArray>("scenario");
    QTest::newRow("malformed") << QByteArray("malformed");
    QTest::newRow("oversize") << QByteArray("oversize");
  }
  void malformedAndOversized() {
    QFETCH(QByteArray, scenario);
    qputenv("ADE_HARNESS_FIXTURE_SCENARIO", scenario);
    CodexAppServer server;
    QSignalSpy failed(&server, &CodexAppServer::failed);
    QVERIFY(server.start(fixture()));
    QTRY_VERIFY(!failed.isEmpty());
    QVERIFY(!server.isReady());
  }
  void streamedTurnAndResume() {
    QTemporaryDir directory;
    CodexHarnessSession session;
    QSignalSpy events(&session, &CodexHarnessSession::event),
        finished(&session, &CodexHarnessSession::finished),
        state(&session, &CodexHarnessSession::sessionStateChanged);
    QVERIFY(session.startTurn(
        options(directory.path()),
        {QJsonObject{{"type", "text"}, {"text", "Reply fixture"}}}));
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(finished.first()[0].toJsonObject()["status"].toString(),
             QString("completed"));
    QVERIFY(events.size() >= 4);
    QCOMPARE(events.first()[0].toJsonObject()["type"].toString(),
             QString("reasoning"));
    QCOMPARE(finished.first()[0]
                 .toJsonObject()["partialText"]
                 .toObject()["answer"]
                 .toString(),
             QString("HARNESS_OK"));
    const auto saved = session.sessionState();
    QCOMPARE(saved["threadId"].toString(), QString("thread-1"));
    QVERIFY(session.startTurn(
        options(directory.path()),
        {QJsonObject{{"type", "text"}, {"text", "Again"}}}, saved));
    QTRY_COMPARE(finished.size(), 2);
    QCOMPARE(session.sessionState()["threadId"], saved["threadId"]);
    QCOMPARE(state.size(), 2);
  }
  void signedOutNeverStartsTurn() {
    qputenv("ADE_HARNESS_FIXTURE_SCENARIO", "signed-out");
    QTemporaryDir directory;
    CodexHarnessSession session;
    QSignalSpy finished(&session, &CodexHarnessSession::finished),
        events(&session, &CodexHarnessSession::event);
    QVERIFY(session.startTurn(options(directory.path()),
                              {QJsonObject{{"type", "text"}, {"text", "hi"}}}));
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(finished.first()[0].toJsonObject()["status"].toString(),
             QString("failed"));
    QVERIFY(events.isEmpty());
  }
  void approvalIsCorrelatedAndSingleUse_data() {
    QTest::addColumn<bool>("allow");
    QTest::newRow("accept") << true;
    QTest::newRow("decline") << false;
  }
  void approvalIsCorrelatedAndSingleUse() {
    QFETCH(bool, allow);
    qputenv("ADE_HARNESS_FIXTURE_SCENARIO", "approval");
    QTemporaryDir directory;
    CodexHarnessSession session;
    QSignalSpy approval(&session, &CodexHarnessSession::approvalRequired),
        finished(&session, &CodexHarnessSession::finished);
    QVERIFY(
        session.startTurn(options(directory.path()),
                          {QJsonObject{{"type", "text"}, {"text", "tool"}}}));
    QTRY_COMPARE(approval.size(), 1);
    QVERIFY(!session.answerApproval("stale", true));
    const auto token = approval.first()[0].toString();
    QVERIFY(session.answerApproval(token, allow));
    QVERIFY(!session.answerApproval(token, !allow));
    QTRY_COMPARE(finished.size(), 1);
    const auto items = finished.first()[0].toJsonObject()["items"].toArray();
    bool found = false;
    for (const auto &item : items)
      if (item.toObject()["id"] == "command") {
        found = true;
        QCOMPARE(item.toObject()["status"].toString(),
                 allow ? QString("completed") : QString("declined"));
      }
    QVERIFY(found);
  }
  void questionUsesExactIds() {
    qputenv("ADE_HARNESS_FIXTURE_SCENARIO", "question");
    QTemporaryDir directory;
    CodexHarnessSession session;
    QSignalSpy questions(&session, &CodexHarnessSession::questionRequired),
        finished(&session, &CodexHarnessSession::finished);
    QVERIFY(session.startTurn(
        options(directory.path()),
        {QJsonObject{{"type", "text"}, {"text", "question"}}}));
    QTRY_COMPARE(questions.size(), 1);
    const auto token = questions.first()[0].toString();
    QVERIFY(!session.answerQuestion(token, {{"Choice", "One"}}));
    QVERIFY(session.answerQuestion(token, {{"choice", "One"}}));
    QVERIFY(!session.answerQuestion(token, {{"choice", "Two"}}));
    QTRY_COMPARE(finished.size(), 1);
  }
  void cancelRetainsPartialAndRejectsLateAnswers() {
    qputenv("ADE_HARNESS_FIXTURE_SCENARIO", "approval");
    QTemporaryDir directory;
    CodexHarnessSession session;
    QSignalSpy approval(&session, &CodexHarnessSession::approvalRequired),
        finished(&session, &CodexHarnessSession::finished);
    QVERIFY(
        session.startTurn(options(directory.path()),
                          {QJsonObject{{"type", "text"}, {"text", "hold"}}}));
    QTRY_COMPARE(approval.size(), 1);
    session.interrupt();
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(finished.first()[0].toJsonObject()["status"].toString(),
             QString("interrupted"));
    QVERIFY(!session.answerApproval(approval.first()[0].toString(), true));
    QVERIFY(!finished.first()[0]
                 .toJsonObject()["partialReasoning"]
                 .toObject()
                 .isEmpty());
  }
  void forkCreatesDifferentThread() {
    QTemporaryDir directory;
    CodexHarnessSession session;
    QSignalSpy finished(&session, &CodexHarnessSession::finished);
    const QJsonArray input{QJsonObject{{"type", "text"}, {"text", "hi"}}};
    QVERIFY(session.startTurn(options(directory.path()), input));
    QTRY_COMPARE(finished.size(), 1);
    const auto state = CodexHarnessSession::forkState(session.sessionState());
    session.shutdown();
    QVERIFY(session.startTurn(options(directory.path()), input, state));
    QTRY_COMPARE(finished.size(), 2);
    QCOMPARE(session.sessionState()["threadId"].toString(),
             QString("thread-fork"));
  }
  void retargetedWorkspaceCannotResumeOldThread() {
#ifdef Q_OS_WIN
    QTemporaryDir directory;
    const auto a = directory.filePath("a"), b = directory.filePath("b"),
               alias = directory.filePath("alias");
    QVERIFY(QDir().mkpath(a));
    QVERIFY(QDir().mkpath(b));
    QCOMPARE(QProcess::execute("cmd.exe", {"/c", "mklink", "/J",
                                           QDir::toNativeSeparators(alias),
                                           QDir::toNativeSeparators(a)}),
             0);
    CodexHarnessSession first;
    QSignalSpy firstDone(&first, &CodexHarnessSession::finished);
    const QJsonArray input{QJsonObject{{"type", "text"}, {"text", "first"}}};
    QVERIFY(first.startTurn(options(alias), input));
    QTRY_COMPARE(firstDone.size(), 1);
    const auto saved = first.sessionState();
    QVERIFY(saved["workspace"].toString().endsWith("/a"));
    QVERIFY(RemoveDirectoryW(
        QDir::toNativeSeparators(alias).toStdWString().c_str()));
    QCOMPARE(QProcess::execute("cmd.exe", {"/c", "mklink", "/J",
                                           QDir::toNativeSeparators(alias),
                                           QDir::toNativeSeparators(b)}),
             0);
    qputenv("ADE_HARNESS_FIXTURE_SCENARIO", "resume-error");
    CodexHarnessSession second;
    QSignalSpy secondDone(&second, &CodexHarnessSession::finished);
    QVERIFY(second.startTurn(options(alias), input, saved));
    QTRY_COMPARE(secondDone.size(), 1);
    QCOMPARE(secondDone.first()[0].toJsonObject()["status"].toString(),
             QString("completed"));
    QVERIFY(second.sessionState()["workspace"].toString().endsWith("/b"));
    second.shutdown();
    first.shutdown();
    QVERIFY(RemoveDirectoryW(
        QDir::toNativeSeparators(alias).toStdWString().c_str()));
#endif
  }
  void replacedPhysicalRootCannotResumeOldThread() {
    QTemporaryDir directory;
    const auto root = directory.filePath("workspace");
    QVERIFY(QDir().mkpath(root));
    CodexHarnessSession first;
    QSignalSpy firstDone(&first, &CodexHarnessSession::finished);
    const QJsonArray input{QJsonObject{{"type", "text"}, {"text", "first"}}};
    QVERIFY(first.startTurn(options(root), input));
    QTRY_COMPARE(firstDone.size(), 1);
    const auto saved = first.sessionState();
    first.shutdown();
    QVERIFY(QDir().rename(root, directory.filePath("old-workspace")));
    QVERIFY(QDir().mkpath(root));
    qputenv("ADE_HARNESS_FIXTURE_SCENARIO", "resume-error");
    CodexHarnessSession second;
    QSignalSpy secondDone(&second, &CodexHarnessSession::finished);
    QVERIFY(second.startTurn(options(root), input, saved));
    QTRY_COMPARE(secondDone.size(), 1);
    QCOMPARE(secondDone.first()[0].toJsonObject()["status"].toString(),
             QString("completed"));
    QVERIFY(second.sessionState()["workspaceIdentity"] !=
            saved["workspaceIdentity"]);
  }
  void resumeFailureCreatesOneFreshThread_data() {
    QTest::addColumn<bool>("startFails");
    QTest::newRow("fresh-thread-success") << false;
    QTest::newRow("fresh-thread-failure-is-terminal") << true;
  }
  void resumeFailureCreatesOneFreshThread() {
    QFETCH(bool, startFails);
    QTemporaryDir directory;
    CodexHarnessSession session;
    QSignalSpy finished(&session, &CodexHarnessSession::finished);
    const QJsonArray input{QJsonObject{{"type", "text"}, {"text", "CURRENT"}}};
    QVERIFY(session.startTurn(options(directory.path()), input));
    QTRY_COMPARE(finished.size(), 1);
    const auto saved = session.sessionState();
    session.shutdown();
    const auto log = directory.filePath("requests.jsonl");
    qputenv("ADE_HARNESS_FIXTURE_REQUESTS", log.toUtf8());
    qputenv("ADE_HARNESS_FIXTURE_SCENARIO",
            startFails ? "thread-error" : "resume-error");
    auto recovery = options(directory.path());
    recovery.historyContext = "CANONICAL_PRIOR_HISTORY";
    QVERIFY(session.startTurn(recovery, input, saved));
    QTRY_COMPARE(finished.size(), 2);
    const auto result = finished.last()[0].toJsonObject();
    QCOMPARE(result["status"].toString(),
             startFails ? QString("failed") : QString("completed"));
    QVERIFY(!QJsonDocument(result).toJson().contains("secret-fixture-error"));
    QFile file(log);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QStringList threadMethods;
    QJsonArray turns;
    for (const auto &line : file.readAll().split('\n')) {
      const auto row = QJsonDocument::fromJson(line).object();
      const auto method = row["method"].toString();
      if (method.startsWith("thread/"))
        threadMethods.append(method);
      if (method == "turn/start")
        turns.append(row["params"]);
    }
    QCOMPARE(threadMethods, QStringList({"thread/resume", "thread/start"}));
    QCOMPARE(turns.size(), startFails ? 0 : 1);
    if (!startFails) {
      const auto wire = QJsonDocument(turns).toJson();
      QCOMPARE(wire.count("CANONICAL_PRIOR_HISTORY"), 1);
      QCOMPARE(wire.count("CURRENT"), 1);
      const auto after = session.sessionState();
      QVERIFY(!after.isEmpty());
      QCOMPARE(after["workspaceIdentity"], saved["workspaceIdentity"]);
    }
  }
  void deleteDuringStream() {
    QTemporaryDir directory;
    QPointer<CodexHarnessSession> session = new CodexHarnessSession;
    connect(session, &CodexHarnessSession::event, session,
            [&] { delete session.data(); });
    QVERIFY(
        session->startTurn(options(directory.path()),
                           {QJsonObject{{"type", "text"}, {"text", "hi"}}}));
    QTRY_VERIFY(session.isNull());
  }
  void cancelFromStreamObserver() {
    QTemporaryDir directory;
    CodexHarnessSession session;
    QSignalSpy finished(&session, &CodexHarnessSession::finished);
    connect(&session, &CodexHarnessSession::event, &session,
            [&](const QJsonObject &) { session.interrupt(); });
    QVERIFY(session.startTurn(options(directory.path()),
                              {QJsonObject{{"type", "text"}, {"text", "hi"}}}));
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(finished.first()[0].toJsonObject()["status"].toString(),
             QString("interrupted"));
  }
  void ownedChildStopsWithTransport() {
#ifdef Q_OS_WIN
    QTemporaryDir directory;
    const auto path = directory.filePath("pid.txt");
    qputenv("ADE_HARNESS_FIXTURE_SCENARIO", "child");
    qputenv("ADE_HARNESS_FIXTURE_PID", path.toUtf8());
    JsonRpcProcess process;
    QSignalSpy started(&process, &JsonRpcProcess::started);
    QVERIFY(process.start(fixture(), {}, directory.path(),
                          JsonRpcProcess::managedEnvironment()));
    QTRY_COMPARE(started.size(), 1);
    QTRY_VERIFY(QFileInfo::exists(path));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto pid = file.readAll().toULong();
    file.close();
    QVERIFY(pid > 0);
    HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    QVERIFY(child);
    process.stop();
    QCOMPARE(WaitForSingleObject(child, 3000), DWORD(WAIT_OBJECT_0));
    CloseHandle(child);
#endif
  }
};
QTEST_GUILESS_MAIN(CodexHarnessTest)
#include "test_codex_harness.moc"
