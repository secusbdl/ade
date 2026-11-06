#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QToolButton>
#include "ui/AppFonts.h"
#include "ui/Composer.h"
#include "ui/MainWindow.h"
#include "ui/SessionStore.h"
#include "ui/SettingsPage.h"
#include "attachments/AttachmentPipeline.h"
#include "attachments/FileReadSnapshotStore.h"
#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include "runtime/AgentToolHost.h"
#include "git/GitPanel.h"
#include "git/ProcessRunner.h"

using namespace ade;
namespace {
bool put(const QString& path,const QByteArray& bytes) {QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.flush();}
QByteArray read(const QString& path) {QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray();}
void deferredDeletes() {QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);}
Conversation chat(const QString& id,bool archived=false) {
    Conversation value;value.id=id;value.title="Title "+id;value.created=value.updated=1000;value.archived=archived;
    value.titleLocked=true;value.providerId="fixture";value.model="fixture-model";return value;
}
struct Fixture {
    QTemporaryDir base;QString data=base.filePath("data");
    Fixture(){QDir().mkpath(data);}
    bool seed(const QList<Conversation>& values){return SessionStore(data).save(values);}
    QList<Conversation> sessions()const{return SessionStore(data).load();}
    Conversation stored(const QString& id)const{for(const auto& value:sessions())if(value.id==id)return value;return {};}
    QByteArray sessionBytes()const{return read(data+"/sessions.json");}
    bool externalChange(){SessionStore other(data);auto values=other.load();if(values.isEmpty())return false;values[0].title="External writer title";return other.save(values);}
    bool provider(quint16 port){ProviderConfig p;p.id="fixture";p.name="Fixture";p.model="fixture-model";p.protocol="openai";p.apiKey="test-only-not-a-secret";p.baseUrl=QString("http://127.0.0.1:%1/v1").arg(port);return ProviderStore::save(data+"/providers.json",{p});}
};
class HeldProvider:public QObject {
public:
    QTcpServer server;int requests=0;
    HeldProvider(){connect(&server,&QTcpServer::newConnection,this,[this]{while(auto* socket=server.nextPendingConnection()){socket->setParent(this);connect(socket,&QTcpSocket::readyRead,this,[this,socket]{if(!socket->property("counted").toBool()){socket->setProperty("counted",true);++requests;}socket->readAll();});}});server.listen(QHostAddress::LocalHost);}
};
QListWidgetItem* itemFor(QListWidget* list,const QString& id){if(!list)return nullptr;for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toString()==id)return list->item(i);return nullptr;}
bool selectChat(MainWindow& window,const QString& id){auto* list=window.findChild<QListWidget*>("agentList");auto* item=itemFor(list,id);if(!item)return false;list->setCurrentItem(item);return true;}
QPointer<QMenu> conversationMenu(MainWindow& window,const QString& id){
    deferredDeletes();auto* list=window.findChild<QListWidget*>("agentList");auto* item=itemFor(list,id);if(!item)return {};
    list->resize(220,600);list->doItemsLayout();list->scrollToItem(item);
    emit list->customContextMenuRequested(list->visualItemRect(item).center());
    return window.findChild<QMenu*>("conversationMenu");
}
bool trigger(MainWindow& window,const QString& id,const QString& actionName){
    auto menu=conversationMenu(window,id);if(!menu)return false;auto* action=menu->findChild<QAction*>(actionName);const bool enabled=action&&action->isEnabled();
    if(enabled)action->trigger();
    if(menu)menu->close();
    deferredDeletes();return enabled;
}
bool confirmDelete(MainWindow& window,bool confirm){
    auto* box=window.findChild<QMessageBox*>("deleteConversationDialog");if(!box)return false;auto* button=box->button(confirm?QMessageBox::Yes:QMessageBox::Cancel);if(!button)return false;button->click();deferredDeletes();return true;
}
bool rename(MainWindow& window,const QString& id,const QString& title){
    if(!trigger(window,id,"renameConversation"))return false;
    auto* dialog=window.findChild<QInputDialog*>("renameConversationDialog");if(!dialog)return false;dialog->setTextValue(title);dialog->accept();deferredDeletes();return true;
}
SettingsPage* archivedPage(MainWindow& window){auto* page=window.findChild<SettingsPage*>();if(page)page->selectSection("archived-agents");return page;}
QPushButton* archivedAction(SettingsPage* page,const QString& id,bool restore){
    auto* list=page?page->findChild<QListWidget*>("archivedChats"):nullptr;auto* item=itemFor(list,id);auto* row=item?list->itemWidget(item):nullptr;
    auto* button=row?row->findChild<QPushButton*>((restore?"restoreArchivedChat_":"deleteArchivedChat_")+id):nullptr;
    return button&&button->property("conversationId").toString()==id?button:nullptr;
}
bool clickArchivedAction(SettingsPage* page,const QString& id,bool restore){auto* button=archivedAction(page,id,restore);if(!button)return false;button->click();deferredDeletes();return true;}
AgentToolHost* hostFor(MainWindow& window,const QString& id){for(auto* host:window.findChildren<AgentToolHost*>())if(host->agentId()==id)return host;return nullptr;}
AgentEngine* engineFor(MainWindow& window,const QString& id){auto* host=hostFor(window,id);return host?qobject_cast<AgentEngine*>(host->parent()):nullptr;}
struct Context {
    AttachmentRef attachment;QJsonObject attachmentMetadata,readMetadata;QByteArray bytes="independent attachment bytes\n";FileReadCapture capture;
    bool seed(Fixture& f,Conversation* source,QString* error){
        AttachmentStore store(f.data);attachment=store.write(source->id,"notes.txt",bytes,error);if(attachment.isEmpty())return false;
        AttachmentPipeline pipeline(store);attachmentMetadata=pipeline.capture(source->id,{attachment.toJson()},{},error);if(attachmentMetadata.isEmpty())return false;
        capture.tool="read";capture.bytes="captured file version\n";capture.descriptor={"ws1/note.txt","note.txt",capture.bytes.size(),{}};
        const QJsonObject args{{"path","ws1/note.txt"}};capture.inputHash=fileReadInputHash("read",args);
        FileReadSnapshotStore reads(f.data);readMetadata=FileReadSnapshotStore::commit(reads.acquire(source->id,error),capture,nullptr,error);if(readMetadata.isEmpty())return false;
        source->messages={QJsonObject{{"role","user"},{"content","Source history"},{"metadata",attachmentMetadata}},
            QJsonObject{{"role","assistant"},{"tool_calls",QJsonArray{QJsonObject{{"id","read-call"},{"type","function"},{"function",QJsonObject{{"name","read"},{"arguments",QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}}}}},
            QJsonObject{{"role","tool"},{"tool_call_id","read-call"},{"content","File opened and loaded into context."},{"metadata",readMetadata}}};
        return true;
    }
};
GitResult gitResult(GitService* service,quint64 id){QSignalSpy completed(service,&GitService::completed);QElapsedTimer timer;timer.start();while(timer.elapsed()<15000){for(const auto& row:completed)if(row[0].toULongLong()==id)return qvariant_cast<GitResult>(row[1]);QTest::qWait(5);}GitResult result;result.error.message="Timed out waiting for fixture Git inspection";return result;}
bool runGit(const QString& temporaryRoot,const QString& cwd,const QStringList& args,QString* error){
    if(!QDir::cleanPath(cwd).startsWith(QDir::cleanPath(temporaryRoot)+'/',Qt::CaseInsensitive)){*error="Fixture Git command escaped its temporary root";return false;}
    auto env=gitdetail::safeEnvironment();env.insert("GIT_CONFIG_NOSYSTEM","1");env.insert("GIT_CONFIG_GLOBAL",temporaryRoot+"/no-global-config");QProcess process;process.setProcessEnvironment(env);process.setWorkingDirectory(cwd);process.start("git",args);
    if(!process.waitForFinished(15000)||process.exitCode()!=0){*error=QString::fromUtf8(process.readAllStandardError());return false;}return true;
}
}

class ConversationsTest:public QObject {
    Q_OBJECT
private slots:
    void forkClonesHistoryAndBothSnapshotStoresIndependently();
    void archiveCancelsBusyAgentRetainsFilesAndUnloadsHost();
    void restoreDoesNotActivateUntilSelected();
    void deleteCancelPreservesAndConfirmationRemovesOwnedFiles();
    void busyDeleteIsDisabledAndLateConfirmationIsRefused();
    void archivedSearchPaginationAndSelection();
    void failedPersistencePreservesConversationState_data();
    void failedPersistencePreservesConversationState();
    void failedSelectionSaveKeepsDraftVisible();
    void liveOwnersSeparateFromArchivedAndUnselectedHistory();
    void backgroundForkDoesNotLeaveAnUntrackedSourceHost();
    void rejectedInheritedHistoryPreservesDraftAndAttachments_data();
    void rejectedInheritedHistoryPreservesDraftAndAttachments();
    void archiveRowsTargetTheirOwnChatAndSortByArchiveDate();
    void lastSelectedActiveChatSurvivesRestart();
    void approvalPolicyFollowsSourceForkNewChatAndRestart();
    void failedApprovalSaveRestoresSelectorAndStoredPolicy_data();
    void failedApprovalSaveRestoresSelectorAndStoredPolicy();
    void legacyApprovalPolicyDefaultsToAsk();
};

void ConversationsTest::forkClonesHistoryAndBothSnapshotStoresIndependently(){
    Fixture f;auto source=chat("source");Context context;QString error;QVERIFY2(context.seed(f,&source,&error),qPrintable(error));source.draft="Unsent source draft";source.draftAttachments={context.attachment.toJson()};QVERIFY(f.seed({source}));
    MainWindow window(f.data);QVERIFY(trigger(window,"source","forkConversation"));const auto forkId=window.currentSessionId();QVERIFY(forkId!="source");
    const auto fork=f.stored(forkId);QCOMPARE(f.sessions().size(),2);QCOMPARE(fork.messages,source.messages);QCOMPARE(fork.title,QString("Fork: ")+source.title);QVERIFY(fork.titleLocked);QVERIFY(!fork.archived);QVERIFY(fork.draft.isEmpty());QVERIFY(fork.draftAttachments.isEmpty());QCOMPARE(f.stored("source").draft,source.draft);
    QVERIFY(rename(window,forkId,"Independent fork title"));QCOMPARE(f.stored("source").title,source.title);
    AttachmentStore attachments(f.data);FileReadSnapshotStore reads(f.data);FileReadCapture captured;
    QCOMPARE(attachments.read(forkId,context.attachment,{},&error),context.bytes);QVERIFY(error.isEmpty());
    const auto hash=context.attachmentMetadata["pathReferences"].toObject()[context.attachment.path].toString();QVERIFY(!hash.isEmpty());QCOMPARE(attachments.readSnapshot(forkId,context.attachment,hash,&error),context.bytes);
    QVERIFY2(FileReadSnapshotStore::read(reads.acquire(forkId),context.readMetadata,&captured,&error),qPrintable(error));QCOMPARE(captured.bytes,context.capture.bytes);
    QVERIFY(trigger(window,"source","deleteConversation"));QVERIFY(confirmDelete(window,true));QVERIFY(f.stored("source").id.isEmpty());
    QCOMPARE(f.stored(forkId).messages,source.messages);QCOMPARE(attachments.read(forkId,context.attachment,{},&error),context.bytes);
    QVERIFY2(FileReadSnapshotStore::read(reads.acquire(forkId),context.readMetadata,&captured,&error),qPrintable(error));QCOMPARE(captured.bytes,context.capture.bytes);
}
void ConversationsTest::archiveCancelsBusyAgentRetainsFilesAndUnloadsHost(){
    Fixture f;HeldProvider provider;QVERIFY(provider.server.isListening());QVERIFY(f.provider(provider.server.serverPort()));auto source=chat("source"),other=chat("other");Context context;QString error;QVERIFY2(context.seed(f,&source,&error),qPrintable(error));QVERIFY(f.seed({source,other}));
    MainWindow window(f.data);window.submitPrompt("Hold this request locally");QPointer<AgentEngine> engine=engineFor(window,"source");QPointer<AgentToolHost> host=hostFor(window,"source");QVERIFY(engine);QVERIFY(host);QTRY_VERIFY(engine->isBusy());QTRY_COMPARE_WITH_TIMEOUT(provider.requests,1,5000);
    QSignalSpy finished(engine,&AgentEngine::finished);const auto existingLease=FileReadSnapshotStore(f.data).acquire("source");
    const auto beforeArchive=QDateTime::currentMSecsSinceEpoch();QVERIFY(trigger(window,"source","archiveConversation"));QVERIFY(f.stored("source").archived);QVERIFY(f.stored("source").archivedAt>=beforeArchive);QVERIFY(f.stored("source").archivedAt<=QDateTime::currentMSecsSinceEpoch());QCOMPARE(finished.size(),1);QVERIFY(engine.isNull());QVERIFY(host.isNull());QCOMPARE(window.currentSessionId(),QString("other"));
    QVERIFY(f.stored("source").messages.last().toObject()["content"].toString().contains("Hold this request locally"));
    QCOMPARE(AttachmentStore(f.data).read("source",context.attachment,{},&error),context.bytes);QVERIFY(error.isEmpty());QVERIFY(QDir(f.data+"/read-context-v1/attachment-snapshots/source").exists());
    FileReadCapture captured;QVERIFY(!FileReadSnapshotStore::read(existingLease,context.readMetadata,&captured,&error));
}
void ConversationsTest::restoreDoesNotActivateUntilSelected(){
    Fixture f;auto active=chat("active"),archived=chat("archived",true);archived.draft="Retained archived draft";QVERIFY(f.seed({active,archived}));MainWindow window(f.data);
    auto* page=archivedPage(window);QVERIFY(page);auto* list=page->findChild<QListWidget*>("archivedChats");QCOMPARE(list->count(),1);QCOMPARE(window.currentSessionId(),QString("active"));QVERIFY(!hostFor(window,"archived"));
    QVERIFY(clickArchivedAction(page,"archived",true));QVERIFY(!f.stored("archived").archived);QCOMPARE(f.stored("archived").archivedAt,qint64(0));QCOMPARE(window.currentSessionId(),QString("active"));QVERIFY(!hostFor(window,"archived"));
    QCOMPARE(page->findChild<QListWidget*>("archivedChats")->count(),0);QVERIFY(selectChat(window,"archived"));QCOMPARE(window.currentSessionId(),QString("archived"));QCOMPARE(window.findChild<Composer*>()->toPlainText(),archived.draft);QVERIFY(!window.findChild<Composer*>()->isReadOnly());
}
void ConversationsTest::deleteCancelPreservesAndConfirmationRemovesOwnedFiles(){
    Fixture f;auto source=chat("source"),other=chat("other");Context context;QString error;QVERIFY(context.seed(f,&source,&error));QVERIFY(f.seed({source,other}));MainWindow window(f.data);const auto before=f.sessionBytes();
    QVERIFY(trigger(window,"source","deleteConversation"));QVERIFY(confirmDelete(window,false));QCOMPARE(f.sessionBytes(),before);QCOMPARE(window.currentSessionId(),QString("source"));QCOMPARE(AttachmentStore(f.data).read("source",context.attachment,{},&error),context.bytes);
    QVERIFY(trigger(window,"source","deleteConversation"));QVERIFY(confirmDelete(window,true));QCOMPARE(window.currentSessionId(),QString("other"));QVERIFY(f.stored("source").id.isEmpty());QCOMPARE(f.sessions().size(),1);
    QVERIFY(!QDir(f.data+"/attachments/source").exists());QVERIFY(!QDir(f.data+"/attachment-snapshots/source").exists());QVERIFY(!QDir(f.data+"/read-context-v1/attachment-snapshots/source").exists());SessionStore restarted(f.data);restarted.load();QVERIFY(restarted.pendingCleanup().isEmpty());
}
void ConversationsTest::busyDeleteIsDisabledAndLateConfirmationIsRefused(){
    Fixture f;HeldProvider provider;QVERIFY(provider.server.isListening());QVERIFY(f.provider(provider.server.serverPort()));QVERIFY(f.seed({chat("source"),chat("other")}));MainWindow window(f.data);
    QVERIFY(trigger(window,"source","deleteConversation"));QVERIFY(window.findChild<QMessageBox*>("deleteConversationDialog"));window.submitPrompt("Busy while confirmation was open");QPointer<AgentEngine> engine=engineFor(window,"source");QVERIFY(engine);QTRY_VERIFY(engine->isBusy());
    QVERIFY(confirmDelete(window,true));QVERIFY(!f.stored("source").id.isEmpty());QVERIFY(engine&&engine->isBusy());QVERIFY(window.findChild<QLabel*>("sessionStatus")->text().contains("Stop this chat"));
    auto menu=conversationMenu(window,"source");QVERIFY(menu);auto* remove=menu->findChild<QAction*>("deleteConversation");QVERIFY(remove);QVERIFY(!remove->isEnabled());menu->close();deferredDeletes();engine->cancel();
}
void ConversationsTest::archivedSearchPaginationAndSelection(){
    Fixture f;QList<Conversation> values{chat("active")};for(int i=0;i<53;++i){auto value=chat(QString("archived-%1").arg(i,2,10,QChar('0')),true);value.title=QString("Archived item %1").arg(i,2,10,QChar('0'));values.append(value);}QVERIFY(f.seed(values));MainWindow window(f.data);auto* page=archivedPage(window);QVERIFY(page);
    auto* list=page->findChild<QListWidget*>("archivedChats");QCOMPARE(list->count(),25);auto* more=page->findChild<QPushButton*>("loadMoreArchivedChats");QVERIFY(!more->isHidden());more->click();QCOMPARE(list->count(),50);list->setCurrentRow(31);const auto selected=list->currentItem()->data(Qt::UserRole).toString();more->click();QCOMPARE(list->count(),53);QVERIFY(more->isHidden());QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(),selected);
    page->reload();list=page->findChild<QListWidget*>("archivedChats");QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(),selected);QCOMPARE(list->count(),53);
    auto* search=page->findChild<QLineEdit*>("archivedSearch");search->setText("  ITEM 42  ");QTRY_COMPARE(list->count(),1);QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(),QString("archived-42"));
    search->setText("does not exist");QTRY_COMPARE(list->count(),0);QVERIFY(!archivedAction(page,"archived-42",true));QVERIFY(!archivedAction(page,"archived-42",false));QVERIFY(!page->findChild<QLabel*>("archivedEmpty")->isHidden());QCOMPARE(page->findChild<QLabel*>("archivedEmpty")->text(),QString("No matching archived chats."));
    search->setText("item 42");QTRY_COMPARE(list->count(),1);QVERIFY(clickArchivedAction(page,"archived-42",true));QVERIFY(!f.stored("archived-42").archived);QCOMPARE(page->findChild<QListWidget*>("archivedChats")->count(),0);QCOMPARE(window.currentSessionId(),QString("active"));
}
void ConversationsTest::failedPersistencePreservesConversationState_data(){QTest::addColumn<QString>("operation");for(const auto& value:{"new","rename","fork","archive","delete","restore"})QTest::newRow(value)<<QString(value);}
void ConversationsTest::failedPersistencePreservesConversationState(){
    QFETCH(QString,operation);Fixture f;auto source=chat("source"),archived=chat("archived",true);Context context;QString error;QVERIFY(context.seed(f,&source,&error));source.draft="Visible draft must survive";QVERIFY(f.seed({source,chat("other"),archived}));MainWindow window(f.data);QVERIFY(f.externalChange());const auto external=f.sessionBytes();const auto beforeCount=window.findChild<QListWidget*>("agentList")->count();
    if(operation=="new")window.findChild<QPushButton*>("newAgentButton")->click();
    else if(operation=="rename")QVERIFY(rename(window,"source","Must not replace title"));
    else if(operation=="delete"){QVERIFY(trigger(window,"source","deleteConversation"));QVERIFY(confirmDelete(window,true));}
    else if(operation=="restore"){auto* page=archivedPage(window);QVERIFY(page);QVERIFY(clickArchivedAction(page,"archived",true));QCOMPARE(page->findChild<QListWidget*>("archivedChats")->count(),1);}
    else QVERIFY(trigger(window,"source",operation+"Conversation"));
    QCOMPARE(window.currentSessionId(),QString("source"));QCOMPARE(window.findChild<Composer*>()->toPlainText(),source.draft);QCOMPARE(window.findChild<QListWidget*>("agentList")->count(),beforeCount);QCOMPARE(f.sessionBytes(),external);QVERIFY(f.stored("archived").archived);QVERIFY(!f.stored("source").archived);
    QVERIFY(window.findChild<QLabel*>("sessionStatus")->text().contains("Could not save conversations"));QCOMPARE(AttachmentStore(f.data).read("source",context.attachment,{},&error),context.bytes);
    if(operation=="fork"){QCOMPARE(QDir(f.data+"/attachments").entryList(QDir::Dirs|QDir::NoDotAndDotDot),QStringList{"source"});QCOMPARE(QDir(f.data+"/read-context-v1/attachment-snapshots").entryList(QDir::Dirs|QDir::NoDotAndDotDot),QStringList{"source"});}
}
void ConversationsTest::failedSelectionSaveKeepsDraftVisible(){
    Fixture f;QVERIFY(f.seed({chat("source"),chat("other")}));MainWindow window(f.data);auto* composer=window.findChild<Composer*>();composer->setPlainText("Unsaved draft in source");QVERIFY(f.externalChange());const auto external=f.sessionBytes();QVERIFY(selectChat(window,"other"));
    QCOMPARE(window.currentSessionId(),QString("source"));QCOMPARE(composer->toPlainText(),QString("Unsaved draft in source"));QCOMPARE(window.findChild<QListWidget*>("agentList")->currentItem()->data(Qt::UserRole).toString(),QString("source"));QVERIFY(window.findChild<QLabel*>("sessionStatus")->text().contains("Could not save conversations"));QCOMPARE(f.sessionBytes(),external);
}
void ConversationsTest::liveOwnersSeparateFromArchivedAndUnselectedHistory(){
    Fixture f;const auto root=f.base.filePath("repository"),linked=f.data+"/worktrees/fixture/linked";QVERIFY(QDir().mkpath(root));QVERIFY(QDir().mkpath(QFileInfo(linked).absolutePath()));QString error;
    for(const auto& args:QList<QStringList>{{"init","-b","main"},{"config","user.name","ADE fixture"},{"config","user.email","fixture@example.invalid"},{"config","commit.gpgsign","false"},{"config","core.hooksPath",f.base.filePath("hooks")}})QVERIFY2(runGit(f.base.path(),root,args,&error),qPrintable(error));
    QVERIFY(put(root+"/file.txt","fixture\n"));QVERIFY(runGit(f.base.path(),root,{"add","."},&error));QVERIFY(runGit(f.base.path(),root,{"commit","-m","initial"},&error));QVERIFY(runGit(f.base.path(),root,{"worktree","add","-b","linked",linked},&error));
    auto main=chat("main"),worktree=chat("worktree");main.workspace=root;worktree.workspace=linked;QVERIFY(f.seed({main,worktree}));MainWindow window(f.data);auto* panel=window.findChild<GitPanel*>();QVERIFY(panel);auto* service=panel->service();
    auto info=gitResult(service,service->deletionInfo(linked));QVERIFY2(info.ok,qPrintable(info.error.message));QVERIFY(!info.deletion.mounted);
    QVERIFY(selectChat(window,"worktree"));info=gitResult(service,service->deletionInfo(linked));QVERIFY(info.ok);QVERIFY(info.deletion.mounted);
    QVERIFY(selectChat(window,"main"));info=gitResult(service,service->deletionInfo(linked));QVERIFY(info.ok);QVERIFY(info.deletion.mounted);
    QVERIFY(trigger(window,"worktree","archiveConversation"));info=gitResult(service,service->deletionInfo(linked));QVERIFY(info.ok);QVERIFY(!info.deletion.mounted);
    auto* page=archivedPage(window);QVERIFY(clickArchivedAction(page,"worktree",true));info=gitResult(service,service->deletionInfo(linked));QVERIFY(info.ok);QVERIFY(!info.deletion.mounted);
    QVERIFY(selectChat(window,"worktree"));info=gitResult(service,service->deletionInfo(linked));QVERIFY(info.ok);QVERIFY(info.deletion.mounted);QVERIFY(QDir(linked).exists());
}
void ConversationsTest::backgroundForkDoesNotLeaveAnUntrackedSourceHost(){
    Fixture f;const auto root=f.base.filePath("repository"),linked=f.data+"/worktrees/fixture/linked";QVERIFY(QDir().mkpath(root));QVERIFY(QDir().mkpath(QFileInfo(linked).absolutePath()));QString error;
    for(const auto& args:QList<QStringList>{{"init","-b","main"},{"config","user.name","ADE fixture"},{"config","user.email","fixture@example.invalid"},{"config","commit.gpgsign","false"},{"config","core.hooksPath",f.base.filePath("hooks")}})QVERIFY2(runGit(f.base.path(),root,args,&error),qPrintable(error));
    QVERIFY(put(root+"/file.txt","fixture\n"));QVERIFY(runGit(f.base.path(),root,{"add","."},&error));QVERIFY(runGit(f.base.path(),root,{"commit","-m","initial"},&error));QVERIFY(runGit(f.base.path(),root,{"worktree","add","-b","linked",linked},&error));
    auto main=chat("main"),source=chat("background-source");main.workspace=root;source.workspace=linked;QVERIFY(f.seed({main,source}));MainWindow window(f.data);QVERIFY(!hostFor(window,source.id));
    QVERIFY(trigger(window,source.id,"forkConversation"));const auto forkId=window.currentSessionId();QVERIFY(forkId!=source.id);QVERIFY(forkId!="main");
    QVERIFY(trigger(window,forkId,"archiveConversation"));QCOMPARE(window.currentSessionId(),QString("main"));
    auto* service=window.findChild<GitPanel*>()->service();const auto info=gitResult(service,service->deletionInfo(linked));QVERIFY2(info.ok,qPrintable(info.error.message));
    QVERIFY2(!hostFor(window,source.id)||info.deletion.mounted,"A source host created only for cloning must be disposed or retain a live workspace owner after the fork is archived.");
}
void ConversationsTest::rejectedInheritedHistoryPreservesDraftAndAttachments_data(){
    QTest::addColumn<QString>("rawDraft");QTest::addColumn<bool>("withAttachment");
    QTest::newRow("text-whitespace")<<QString(" \t keep this exact\n  draft \t\n")<<false;
    QTest::newRow("text-and-attachment")<<QString(" \t keep this exact\n  draft \t\n")<<true;
    QTest::newRow("attachment-only-whitespace")<<QString(" \t\n  ")<<true;
}
void ConversationsTest::rejectedInheritedHistoryPreservesDraftAndAttachments(){
    QFETCH(QString,rawDraft);QFETCH(bool,withAttachment);Fixture f;HeldProvider provider;QVERIFY(provider.server.isListening());QVERIFY(f.provider(provider.server.serverPort()));
    auto source=chat("source");source.messages={QJsonObject{{"role","tool"},{"tool_call_id","orphan-inherited-call"},{"content","An imported result without a call"}}};source.draft=rawDraft;
    AttachmentRef attachment;QString error;const QByteArray attachmentBytes="Draft attachment must survive rejected history";
    if(withAttachment){attachment=AttachmentStore(f.data).write(source.id,"draft.txt",attachmentBytes,&error);QVERIFY2(!attachment.isEmpty(),qPrintable(error));source.draftAttachments={attachment.toJson()};}
    QVERIFY(f.seed({source}));QString visibleDraft,status;QJsonArray visibleAttachments;
    {
        MainWindow window(f.data);window.findChild<QToolButton*>("sendButton")->click();QTest::qWait(100);
        auto* engine=engineFor(window,source.id);QVERIFY(engine);QVERIFY(!engine->isBusy());QCOMPARE(provider.requests,0);
        visibleDraft=window.findChild<Composer*>()->toPlainText();status=window.findChild<QLabel*>("sessionStatus")->text();visibleAttachments=f.stored(source.id).draftAttachments;
        QCOMPARE(f.stored(source.id).messages,source.messages);
    }
    const auto persisted=f.stored(source.id);QString reopenedDraft;QJsonArray reopenedAttachments;
    {MainWindow reopened(f.data);QCOMPARE(reopened.currentSessionId(),source.id);reopenedDraft=reopened.findChild<Composer*>()->toPlainText();reopenedAttachments=f.stored(source.id).draftAttachments;}
    qInfo().noquote()<<"Rejected-history preservation: visibleDraft="<<(visibleDraft==rawDraft)<<" diskDraft="<<(persisted.draft==rawDraft)<<" reopenedDraft="<<(reopenedDraft==rawDraft)<<" draftAttachments="<<(visibleAttachments==source.draftAttachments)<<" reopenedAttachments="<<(reopenedAttachments==source.draftAttachments);
    QVERIFY2(status.contains("tool history is invalid"),qPrintable(status));
    if(withAttachment){QCOMPARE(AttachmentStore(f.data).read(source.id,attachment,{},&error),attachmentBytes);QVERIFY(error.isEmpty());}
    QCOMPARE(visibleDraft,rawDraft);QCOMPARE(persisted.draft,rawDraft);QCOMPARE(reopenedDraft,rawDraft);QCOMPARE(visibleAttachments,source.draftAttachments);QCOMPARE(reopenedAttachments,source.draftAttachments);
}
void ConversationsTest::archiveRowsTargetTheirOwnChatAndSortByArchiveDate(){
    Fixture f;auto active=chat("active"),old=chat("old",true),recent=chat("recent",true),fallback=chat("fallback",true),tieA=chat("tie-a",true),tieB=chat("tie-b",true);
    old.archivedAt=100;old.updated=9000;recent.archivedAt=500;recent.updated=10;fallback.updated=600;tieA.archivedAt=tieB.archivedAt=500;tieA.updated=tieB.updated=10;
    QVERIFY(f.seed({active,old,recent,fallback,tieA,tieB}));MainWindow window(f.data);auto* page=archivedPage(window);auto* list=page->findChild<QListWidget*>("archivedChats");QStringList order;for(int i=0;i<list->count();++i)order.append(list->item(i)->data(Qt::UserRole).toString());
    QCOMPARE(order,QStringList({"fallback","recent","tie-a","tie-b","old"}));list->setCurrentRow(0);QVERIFY(clickArchivedAction(page,"recent",true));
    QVERIFY(!f.stored("recent").archived);QCOMPARE(f.stored("recent").archivedAt,qint64(0));QVERIFY(f.stored("fallback").archived);QCOMPARE(window.currentSessionId(),QString("active"));
    list=page->findChild<QListWidget*>("archivedChats");list->setCurrentRow(0);QVERIFY(clickArchivedAction(page,"old",false));QVERIFY(confirmDelete(window,false));QVERIFY(f.stored("old").archived);
    QVERIFY(clickArchivedAction(page,"old",false));QVERIFY(confirmDelete(window,true));QVERIFY(f.stored("old").id.isEmpty());QVERIFY(f.stored("fallback").archived);QVERIFY(f.stored("tie-a").archived);QVERIFY(f.stored("tie-b").archived);
}
void ConversationsTest::lastSelectedActiveChatSurvivesRestart(){
    Fixture f;auto newest=chat("newest"),older=chat("older"),archived=chat("archived",true);older.draft="Older chat draft";QVERIFY(f.seed({newest,older,archived}));
    {MainWindow window(f.data);QCOMPARE(window.currentSessionId(),newest.id);QVERIFY(selectChat(window,older.id));QCOMPARE(window.currentSessionId(),older.id);}
    {MainWindow reopened(f.data);QCOMPARE(reopened.currentSessionId(),older.id);QCOMPARE(reopened.findChild<Composer*>()->toPlainText(),older.draft);QVERIFY(trigger(reopened,older.id,"archiveConversation"));QCOMPARE(reopened.currentSessionId(),newest.id);}
    {MainWindow reopened(f.data);QCOMPARE(reopened.currentSessionId(),newest.id);}
    {QSettings settings(f.data+"/window.ini",QSettings::IniFormat);settings.setValue("lastConversationId",archived.id);settings.sync();}
    {MainWindow reopened(f.data);QCOMPARE(reopened.currentSessionId(),newest.id);QVERIFY(f.stored(archived.id).archived);QVERIFY(!hostFor(reopened,archived.id));}
}
void ConversationsTest::approvalPolicyFollowsSourceForkNewChatAndRestart(){
    Fixture f;auto ask=chat("ask"),allow=chat("allow");allow.alwaysAllow=true;QVERIFY(f.seed({ask,allow}));QString forkId,allowChildId,askChildId;
    {
        MainWindow window(f.data);auto* selector=window.findChild<QComboBox*>("permissionSelector");QVERIFY(selector);QVERIFY(!selector->currentData().toBool());QVERIFY(selectChat(window,allow.id));QVERIFY(selector->currentData().toBool());
        QVERIFY(trigger(window,ask.id,"forkConversation"));forkId=window.currentSessionId();QVERIFY(forkId!=ask.id);QVERIFY(!selector->currentData().toBool());QVERIFY(!f.stored(forkId).alwaysAllow);QVERIFY(!f.stored(ask.id).alwaysAllow);QVERIFY(f.stored(allow.id).alwaysAllow);
        window.findChild<QPushButton*>("newAgentButton")->click();askChildId=window.currentSessionId();QVERIFY(!f.stored(askChildId).alwaysAllow);QVERIFY(!selector->currentData().toBool());
        QVERIFY(selectChat(window,allow.id));window.findChild<QPushButton*>("newAgentButton")->click();allowChildId=window.currentSessionId();QVERIFY(f.stored(allowChildId).alwaysAllow);QVERIFY(selector->currentData().toBool());
    }
    {
        MainWindow reopened(f.data);auto* selector=reopened.findChild<QComboBox*>("permissionSelector");QCOMPARE(reopened.currentSessionId(),allowChildId);QVERIFY(selector->currentData().toBool());
        QVERIFY(selectChat(reopened,forkId));QVERIFY(!selector->currentData().toBool());QVERIFY(selectChat(reopened,allow.id));QVERIFY(selector->currentData().toBool());QVERIFY(selectChat(reopened,askChildId));QVERIFY(!selector->currentData().toBool());
        selector->setCurrentIndex(selector->findData(true));QVERIFY(f.stored(askChildId).alwaysAllow);QVERIFY(!f.stored(ask.id).alwaysAllow);QVERIFY(!f.stored(forkId).alwaysAllow);
    }
    {MainWindow reopened(f.data);QCOMPARE(reopened.currentSessionId(),askChildId);QVERIFY(reopened.findChild<QComboBox*>("permissionSelector")->currentData().toBool());}
}
void ConversationsTest::failedApprovalSaveRestoresSelectorAndStoredPolicy_data(){QTest::addColumn<bool>("initial");QTest::newRow("ask-to-allow")<<false;QTest::newRow("allow-to-ask")<<true;}
void ConversationsTest::failedApprovalSaveRestoresSelectorAndStoredPolicy(){
    QFETCH(bool,initial);Fixture f;auto source=chat("source");source.alwaysAllow=initial;QVERIFY(f.seed({source}));
    {MainWindow window(f.data);auto* selector=window.findChild<QComboBox*>("permissionSelector");QCOMPARE(selector->currentData().toBool(),initial);QVERIFY(f.externalChange());const auto external=f.sessionBytes();selector->setCurrentIndex(selector->findData(!initial));
        QCOMPARE(selector->currentData().toBool(),initial);QCOMPARE(f.stored(source.id).alwaysAllow,initial);QCOMPARE(f.sessionBytes(),external);QVERIFY(window.findChild<QLabel*>("sessionStatus")->text().contains("Could not save conversations"));}
    {MainWindow reopened(f.data);QCOMPARE(reopened.findChild<QComboBox*>("permissionSelector")->currentData().toBool(),initial);}
}
void ConversationsTest::legacyApprovalPolicyDefaultsToAsk(){
    Fixture f;QVERIFY(f.seed({chat("legacy")}));auto document=QJsonDocument::fromJson(f.sessionBytes()).object();auto sessions=document["sessions"].toArray();auto source=sessions[0].toObject();source.remove("alwaysAllow");sessions[0]=source;document["sessions"]=sessions;QVERIFY(put(f.data+"/sessions.json",QJsonDocument(document).toJson()));
    MainWindow window(f.data);QCOMPARE(window.currentSessionId(),QString("legacy"));QVERIFY(!window.findChild<QComboBox*>("permissionSelector")->currentData().toBool());QVERIFY(!f.stored("legacy").alwaysAllow);
}
int main(int argc,char** argv){qputenv("QT_QPA_PLATFORM","offscreen");qputenv("QT_QPA_FONTDIR",QDir(qEnvironmentVariable("WINDIR","C:/Windows")).filePath("Fonts").toUtf8());QApplication application(argc,argv);const auto fonts=AppFonts::load();QFont font(fonts.body);font.setPixelSize(13);application.setFont(font);ConversationsTest tests;return QTest::qExec(&tests,argc,argv);}
#include "test_conversations.moc"
