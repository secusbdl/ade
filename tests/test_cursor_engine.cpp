#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class CursorEngineTest : public QObject {
  Q_OBJECT
  ProviderConfig provider() {
    ProviderConfig p;
    p.id = "cursor-provider";
    p.name = "Cursor";
    p.kind = "cursor";
    p.protocol = "cursor-acp";
    p.model = "model-b";
    p.templateId = "cursor";
    p.harnessExecutable = QDir(QCoreApplication::applicationDirPath())
                              .filePath("cursor_fixture.exe");
    return p;
  }
  void configure(AgentEngine &e, const QString &path,
                 const QString &owner = "owner") {
    e.setProvider(provider());
    e.setHarnessContext(owner, path);
    e.setWorkspace(path);
  }
  void connectOnly(AgentEngine &e) {
    connect(&e, &AgentEngine::approvalRequired, &e,
            [&e](const QString &id, const QString &name, const QJsonObject &) {
              if (name == "Connect Cursor account")
                e.approveTool(id, true);
            });
  }
  QJsonArray calls(const QString &path, const QString &method) {
    QFile f(path);
    QJsonArray rows;
    if (f.open(QIODevice::ReadOnly))
      for (const auto &line : f.readAll().split('\n')) {
        const auto o = QJsonDocument::fromJson(line).object();
        if (o["method"] == method)
          rows.append(o["params"]);
      }
    return rows;
  }
