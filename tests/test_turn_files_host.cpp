#include "ui/MainWindow.h"
#include "ui/ConversationView.h"
#include "ui/TurnFileEdits.h"
#include "ui/FileChangesPanel.h"
#include "ui/GitDiffTab.h"
#include "ui/SideChatPane.h"
#include "ui/WorkspaceChangesController.h"
#include "ui/SessionStore.h"
#include "ui/AppStyle.h"
#include "core/AgentEngine.h"
#include "mutations/FileMutationStore.h"
#include "editor/EditorWidget.h"
#include "editor/CodeEditor.h"
#include <QApplication>
#include <QFile>
#include <QDir>
#include <QJsonObject>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUuid>
#include <QtTest>
using namespace ade;
namespace {
bool write(const QString& path,const QByteArray& bytes){QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();}
QByteArray read(const QString& path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};}
bool git(const QString& root,const QStringList& args){QProcess p;p.setWorkingDirectory(root);p.start("git",args);return p.waitForFinished(10000)&&p.exitCode()==0;}
QJsonArray history(const QString& mutation){return {QJsonObject{{"role","assistant"},{"content",""},{"tool_calls",QJsonArray{QJsonObject{{"id","same-model-id"},{"type","function"},{"function",QJsonObject{{"name","write"},{"arguments","{\"path\":\"file.txt\"}"}}}}}}},QJsonObject{{"role","tool"},{"tool_call_id","same-model-id"},{"content","Saved"},{"metadata",QJsonObject{{"mutations",QJsonArray{QJsonObject{{"mutationId",mutation},{"agentId","chat"},{"committed",true},{"recoveryRequired",false}}}}}}},QJsonObject{{"role","assistant"},{"content","Updated the file."}}};}
struct Fixture {
    QTemporaryDir data,workspace;QString error,invocationId;QJsonArray messages=history("selected");
    bool seed(int later=0){
        if(!write(workspace.filePath("file.txt"),"before\n")||!git(workspace.path(),{"init","--initial-branch=main"})||!git(workspace.path(),{"add","file.txt"})||!git(workspace.path(),{"-c","user.name=Fixture","-c","user.email=fixture@example.invalid","commit","-m","fixture"}))return false;
        mutations::FileMutationStore store(data.path(),"chat");if(!store.open(&error)||!store.bindMount("ws1",workspace.path(),true,&error))return false;
        mutations::Request request;request.agentId="chat";request.nonce="selected";request.invocationId=invocationId;request.toolCallId="same-model-id";request.tool="write";request.path="ws1/file.txt";request.content="saved\n";request.raw=true;if(!store.execute(request).success)return false;
        for(int i=0;i<later;++i){request.nonce="later-"+QString::number(i);request.path="ws1/other.txt";request.content=QString::number(i);if(!store.execute(request).success)return false;}
        Conversation chat;chat.id="chat";chat.title="Saved changes fixture";chat.titleLocked=true;chat.workspace=workspace.path();chat.messages=messages;return SessionStore(data.path()).save({chat},&error);
    }
    QString rowId()const{return AgentEngine::displayHistory(messages).last().toObject()["id"].toString();}
};
bool loaded(ConversationView* view){for(auto* label:view->findChildren<QLabel*>("turnFilesTitle"))if(label->text()=="Changed files 1")return true;return false;}
void expose(MainWindow& window){window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1000,740);window.show();}
}
class TurnFilesHostTest:public QObject {
 Q_OBJECT
private slots:
 void interruptedSavedDispatchHydratesOnOpenAndPersistsRecoveredResult_data(){
    QTest::addColumn<bool>("external");QTest::newRow("direct-unsettled")<<false;QTest::newRow("sandbox-settled-without-outer-result")<<true;
 }
 void interruptedSavedDispatchHydratesOnOpenAndPersistsRecoveredResult(){
    QFETCH(bool,external);
    Fixture f;f.invocationId=QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto assistant=f.messages.first().toObject();
    assistant["metadata"]=QJsonObject{{"display",QJsonObject{{"version",1},{"id","interrupted-batch"}}},
        {"mutationInvocations",QJsonArray{QJsonObject{{"invocationId",f.invocationId},{"toolCallId","same-model-id"},{"agentId","chat"},{"external",external},{"settled",external}}}}};
    f.messages={assistant,QJsonObject{{"role","tool"},{"tool_call_id","same-model-id"},{"content","Action cancelled by the user."}}};
    if(external)f.messages={assistant};
    QVERIFY2(f.seed(),qPrintable(f.error));
    QJsonArray recovered;
    {
        MainWindow window(f.data.path());expose(window);auto* view=window.findChild<ConversationView*>();QVERIFY(view);
        QTRY_VERIFY(loaded(view));
        const auto chats=SessionStore(f.data.path()).load();QCOMPARE(chats.size(),1);recovered=chats.first().messages;
        QCOMPARE(recovered.size(),2);const auto result=recovered.last().toObject();
        QCOMPARE(result["metadata"].toObject()["mutations"].toArray().size(),1);
        QCOMPARE(result["metadata"].toObject()["mutations"].toArray().first().toObject()["mutationId"].toString(),QString("selected"));
        QVERIFY(!result["content"].toString().contains("cancelled"));
        QCOMPARE(read(f.workspace.filePath("file.txt")),QByteArray("saved\n"));
        for(auto* engine:window.findChildren<AgentEngine*>())QVERIFY(!engine->isBusy());
    }
    MainWindow reopened(f.data.path());expose(reopened);QTRY_VERIFY(loaded(reopened.findChild<ConversationView*>()));
    QCOMPARE(SessionStore(f.data.path()).load().first().messages,recovered);
 }
 void savedHistoryLoadsTreeAndDefaultFileOpensCurrentGitDiff(){Fixture f;QVERIFY2(f.seed(),qPrintable(f.error));MainWindow window(f.data.path());expose(window);auto* view=window.findChild<ConversationView*>();QVERIFY(view);QTRY_VERIFY(loaded(view));QVERIFY(write(f.workspace.filePath("file.txt"),"current working tree\n"));emit view->turnFileRequested("chat",f.rowId(),{"selected"},"ws1/file.txt");QTRY_VERIFY_WITH_TIMEOUT(window.findChild<GitDiffTab*>(),15000);QVERIFY(!window.findChild<QWidget*>("savedTurnDiff"));}
 void headerOpensCurrentSidebarWithoutUndoPanel(){Fixture f;QVERIFY(f.seed());MainWindow window(f.data.path());expose(window);auto* view=window.findChild<ConversationView*>();QTRY_VERIFY(loaded(view));emit view->turnFilesReviewRequested("chat",f.rowId(),{"selected"});QTRY_VERIFY(window.findChild<QWidget*>("workspaceChangesView") && window.findChild<QWidget*>("workspaceChangesView")->isVisible());QVERIFY(!window.findChild<FileChangesPanel*>());}
 void explicitSavedDiffUsesOriginalBytesAfterOutsideEdit(){Fixture f;QVERIFY(f.seed());MainWindow window(f.data.path());expose(window);auto* view=window.findChild<ConversationView*>();QTRY_VERIFY(loaded(view));QVERIFY(write(f.workspace.filePath("file.txt"),"outside edit\n"));emit view->savedTurnFileRequested("chat",f.rowId(),{"selected"},"ws1/file.txt");QTRY_VERIFY(window.findChild<QWidget*>("savedTurnDiff"));auto* page=window.findChild<QWidget*>("savedTurnDiff");QString text;for(auto* code:page->findChildren<QPlainTextEdit*>("fileDiffText"))text+=code->toPlainText();QVERIFY(text.contains("before"));QVERIFY(text.contains("saved"));QVERIFY(!text.contains("outside edit"));QCOMPARE(read(f.workspace.filePath("file.txt")),QByteArray("outside edit\n"));}
 void staleOrForeignSignalsDoNotOpenTabs(){Fixture f;QVERIFY(f.seed());MainWindow window(f.data.path());expose(window);auto* view=window.findChild<ConversationView*>();QTRY_VERIFY(loaded(view));const auto before=window.findChild<QTabWidget*>()->count();emit view->savedTurnFileRequested("other",f.rowId(),{"selected"},"ws1/file.txt");emit view->savedTurnFileRequested("chat",f.rowId(),{"missing"},"ws1/file.txt");emit view->savedTurnFileRequested("chat","retired-row",{"selected"},"ws1/file.txt");QTest::qWait(80);QCOMPARE(window.findChild<QTabWidget*>()->count(),before);}
 void olderThanTwoHundredMutationsCanReviewAndUndo(){Fixture f;QVERIFY2(f.seed(201),qPrintable(f.error));MainWindow window(f.data.path());expose(window);auto* view=window.findChild<ConversationView*>();QTRY_VERIFY(loaded(view));emit view->turnUndoReviewRequested("chat",f.rowId(),{"selected"});QTRY_VERIFY(window.findChild<FileChangesPanel*>());auto* panel=window.findChild<FileChangesPanel*>();QTRY_COMPARE(panel->selectedMutations(),QStringList{"selected"});QStringList paths;QString error;QVERIFY(panel->pathsForMutations({"selected"},&paths,&error));QCOMPARE(paths,QStringList{f.workspace.filePath("file.txt")});panel->findChild<QPushButton*>("undoSelectedMutations")->click();QTRY_COMPARE(read(f.workspace.filePath("file.txt")),QByteArray("before\n"));}
 void originalWorkspaceDirtyEditorBlocksUndoAfterReconnect(){Fixture f;QTemporaryDir second;QVERIFY(f.seed());MainWindow window(f.data.path());expose(window);auto* view=window.findChild<ConversationView*>();QTRY_VERIFY(loaded(view));auto* tabs=window.findChild<QTabWidget*>();auto* editor=new EditorWidget(tabs);QString error;QVERIFY(editor->openFile(f.workspace.filePath("file.txt"),&error));tabs->addTab(editor,"Dirty original file");editor->editor()->insertPlainText("unsaved ");QVERIFY(editor->isModified());window.connectWorkspace(second.path());emit view->turnUndoReviewRequested("chat",f.rowId(),{"selected"});QTRY_VERIFY(window.findChild<FileChangesPanel*>());auto* panel=window.findChild<FileChangesPanel*>();QTRY_COMPARE(panel->selectedMutations(),QStringList{"selected"});panel->findChild<QPushButton*>("undoSelectedMutations")->click();QTRY_VERIFY(panel->findChild<QLabel*>("mutationStatus")->text().contains("unsaved"));QCOMPARE(read(f.workspace.filePath("file.txt")),QByteArray("saved\n"));editor->requestClose(EditorWidget::CloseChoice::Discard);}
 void retainedPanelPathsFailClosedForMissingIds(){FileChangesPanel panel;panel.setHistory("chat",QJsonArray{QJsonObject{{"mutationId","one"},{"state","committed"},{"entries",QJsonArray{QJsonObject{{"path","ws1/a.txt"},{"absolutePath","D:/fixture/a.txt"}}}}}},false);QStringList paths;QString error;QVERIFY(panel.pathsForMutations({"one"},&paths,&error));QCOMPARE(paths,QStringList{"D:/fixture/a.txt"});QVERIFY(!panel.pathsForMutations({"one","missing"},&paths,&error));QVERIFY(paths.isEmpty());QVERIFY(!error.isEmpty());panel.setHistory("chat",QJsonArray{QJsonObject{{"mutationId","one"},{"state","committed"},{"entries",QJsonArray{QJsonObject{{"path","ws1/a.txt"}}}}}},false);QVERIFY(!panel.pathsForMutations({"one"},&paths,&error));}
 void sideChatDifferentWorkspaceOpensItsOwnCurrentDiff(){
    Fixture f;QTemporaryDir parentWorkspace;QVERIFY(f.seed());SessionStore store(f.data.path());auto chats=store.load();QCOMPARE(chats.size(),1);auto child=chats.first();child.sideChatParentId="parent";Conversation parent;parent.id="parent";parent.title="Parent workspace";parent.titleLocked=true;parent.workspace=parentWorkspace.path();QVERIFY(store.save({parent,child}));
    MainWindow window(f.data.path());expose(window);QCOMPARE(window.currentSessionId(),QString("parent"));QTRY_VERIFY(window.findChild<SideChatPane*>());auto* pane=window.findChild<SideChatPane*>();QCOMPARE(pane->chatId(),QString("chat"));auto* tabs=window.findChild<QTabWidget*>();tabs->setCurrentWidget(pane);auto* view=pane->transcript();QTRY_VERIFY(loaded(view));
    emit view->turnFilesReviewRequested("chat",f.rowId(),{"selected"});auto* controller=window.findChild<WorkspaceChangesController*>();QVERIFY(controller);QTRY_COMPARE(QDir::cleanPath(controller->workspace()),QDir::cleanPath(f.workspace.path()));
    tabs->setCurrentWidget(pane);emit view->turnFileRequested("chat",f.rowId(),{"selected"},"ws1/file.txt");QTRY_VERIFY(window.findChild<GitDiffTab*>());auto* diff=window.findChild<GitDiffTab*>();QTRY_VERIFY(diff->ready());QCOMPARE(QDir::cleanPath(diff->workspace()),QDir::cleanPath(f.workspace.path()));QCOMPARE(diff->path(),QString("file.txt"));QCOMPARE(window.currentSessionId(),QString("parent"));
    QTRY_COMPARE(QDir::cleanPath(controller->workspace()),QDir::cleanPath(f.workspace.path()));const auto persisted=SessionStore(f.data.path()).load();for(const auto& chat:persisted)if(chat.id=="parent")QCOMPARE(chat.workspace,parentWorkspace.path());
 }
};
int main(int argc,char** argv){QApplication app(argc,argv);initializeAppStyle(app);TurnFilesHostTest test;return QTest::qExec(&test,argc,argv);}
#include "test_turn_files_host.moc"
