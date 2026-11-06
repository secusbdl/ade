#include "browser/BrowserInspector.h"
#include "core/AgentEngine.h"
#include "core/QueuedMessages.h"
#include "editor/EditorWidget.h"
#include "editor/CodeEditor.h"
#include "mutations/FileMutationJournal.h"
#include "runtime/AgentToolHost.h"
#include "ui/Composer.h"
#include "ui/FileChangesPanel.h"
#include "ui/MainWindow.h"
#include "ui/SessionStore.h"
#include <QAction>
#include <QBuffer>
#include <QFile>
#include <QJsonDocument>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QLabel>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QtTest>
using namespace ade;
namespace {
bool seed(const QString &dir, const QString &workspace = {}) {
  QList<Conversation> chats;
  for (const auto &id : {"first", "second"}) {
    Conversation c;
    c.id = id;
    c.title = id;
    c.titleLocked = true;
    c.workspace = workspace;
    chats.append(c);
  }
  return SessionStore(dir).save(chats);
}
bool select(MainWindow &w, const QString &id) {
  auto *list = w.findChild<QListWidget *>("agentList");
  for (int i = 0; i < list->count(); ++i)
    if (list->item(i)->data(Qt::UserRole) == id) {
      list->setCurrentRow(i);
      return true;
    }
  return false;
}
Conversation saved(const QString &dir, const QString &id) {
  for (const auto &c : SessionStore(dir).load())
    if (c.id == id)
      return c;
  return {};
}
QByteArray read(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
bool showChanges(MainWindow &w) {
  for (auto *action : w.findChildren<QAction *>())
    if (action->text() == "File changes and undo") {
      action->trigger();
      return true;
    }
  return false;
}
} // namespace
class ReferenceControlsTest : public QObject {
  Q_OBJECT
private slots:
  void mutationSelectionNeverCrossesChats() {
    FileChangesPanel panel;
    const QJsonArray rows{
        QJsonObject{
            {"mutationId", "a"}, {"tool", "write"}, {"state", "committed"}},
        QJsonObject{
            {"mutationId", "b"}, {"tool", "write"}, {"state", "rolled_back"}}};
    panel.setHistory("first", rows, false);
    auto *tree = panel.findChild<QTreeWidget *>("mutationHistory");
    auto *undo = panel.findChild<QPushButton *>("undoSelectedMutations");
    QSignalSpy calls(&panel, &FileChangesPanel::undoRequested);
    tree->topLevelItem(0)->setSelected(true);
    tree->topLevelItem(1)->setSelected(true);
    QVERIFY(undo->isEnabled());
    undo->click();
    QCOMPARE(calls.size(), 1);
    QCOMPARE(calls[0][0].toString(), "first");
    QCOMPARE(calls[0][1].toStringList(), QStringList{"a"});
    panel.setHistory("second", rows, false);
    QVERIFY(panel.selectedMutations().isEmpty());
    QVERIFY(!undo->isEnabled());
    tree->topLevelItem(0)->setSelected(true);
    panel.setOperation("first", true, "Stale");
    QVERIFY(undo->isEnabled());
    panel.setOperation("second", true, "Undoing");
    QVERIFY(!undo->isEnabled());
  }
  void committedCleanupIsRecoverableBeforeUndo() {
    FileChangesPanel panel;
    QJsonObject row{{"mutationId","committed-cleanup"},{"tool","write"},{"state","committed"},{"recoveryRequired",true}};
    panel.setHistory("chat",{row},false);
    auto* tree=panel.findChild<QTreeWidget*>("mutationHistory");
    auto* undo=panel.findChild<QPushButton*>("undoSelectedMutations");
    auto* recover=panel.findChild<QPushButton*>("recoverInterruptedMutations");
    tree->topLevelItem(0)->setSelected(true);
    QVERIFY(!undo->isEnabled());QVERIFY(!recover->isHidden());
    QSignalSpy requested(&panel,&FileChangesPanel::recoveryRequested);recover->click();
    QCOMPARE(requested.size(),1);QCOMPARE(requested[0][0].toString(),QString("chat"));
    row["recoveryRequired"]=false;panel.setHistory("chat",{row},false);tree->topLevelItem(0)->setSelected(true);
    QVERIFY(undo->isEnabled());QVERIFY(recover->isHidden());
  }
  void browserCapturesPersistOnlyToTheirOriginalActiveChat() {
    QTemporaryDir data;
    QVERIFY(seed(data.path()));
    MainWindow w(data.path());
    QVERIFY(select(w, "first"));
    w.findChild<QToolButton *>("openBrowserButton")->click();
    auto *inspector = w.findChild<BrowserInspector *>();
    QVERIFY(inspector);
    emit inspector->elementSelected(
        {{"tagName", "button"}, {"innerText", "Fixture"}}, "first");
    auto first = saved(data.path(), "first");
    QCOMPARE(first.draftAttachments.size(), 1);
    QString error;
    const auto ref =
        AttachmentRef::fromJson(first.draftAttachments[0].toObject(), &error);
    const auto bytes =
        AttachmentStore(data.path()).read("first", ref, {}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(QJsonDocument::fromJson(bytes).object()["innerText"].toString(),
             "Fixture");
    QImage image(16, 16, QImage::Format_ARGB32);
    image.fill(Qt::green);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    QVERIFY(image.save(&buffer, "PNG"));
    emit inspector->screenshotCaptured(png, "first");
    QCOMPARE(saved(data.path(), "first").draftAttachments.size(), 2);
    QVERIFY(select(w, "second"));
    emit inspector->screenshotCaptured(png, "first");
    QCOMPARE(saved(data.path(), "first").draftAttachments.size(), 2);
    QVERIFY(saved(data.path(), "second").draftAttachments.isEmpty());
  }
  void persistedMutationCanBeReviewedAndUndoneFromWindow() {
    QTemporaryDir data, workspace;
    QVERIFY(seed(data.path(), workspace.path()));
    const auto path = workspace.filePath("file.txt");
    {
      QFile f(path);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("before");
    }
    {
      FileMutationJournal journal(data.path(), "first");
      QString error;
      QVERIFY(journal.bindMount("ws1", workspace.path(), true, &error));
      const auto result =
          journal.executor()({"fixture",
                              "write",
                              workspace.path(),
                              {},
                              {{"path", "file.txt"}, {"content", "after"}}},
                             nullptr);
      QVERIFY2(!result.error, qPrintable(result.output));
    }
    QCOMPARE(read(path), QByteArray("after"));
    MainWindow w(data.path());
    QVERIFY(select(w, "first"));
    QVERIFY(showChanges(w));
    auto *panel = w.findChild<FileChangesPanel *>();
    QVERIFY(panel);
    auto *tree = panel->findChild<QTreeWidget *>("mutationHistory");
    QCOMPARE(tree->topLevelItemCount(), 1);
    tree->topLevelItem(0)->setSelected(true);
    auto *undo = panel->findChild<QPushButton *>("undoSelectedMutations");
    QVERIFY(undo->isEnabled());
    auto *host = w.findChild<AgentToolHost *>();
    QVERIFY(host);
    auto* engine=w.findChild<AgentEngine*>();QVERIFY(engine);QVERIFY(engine->setQueuedMessages({QueuedMessages::create("Later")}));QVERIFY(!undo->isEnabled());QVERIFY(engine->setQueuedMessages({}));QVERIFY(undo->isEnabled());
    QSignalSpy completed(host, &AgentToolHost::mutationUndoCompleted);
    auto* tabs=w.findChild<QTabWidget*>("contentTabs");auto* editor=new EditorWidget(tabs);QString issue;QVERIFY(editor->openFile(path,&issue));tabs->addTab(editor,"Unsaved file");editor->editor()->insertPlainText("unsaved ");QVERIFY(editor->isModified());undo->click();QCOMPARE(completed.size(),0);QCOMPARE(read(path),QByteArray("after"));QVERIFY(panel->findChild<QLabel*>("mutationStatus")->text().contains("unsaved"));tabs->removeTab(tabs->indexOf(editor));delete editor;
    undo->click();
    QVERIFY(!undo->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 10000);
    QVERIFY2(!completed[0][2].toBool(), qPrintable(completed[0][1].toString()));
    QCOMPARE(read(path), QByteArray("before"));
  }
};
QTEST_MAIN(ReferenceControlsTest)
#include "test_reference_controls.moc"