private slots:
  void cleanup() {
    qunsetenv("ADE_CURSOR_SCENARIO");
    qunsetenv("ADE_CURSOR_LOG");
  }
  void persistedProviderRejectsWrongProtocolAndKey() {
    QTemporaryDir d;
    QString error;
    auto p = provider();
    QVERIFY2(ProviderStore::save(d.filePath("providers.json"), {p}, &error),
             qPrintable(error));
    const auto saved =
        ProviderStore::load(d.filePath("providers.json"), &error);
    QCOMPARE(saved.size(), 1);
    QVERIFY(saved[0].isCursorHarness());
    p.apiKey = "synthetic-not-forwarded";
    QVERIFY(!ProviderStore::save(d.filePath("providers.json"), {p}, &error));
    p.apiKey.clear();
    p.protocol = "opencode-acp";
    QVERIFY(!ProviderStore::save(d.filePath("providers.json"), {p}, &error));
    QCOMPARE(
        ProviderStore::load(d.filePath("providers.json"), &error)[0].protocol,
        QString("cursor-acp"));
  }
  void actualEngineRequiresExplicitConnectEvenAlwaysAllow() {
    QTemporaryDir d;
    const auto path = d.filePath("calls");
    qputenv("ADE_CURSOR_LOG", path.toUtf8());
    AgentEngine e;
    configure(e, d.path());
    e.setAlwaysAllow(true);
    QSignalSpy approval(&e, &AgentEngine::approvalRequired),
        done(&e, &AgentEngine::finished);
    QVERIFY(e.submitWithAttachments("Hello", {}));
    QTRY_COMPARE(approval.size(), 1);
    QCOMPARE(approval[0][1].toString(), QString("Connect Cursor account"));
    QCOMPARE(calls(path, "authenticate").size(), 0);
    e.approveTool(approval[0][0].toString(), true);
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(e.lastTurnSucceeded());
    const auto display = QJsonDocument(e.displayMessages()).toJson();
    QVERIFY(display.contains("Hello"));
    QVERIFY(display.contains("cursor.read"));
    QVERIFY(!display.contains("FOREIGN"));
  }
  void restartResumeAndEditedHistoryStartsFresh() {
    QTemporaryDir d;
    const auto path = d.filePath("calls");
    qputenv("ADE_CURSOR_LOG", path.toUtf8());
    QJsonArray history;
    {
      AgentEngine e;
      configure(e, d.path());
      connectOnly(e);
      QSignalSpy done(&e, &AgentEngine::finished);
      e.submit("first");
      QTRY_COMPARE(done.size(), 1);
      QVERIFY(e.lastTurnSucceeded());
      history = e.messages();
    }
    {
      AgentEngine e;
      configure(e, d.path());
      connectOnly(e);
      e.setMessages(history);
      QSignalSpy done(&e, &AgentEngine::finished);
      e.submit("second");
      QTRY_COMPARE(done.size(), 1);
      QVERIFY(e.lastTurnSucceeded());
      QCOMPARE(calls(path, "session/load").size(), 1);
      history = e.messages();
    }
    auto row = history[0].toObject();
    row["content"] = "edited prior context";
    history[0] = row;
    {
      AgentEngine e;
      configure(e, d.path());
      connectOnly(e);
      e.setMessages(history);
      QSignalSpy done(&e, &AgentEngine::finished);
      e.submit("third");
      QTRY_COMPARE(done.size(), 1);
      QVERIFY(e.lastTurnSucceeded());
      QCOMPARE(calls(path, "session/load").size(), 1);
      QCOMPARE(calls(path, "session/new").size(), 2);
    }
  }
  void permissionsAndPlansStayDistinct_data() {
    QTest::addColumn<QString>("scenario");
    QTest::newRow("permission") << QString("permission");
    QTest::newRow("plan") << QString("plan");
  }
  void permissionsAndPlansStayDistinct() {
    QFETCH(QString, scenario);
    QTemporaryDir d;
    qputenv("ADE_CURSOR_SCENARIO", scenario.toUtf8());
    AgentEngine e;
    configure(e, d.path());
    connectOnly(e);
    QSignalSpy approval(&e, &AgentEngine::approvalRequired),
        done(&e, &AgentEngine::finished);
    e.submit("work");
    QTRY_COMPARE(approval.size(), 2);
    QCOMPARE(approval[1][1].toString(), scenario == "plan"
                                            ? QString("Approve Cursor plan")
                                            : QString("cursor.read"));
    QVERIFY(e.isBusy());
    e.approveTool(approval[1][0].toString(), false);
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(e.lastTurnSucceeded());
  }
  void nativeQuestionValidatesAndCompletes() {
    QTemporaryDir d;
    qputenv("ADE_CURSOR_SCENARIO", "question");
    AgentEngine e;
    configure(e, d.path());
    e.setQuestionToolEnabled(true);
    connectOnly(e);
    QSignalSpy done(&e, &AgentEngine::finished);
    e.submit("question");
    QTRY_VERIFY(!e.pendingQuestion().isEmpty());
    const auto q = e.pendingQuestion();
    const auto token = q["id"].toString();
    QVERIFY(!e.submitQuestionStep(token, 0, {{"mode", "unknown"}}));
    QVERIFY(e.submitQuestionStep(token, 0, {{"mode", "a"}}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(e.lastTurnSucceeded());
    QVERIFY(e.pendingQuestion().isEmpty());
  }
  void ordinaryCursorToolCannotBecomeStagewiseWrapper() {
    QTemporaryDir d;
    qputenv("ADE_CURSOR_SCENARIO", "ordinary-input");
    AgentEngine e;
    configure(e, d.path());
    connectOnly(e);
    QSignalSpy done(&e, &AgentEngine::finished);
    e.submit("ordinary tool");
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(e.lastTurnSucceeded());
    bool found = false;
    for (const auto &r : e.displayMessages())
      for (const auto &v : r.toObject()["parts"].toArray()) {
        const auto p = v.toObject();
        if (p["name"] != "cursor.request_user_input")
          continue;
        found = true;
        QVERIFY(!p["acpPresentation"].toObject()["questionWrapper"].toBool());
      }
    QVERIFY(found);
  }
  void ownerDeleteAtQuestionDisplay() {
    QTemporaryDir d;
    qputenv("ADE_CURSOR_SCENARIO", "question");
    QPointer<AgentEngine> e = new AgentEngine;
    configure(*e, d.path());
    e->setQuestionToolEnabled(true);
    connectOnly(*e);
    connect(e, &AgentEngine::displayChanged, this, [&] {
      if (e && !e->pendingQuestion().isEmpty())
        delete e.data();
    });
    QVERIFY(e->submitWithAttachments("question", {}));
    QTRY_VERIFY(e.isNull());
  }
  void ownerDeleteAtConnect() {
    QTemporaryDir d;
    QPointer<AgentEngine> e = new AgentEngine;
    configure(*e, d.path());
    connect(e, &AgentEngine::approvalRequired, this,
            [&](const QString &, const QString &, const QJsonObject &) {
              delete e.data();
            });
    QVERIFY(e->submitWithAttachments("hello", {}));
    QTRY_VERIFY(e.isNull());
  }
  void cancellationRejectsStaleConnectionDecision() {
    QTemporaryDir d;
    AgentEngine e;
    configure(e, d.path());
    QSignalSpy approval(&e, &AgentEngine::approvalRequired),
        done(&e, &AgentEngine::finished);
    e.submit("one");
    QTRY_COMPARE(approval.size(), 1);
    const auto old = approval[0][0].toString();
    e.cancel();
    QTRY_COMPARE(done.size(), 1);
    e.submit("two");
    QTRY_COMPARE(approval.size(), 2);
    e.approveTool(old, true);
    QTest::qWait(30);
    QCOMPARE(done.size(), 1);
    e.approveTool(approval[1][0].toString(), true);
    QTRY_COMPARE(done.size(), 2);
    QVERIFY(e.lastTurnSucceeded());
  }
};
QTEST_GUILESS_MAIN(CursorEngineTest)
#include "test_cursor_engine.moc"
