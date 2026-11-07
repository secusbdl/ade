#include "attachments/AttachmentStore.h"
#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include "runtime/AgentToolHost.h"
#include "ui/AppStyle.h"
#include "ui/Composer.h"
#include "ui/ConversationView.h"
#include "ui/MainWindow.h"
#include "ui/QuestionPanel.h"
#include "ui/SessionStore.h"
#include "ui/SettingsPage.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
using namespace ade;
namespace {
ProviderConfig provider() {
  ProviderConfig p;
  p.id = "cursor";
  p.name = "Cursor fixture";
  p.kind = "cursor";
  p.protocol = "cursor-acp";
  p.model = "model-b";
  p.harnessExecutable = QDir(QCoreApplication::applicationDirPath())
                            .filePath("cursor_fixture.exe");
  return p;
}
bool seed(const QString &directory) {
  if (!ProviderStore::save(directory + "/providers.json", {provider()}))
    return false;
  const auto root = directory + "/workspace";
  if (!QDir().mkpath(root))
    return false;
  QList<Conversation> rows;
  for (const auto &id : {"first", "second"}) {
    Conversation c;
    c.id = id;
    c.title = id;
    c.titleLocked = true;
    c.providerId = "cursor";
    c.model = "model-b";
    c.workspace = root;
    rows.append(c);
  }
  return SessionStore(directory).save(rows);
}
bool select(MainWindow &w, const QString &id) {
  auto *list = w.findChild<QListWidget *>("agentList");
  if (!list)
    return false;
  for (int i = 0; i < list->count(); ++i)
    if (list->item(i)->data(Qt::UserRole) == id) {
      list->setCurrentRow(i);
      return w.currentSessionId() == id;
    }
  return false;
}
AgentEngine *engine(MainWindow &w, const QString &id = "first") {
  for (auto *h : w.findChildren<AgentToolHost *>())
    if (h->agentId() == id)
      return qobject_cast<AgentEngine *>(h->parent());
  return nullptr;
}
QJsonArray calls(const QString &path, const QString &method) {
  QFile f(path);
  QJsonArray rows;
  if (f.open(QIODevice::ReadOnly))
    for (const auto &line : f.readAll().split('\n')) {
      if (line.trimmed().isEmpty())
        continue;
      const auto o = QJsonDocument::fromJson(line).object();
      if (o["method"].toString() == method)
        rows.append(o);
    }
  return rows;
}
Conversation saved(const QString &directory) {
  for (const auto &row : SessionStore(directory).load())
    if (row.id == "first")
      return row;
  return {};
}
void send(MainWindow &w, const QString &text) {
  auto *c = w.findChild<Composer *>();
  auto *b = w.findChild<QToolButton *>("sendButton");
  QVERIFY(c && b);
  c->setPlainText(text);
  QVERIFY(b->isEnabled());
  b->click();
}
QPushButton *decision(MainWindow &w, const QString &name, bool allow = true) {
  auto *e = engine(w, w.currentSessionId());
  if (!e)
    return nullptr;
  QString partId;
  for (const auto &r : e->displayMessages())
    for (const auto &v : r.toObject()["parts"].toArray()) {
      const auto p = v.toObject();
      if (p["name"] == name && p["state"] == "approval-requested")
        partId = p["id"].toString();
    }
  if (partId.isEmpty())
    return nullptr;
  auto *view = w.findChild<ConversationView *>("conversationView");
  for (auto *widget : view->findChildren<QWidget *>())
    if (widget->property("partId") == partId) {
      auto *b =
          widget->findChild<QPushButton *>(allow ? "approveTool" : "denyTool");
      if (b && b->isVisible() && b->isEnabled())
        return b;
    }
  return nullptr;
}
void capture(MainWindow &w, const QString &name) {
  const auto directory = qEnvironmentVariable("ADE_CURSOR_RENDER_DIRECTORY");
  if (directory.isEmpty())
    return;
  QDir().mkpath(directory);
  for (int pass = 0; pass < 3; ++pass) {
    QCoreApplication::processEvents();
    QTest::qWait(30);
  }
  w.grab().save(directory + "/" + name + ".png");
}
} // namespace
class CursorHostTest : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    initializeAppStyle(*qApp);
    QVERIFY(QFileInfo::exists(provider().harnessExecutable));
  }
  void cleanup() {
    qunsetenv("ADE_CURSOR_SCENARIO");
    qunsetenv("ADE_CURSOR_LOG");
  }
  void connectControlsRemainUsable_data() {
    QTest::addColumn<int>("width");
    QTest::addColumn<QString>("theme");
    QTest::newRow("light-wide") << 1100 << QString("light");
    QTest::newRow("dark-wide") << 1100 << QString("dark");
    QTest::newRow("light-narrow") << 350 << QString("light");
    QTest::newRow("dark-narrow") << 350 << QString("dark");
  }
  void connectControlsRemainUsable() {
    QFETCH(int, width);
    QFETCH(QString, theme);
    QTemporaryDir d;
    QVERIFY(seed(d.path()));
    {
      SettingsPage settings(d.path());
      QVERIFY(settings.setPreference("personalization.appearance", theme));
    }
    MainWindow w(d.path());
    w.resize(width, 750);
    w.show();
    QVERIFY(select(w, "first"));
    send(w, "Connect explicitly");
    QTRY_VERIFY(decision(w, "Connect Cursor account"));
    auto *button = decision(w, "Connect Cursor account");
    QCOMPARE(w.palette().color(QPalette::Base).lightness() < 128,
             theme == "dark");
    const auto rect = QRect(button->mapTo(&w, QPoint()), button->size());
    QVERIFY(w.rect().contains(rect));
    capture(w, "connect-" + theme + "-" + QString::number(width));
    button->click();
    QTRY_VERIFY_WITH_TIMEOUT(engine(w) && !engine(w)->isBusy(), 10000);
    QVERIFY(engine(w)->lastTurnSucceeded());
  }
  void actualComposerConnectStreamAndRestart() {
    QTemporaryDir d;
    QVERIFY(seed(d.path()));
    const auto log = d.filePath("calls");
    qputenv("ADE_CURSOR_LOG", log.toUtf8());
    {
      MainWindow w(d.path());
      w.resize(1100, 750);
      w.show();
      QVERIFY(select(w, "first"));
      send(w, "FIRST_PROMPT");
      QTRY_VERIFY(decision(w, "Connect Cursor account"));
      QCOMPARE(calls(log, "authenticate").size(), 0);
      capture(w, "01-connect-light");
      decision(w, "Connect Cursor account")->click();
      QTRY_VERIFY_WITH_TIMEOUT(engine(w) && !engine(w)->isBusy(), 10000);
      QVERIFY(engine(w)->lastTurnSucceeded());
      QTRY_VERIFY(w.findChild<ConversationView *>("conversationView")
                      ->toPlainText()
                      .contains("Hello"));
      QVERIFY(
          QJsonDocument(saved(d.path()).messages).toJson().contains("cursor"));
      capture(w, "02-completed-light");
    }
    MainWindow w(d.path());
    w.show();
    QVERIFY(select(w, "first"));
    send(w, "SECOND_PROMPT");
    QTRY_VERIFY(decision(w, "Connect Cursor account"));
    decision(w, "Connect Cursor account")->click();
    QTRY_VERIFY_WITH_TIMEOUT(engine(w) && !engine(w)->isBusy(), 10000);
    QVERIFY(engine(w)->lastTurnSucceeded());
    QCOMPARE(calls(log, "session/new").size(), 1);
    QCOMPARE(calls(log, "session/load").size(), 1);
    QCOMPARE(calls(log, "session/prompt").size(), 2);
  }
  void realInlinePermissionAndPlan_data() {
    QTest::addColumn<QString>("scenario");
    QTest::newRow("permission") << QString("permission");
    QTest::newRow("plan") << QString("plan");
  }
  void realInlinePermissionAndPlan() {
    QFETCH(QString, scenario);
    QTemporaryDir d;
    QVERIFY(seed(d.path()));
    qputenv("ADE_CURSOR_SCENARIO", scenario.toUtf8());
    const auto log = d.filePath("calls");
    qputenv("ADE_CURSOR_LOG", log.toUtf8());
    MainWindow w(d.path());
    w.resize(1100, 750);
    w.show();
    QVERIFY(select(w, "first"));
    send(w, "Review operation");
    QTRY_VERIFY(decision(w, "Connect Cursor account"));
    decision(w, "Connect Cursor account")->click();
    const auto name = scenario == "plan" ? QString("Approve Cursor plan")
                                         : QString("cursor.read");
    QTRY_VERIFY(decision(w, name, false));
    QVERIFY(engine(w)->isBusy());
    capture(w, "03-" + scenario);
    decision(w, name, false)->click();
    QTRY_VERIFY_WITH_TIMEOUT(!engine(w)->isBusy(), 10000);
    QVERIFY(engine(w)->lastTurnSucceeded());
    const auto responses = calls(log, {});
    QVERIFY(!responses.isEmpty());
    const auto outcome =
        responses.last().toObject()["result"].toObject()["outcome"].toObject();
    QCOMPARE(outcome["outcome"].toString(),
             scenario == "plan" ? QString("rejected") : QString("selected"));
    if (scenario == "permission")
      QCOMPARE(outcome["optionId"].toString(), QString("reject-once"));
  }
  void questionSwitchAndActualSubmitStayOwned_data() {
    QTest::addColumn<bool>("stop");
    QTest::newRow("answer") << false;
    QTest::newRow("stop-partial") << true;
  }
  void questionSwitchAndActualSubmitStayOwned() {
    QFETCH(bool, stop);
    QTemporaryDir d;
    QVERIFY(seed(d.path()));
    qputenv("ADE_CURSOR_SCENARIO", "question");
    MainWindow w(d.path());
    w.resize(350, 750);
    w.show();
    QVERIFY(select(w, "first"));
    send(w, "Ask a question");
    QTRY_VERIFY(decision(w, "Connect Cursor account"));
    decision(w, "Connect Cursor account")->click();
    QTRY_VERIFY(engine(w) && !engine(w)->pendingQuestion().isEmpty());
    auto *first = engine(w);
    auto *panel = w.findChild<QuestionPanel *>();
    QVERIFY(panel);
    QTRY_VERIFY(panel->isVisible());
    const auto token = panel->token();
    QVERIFY(select(w, "second"));
    QTRY_VERIFY(!panel->isVisible());
    panel->submitStep("first", token, 0, {{"mode", "a"}});
    QVERIFY(!first->pendingQuestion().isEmpty());
    QVERIFY(select(w, "first"));
    QTRY_VERIFY(panel->isVisible());
    auto *radio = panel->findChild<QRadioButton *>("answer_mode_a");
    QVERIFY(radio);
    radio->click();
    capture(w, "04-question-narrow");
    if (stop) {
      auto *button = w.findChild<QToolButton *>("stopButton");
      QVERIFY(button && button->isEnabled());
      button->click();
    } else {
      auto *submit = panel->findChild<QPushButton *>("questionSubmit");
      QVERIFY(submit && submit->isEnabled());
      submit->click();
    }
    QTRY_VERIFY_WITH_TIMEOUT(!first->isBusy(), 10000);
    QCOMPARE(first->lastTurnSucceeded(), !stop);
    QVERIFY(first->pendingQuestion().isEmpty());
    const auto transcript = QJsonDocument(saved(d.path()).messages).toJson();
    QVERIFY(transcript.contains("askUserQuestions"));
    QVERIFY(transcript.contains("selectedOptionIds") ||
            transcript.contains("answers"));
    capture(w, stop ? "05-question-stopped" : "05-question-answered");
  }
  void pendingConnectStopRejectsStaleCard() {
    QTemporaryDir d;
    QVERIFY(seed(d.path()));
    const auto log = d.filePath("calls");
    qputenv("ADE_CURSOR_LOG", log.toUtf8());
    MainWindow w(d.path());
    w.show();
    QVERIFY(select(w, "first"));
    send(w, "Wait");
    QTRY_VERIFY(decision(w, "Connect Cursor account"));
    QPointer<QPushButton> stale = decision(w, "Connect Cursor account");
    auto *stop = w.findChild<QToolButton *>("stopButton");
    QVERIFY(stop && stop->isEnabled());
    stop->click();
    QTRY_VERIFY(!engine(w)->isBusy());
    if (stale)
      stale->click();
    QTest::qWait(30);
    QCOMPARE(calls(log, "authenticate").size(), 0);
    QVERIFY(!engine(w)->lastTurnSucceeded());
  }
  void actualHostOriginalImageAndHistoryReference() {
    QTemporaryDir d;
    const auto log = d.filePath("calls");
    qputenv("ADE_CURSOR_LOG", log.toUtf8());
    AgentEngine e;
    e.setProvider(provider());
    AgentToolHost host(&e, nullptr, "image-owner", d.path());
    QImage image(7, 5, QImage::Format_RGB32);
    image.fill(Qt::green);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(image.save(&buffer, "PNG"));
    AttachmentStore store(d.path());
    QString error;
    const auto ref = store.write("image-owner", "original.png", bytes, &error);
    QVERIFY2(!ref.isEmpty(), qPrintable(error));
    const auto metadata = host.captureAttachments({ref.toJson()}, &error);
    QVERIFY2(!metadata.isEmpty(), qPrintable(error));
    connect(&e, &AgentEngine::approvalRequired, &e,
            [&e](const QString &id, const QString &name, const QJsonObject &) {
              if (name == "Connect Cursor account")
                e.approveTool(id, true);
            });
    QSignalSpy done(&e, &AgentEngine::finished);
    QVERIFY(e.submitWithAttachments("See image",
                                    metadata["attachments"].toArray(),
                                    metadata["pathReferences"].toObject(),
                                    metadata["pathMetadata"].toObject()));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
    QVERIFY2(e.lastTurnSucceeded(),
             qPrintable(QJsonDocument(e.messages()).toJson()));
    const auto prompt = calls(log, "session/prompt");
    QCOMPARE(prompt.size(), 1);
    bool found = false;
    for (const auto &v :
         prompt[0].toObject()["params"].toObject()["prompt"].toArray()) {
      const auto p = v.toObject();
      if (p["type"] == "image") {
        found = true;
        QCOMPARE(p["mimeType"].toString(), QString("image/png"));
        QCOMPARE(QByteArray::fromBase64(p["data"].toString().toLatin1()),
                 bytes);
      }
    }
    QVERIFY(found);
    QCOMPARE(e.messages()[0].toObject()["metadata"].toObject()["attachments"],
             metadata["attachments"]);
    QVERIFY(!QJsonDocument(e.messages()).toJson().contains(bytes.toBase64()));
  }
};
QTEST_MAIN(CursorHostTest)
#include "test_cursor_host.moc"
