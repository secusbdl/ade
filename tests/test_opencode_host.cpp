#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include "harness/HarnessWorkspace.h"
#include "runtime/AgentToolHost.h"
#include "ui/AppStyle.h"
#include "ui/Composer.h"
#include "ui/MainWindow.h"
#include "ui/QuestionPanel.h"
#include "ui/SessionStore.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
using namespace ade;
namespace {
ProviderConfig provider() {
  ProviderConfig p;
  p.id = "opencode";
  p.name = "OpenCode fixture";
  p.kind = "opencode";
  p.protocol = "opencode-acp";
  p.model = "model-b";
  p.harnessExecutable =
      QDir(QCoreApplication::applicationDirPath()).filePath("acp_fixture.exe");
  return p;
}
bool seed(const QString &directory, const QString &workspace = {}) {
  if (!ProviderStore::save(directory + "/providers.json", {provider()}))
    return false;
  QList<Conversation> chats;
  for (const auto &id : {"first", "second"}) {
    Conversation chat;
    chat.id = id;
    chat.title = id;
    chat.titleLocked = true;
    chat.providerId = "opencode";
    chat.model = "model-b";
    chat.workspace = workspace;
    chats.append(chat);
  }
  return SessionStore(directory).save(chats);
}
bool select(MainWindow &window, const QString &id) {
  auto *list = window.findChild<QListWidget *>("agentList");
  if (!list)
    return false;
  for (int i = 0; i < list->count(); ++i)
    if (list->item(i)->data(Qt::UserRole).toString() == id) {
      list->setCurrentItem(list->item(i));
      return window.currentSessionId() == id;
    }
  return false;
}
AgentEngine *engine(MainWindow &window, const QString &id) {
  for (auto *host : window.findChildren<AgentToolHost *>())
    if (host->agentId() == id)
      return qobject_cast<AgentEngine *>(host->parent());
  return nullptr;
}
Conversation saved(const QString &directory, const QString &id) {
  for (const auto &chat : SessionStore(directory).load())
    if (chat.id == id)
      return chat;
  return {};
}
QJsonArray requests(const QString &path, const QString &method) {
  QJsonArray rows;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return rows;
  for (const auto &line : file.readAll().split('\n')) {
    const auto row = QJsonDocument::fromJson(line).object();
    if (row["method"].toString() == method)
      rows.append(row["params"]);
  }
  return rows;
}
QJsonObject mapping(const Conversation &chat) {
  for (qsizetype i = chat.messages.size(); i-- > 0;) {
    const auto state = chat.messages[i]
                           .toObject()["metadata"]
                           .toObject()["harness"]
                           .toObject();
    if (!state.isEmpty())
      return state;
  }
  return {};
}
void typeAndClick(MainWindow &window, const QString &text) {
  auto *composer = window.findChild<Composer *>();
  auto *send = window.findChild<QToolButton *>("sendButton");
  QVERIFY(composer);
  QVERIFY(send);
  composer->setPlainText(text);
  QVERIFY(send->isEnabled());
  send->click();
}
} // namespace
class OpenCodeHostTest : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    initializeAppStyle(*qApp);
    QVERIFY(QFileInfo::exists(provider().harnessExecutable));
  }
  void cleanup() {
    qunsetenv("ADE_ACP_SCENARIO");
    qunsetenv("ADE_ACP_LOG");
  }
  void emptyCredentialsSendAndReopenUseOwnedWorkspace() {
    QTemporaryDir dir;
    const auto workspace = dir.filePath("workspace");
    QVERIFY(QDir().mkpath(workspace));
    const auto expected = HarnessWorkspace::resolve(workspace);
    QVERIFY(!expected.path.isEmpty());
    QVERIFY(seed(dir.path(), workspace));
    const auto log = dir.filePath("requests.jsonl");
    qputenv("ADE_ACP_LOG", log.toUtf8());
    QJsonArray original;
    {
      MainWindow window(dir.path());
      window.show();
      QVERIFY(select(window, "first"));
      typeAndClick(window, "FIRST_CURRENT");
      QTRY_VERIFY(engine(window, "first"));
      QTRY_VERIFY_WITH_TIMEOUT(!engine(window, "first")->isBusy(), 10000);
      QVERIFY(engine(window, "first")->lastTurnSucceeded());
      const auto chat = saved(dir.path(), "first");
      original = chat.messages;
      QCOMPARE(mapping(chat)["ownerId"].toString(), QString("first"));
      QCOMPARE(mapping(chat)["workspace"].toString(), expected.path);
      QCOMPARE(mapping(chat)["workspaceIdentity"].toString(),
               expected.identity);
      QVERIFY(QJsonDocument(chat.messages).toJson().contains("ACP_OK"));
      QCOMPARE(requests(log, "session/new").size(), 1);
      QCOMPARE(
          requests(log, "session/new").first().toObject()["cwd"].toString(),
          expected.path);
    }
    MainWindow restarted(dir.path());
    restarted.show();
    QVERIFY(select(restarted, "first"));
    QCOMPARE(saved(dir.path(), "first").messages, original);
    typeAndClick(restarted, "SECOND_CURRENT");
    QTRY_VERIFY(engine(restarted, "first"));
    QTRY_VERIFY_WITH_TIMEOUT(!engine(restarted, "first")->isBusy(), 10000);
    QVERIFY(engine(restarted, "first")->lastTurnSucceeded());
    QCOMPARE(requests(log, "session/new").size(), 1);
    QCOMPARE(requests(log, "session/resume").size(), 1);
    const auto turns = requests(log, "session/prompt");
    QCOMPARE(turns.size(), 2);
    const auto second = QJsonDocument(turns.last().toObject()).toJson();
    QVERIFY(second.contains("SECOND_CURRENT"));
    QVERIFY(!second.contains("FIRST_CURRENT"));
    const auto providers = ProviderStore::load(dir.filePath("providers.json"));
    QCOMPARE(providers.size(), 1);
    QVERIFY(providers[0].apiKey.isEmpty());
    QVERIFY(providers[0].baseUrl.isEmpty());
  }
  void busyQueueStopReopenAndExplicitResume() {
    QTemporaryDir dir;
    QVERIFY(seed(dir.path()));
    const auto log = dir.filePath("requests.jsonl");
    qputenv("ADE_ACP_LOG", log.toUtf8());
    qputenv("ADE_ACP_SCENARIO", "hold");
    {
      MainWindow window(dir.path());
      window.show();
      QVERIFY(select(window, "first"));
      typeAndClick(window, "HOLD_CURRENT");
      QTRY_COMPARE_WITH_TIMEOUT(requests(log, "session/prompt").size(), 1,
                                10000);
      QVERIFY(engine(window, "first"));
      QVERIFY(engine(window, "first")->isBusy());
      typeAndClick(window, "QUEUED_CURRENT");
      QCOMPARE(saved(dir.path(), "first").queuedMessages.size(), 1);
      auto *stop = window.findChild<QToolButton *>("stopButton");
      QVERIFY(stop);
      QVERIFY(stop->isEnabled());
      stop->click();
      QTRY_VERIFY(!engine(window, "first")->isBusy());
      QVERIFY(!engine(window, "first")->lastTurnSucceeded());
      QCOMPARE(saved(dir.path(), "first").queuedMessages.size(), 1);
    }
    qunsetenv("ADE_ACP_SCENARIO");
    MainWindow restored(dir.path());
    restored.show();
    QVERIFY(select(restored, "first"));
    QCoreApplication::processEvents();
    QCOMPARE(requests(log, "session/prompt").size(), 1);
    auto *resume = restored.findChild<QPushButton *>("queueResume");
    QVERIFY(resume);
    QVERIFY(resume->isVisible());
    resume->click();
    QTRY_COMPARE_WITH_TIMEOUT(requests(log, "session/prompt").size(), 2, 10000);
    QTRY_VERIFY(!engine(restored, "first")->isBusy());
    QVERIFY(engine(restored, "first")->lastTurnSucceeded());
    QVERIFY(saved(dir.path(), "first").queuedMessages.isEmpty());
    QVERIFY(QJsonDocument(requests(log, "session/prompt").last().toObject())
                .toJson()
                .contains("QUEUED_CURRENT"));
  }
  void elicitationUsesOwnedNativeQuestionPanel() {
    QTemporaryDir dir;
    QVERIFY(seed(dir.path()));
    const auto log = dir.filePath("requests");
    qputenv("ADE_ACP_LOG", log.toUtf8());
    qputenv("ADE_ACP_SCENARIO", "question");
    MainWindow window(dir.path());
    window.show();
    QVERIFY(select(window, "first"));
    typeAndClick(window, "Ask native form");
    QTRY_VERIFY(engine(window, "first"));
    auto *first = engine(window, "first");
    QTRY_VERIFY(!first->pendingQuestion().isEmpty());
    const auto token = first->pendingQuestion()["id"].toString();
    auto *panel = window.findChild<QuestionPanel *>();
    QVERIFY(panel);
    QTRY_VERIFY(panel->isVisible());
    QVERIFY(select(window, "second"));
    panel->submitStep("first", token, 0, {{"name", "wrong"}});
    QCOMPARE(first->pendingQuestion()["id"].toString(), token);
    QVERIFY(select(window, "first"));
    QTRY_VERIFY(panel->isVisible());
    auto *answer = panel->findChild<QLineEdit *>("answer_name");
    auto *submit = panel->findChild<QPushButton *>("questionSubmit");
    QVERIFY(answer && submit);
    answer->setText("NativeOK");
    submit->click();
    QTRY_VERIFY(!first->isBusy());
    QVERIFY(first->lastTurnSucceeded());
    QVERIFY(first->pendingQuestion().isEmpty());
    QFile file(log);
    QVERIFY(file.open(QIODevice::ReadOnly));
    bool accepted = false;
    for (const auto &line : file.readAll().split('\n')) {
      const auto row = QJsonDocument::fromJson(line).object();
      if (row["id"] == "question-1")
        accepted = row["result"].toObject()["action"] == "accept" &&
                   row["result"].toObject()["content"].toObject()["name"] ==
                       "NativeOK";
    }
    QVERIFY(accepted);
    QCOMPARE(mapping(saved(dir.path(), "first"))["ownerId"].toString(),
             QString("first"));
  }
  void noWorkspaceFallbackIsBoundToEachOwner() {
    QTemporaryDir dir;
    QVERIFY(seed(dir.path()));
    MainWindow window(dir.path());
    window.show();
    QVERIFY(select(window, "first"));
    typeAndClick(window, "FIRST_CHAT");
    QTRY_VERIFY(engine(window, "first"));
    QTRY_VERIFY_WITH_TIMEOUT(!engine(window, "first")->isBusy(), 10000);
    QVERIFY(engine(window, "first")->lastTurnSucceeded());
    const auto first = mapping(saved(dir.path(), "first"));
    QVERIFY(select(window, "second"));
    typeAndClick(window, "SECOND_CHAT");
    QTRY_VERIFY(engine(window, "second"));
    QTRY_VERIFY_WITH_TIMEOUT(!engine(window, "second")->isBusy(), 10000);
    QVERIFY(engine(window, "second")->lastTurnSucceeded());
    const auto second = mapping(saved(dir.path(), "second"));
    QCOMPARE(first["ownerId"].toString(), QString("first"));
    QCOMPARE(second["ownerId"].toString(), QString("second"));
    QVERIFY(first["workspace"] != second["workspace"]);
    const auto accepted =
        HarnessWorkspace::resolve(dir.filePath("harness-workspaces")).path +
        "/";
    QVERIFY(first["workspace"].toString().startsWith(accepted,
                                                     Qt::CaseInsensitive));
    QVERIFY(second["workspace"].toString().startsWith(accepted,
                                                      Qt::CaseInsensitive));
    QVERIFY(saved(dir.path(), "first").workspace.isEmpty());
    QVERIFY(saved(dir.path(), "second").workspace.isEmpty());
  }
};
QTEST_MAIN(OpenCodeHostTest)
#include "test_opencode_host.moc"
