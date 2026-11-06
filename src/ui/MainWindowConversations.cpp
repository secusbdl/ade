#include "MainWindow.h"
#include "Composer.h"
#include "ConversationView.h"
#include "ModelSelection.h"
#include "SettingsPage.h"
#include "SidebarNavigation.h"
#include "attachments/AttachmentStore.h"
#include "attachments/FileReadSnapshotStore.h"
#include "core/AgentEngine.h"
#include "core/UtilityService.h"
#include "runtime/AgentToolHost.h"
#include "mutations/FileMutationJournal.h"
#include "shell/SessionLogStore.h"
#include <QComboBox>
#include <QAction>
#include <QDateTime>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QToolButton>
#include <QTextBrowser>
#include <QUuid>

namespace ade {
void MainWindow::conversationError(const QString& message) {
    m_status->setText(message);
    if(m_settings&&m_settings->currentSection()=="archived-agents")m_settings->reportConversationError(message);
}
bool MainWindow::commitConversationState(const QList<Conversation>& next,const QStringList& pendingCleanup) {
    QString error;
    if(!m_store.saveWithCleanup(next,pendingCleanup,&error)) {
        conversationError("Could not save conversations: "+error);return false;
    }
    m_sessions=next;updateGitUsageTracking();return true;
}
void MainWindow::newConversation() {
    Conversation session;
    if(const auto* previous=current())session.alwaysAllow=previous->alwaysAllow;
    const auto selection=ModelSelection::fromJson(m_models->currentData().toJsonObject());
    session.providerId=selection.providerId;session.model=selection.modelId;session.presetId=selection.presetId;
    session.id=QUuid::createUuid().toString(QUuid::WithoutBraces);session.title="New Conversation";
    session.created=session.updated=QDateTime::currentMSecsSinceEpoch();
    auto next=m_sessions;
    for(auto& previous:next)if(previous.id==m_currentId)previous.draft=m_composer->toPlainText();
    next.prepend(session);
    if(!commitConversationState(next,m_store.pendingCleanup()))return;
    m_agents->setProperty("showArchived",false);refreshAgents();selectConversation(session.id);m_composer->setFocus();
}
void MainWindow::showConversationMenu(const QPoint& point) {
    const auto* item=m_agents->itemAt(point);if(!item)return;
    const auto id=item->data(Qt::UserRole).toString();const auto* session=find(id);if(!session)return;
    auto* menu=new QMenu(this);menu->setObjectName("conversationMenu");menu->setAttribute(Qt::WA_DeleteOnClose);
    if(!session->archived&&m_sidebarNavigation){
        const bool pinned=m_sidebarNavigation->isPinned(id);auto* pin=menu->addAction(pinned?"Unpin":"Pin globally");pin->setObjectName(pinned?"unpinConversation":"pinConversation");
        connect(pin,&QAction::triggered,this,[this,id,pinned]{if(m_sidebarNavigation)m_sidebarNavigation->setPinned(id,!pinned);});
        if(pinned){for(const auto offset:{-1,1}){auto* action=menu->addAction(offset<0?"Move pin up":"Move pin down");action->setObjectName(offset<0?"movePinUp":"movePinDown");connect(action,&QAction::triggered,this,[this,id,offset]{if(m_sidebarNavigation)m_sidebarNavigation->movePinned(id,offset);});}}
        menu->addSeparator();
    }
    auto* rename=menu->addAction("Rename");rename->setObjectName("renameConversation");
    connect(rename,&QAction::triggered,this,[this,id]{requestRenameConversation(id);});
    auto* fork=menu->addAction("Fork chat");fork->setObjectName("forkConversation");
    connect(fork,&QAction::triggered,this,[this,id]{forkConversation(id);});
    auto* archive=menu->addAction(session->archived?"Restore":"Archive");archive->setObjectName(session->archived?"restoreConversation":"archiveConversation");
    const bool archived=session->archived;
    connect(archive,&QAction::triggered,this,[this,id,archived]{if(archived)restoreConversation(id);else archiveConversation(id);});
    menu->addSeparator();auto* remove=menu->addAction("Delete");remove->setObjectName("deleteConversation");
    remove->setEnabled(!m_engines.value(id)||!m_engines.value(id)->isBusy());
    connect(remove,&QAction::triggered,this,[this,id]{requestDeleteConversation(id);});
    menu->popup(m_agents->viewport()->mapToGlobal(point));
}
void MainWindow::requestRenameConversation(const QString& id) {
    const auto* session=find(id);if(!session)return;
    auto* dialog=new QInputDialog(this);dialog->setObjectName("renameConversationDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle("Rename chat");dialog->setLabelText("Name");dialog->setTextValue(session->title);
    connect(dialog,&QInputDialog::textValueSelected,this,[this,id](const QString& title){renameConversation(id,title);});dialog->open();
}
bool MainWindow::renameConversation(const QString& id,const QString& title) {
    if(title.trimmed().isEmpty()||!find(id))return false;
    auto next=m_sessions;for(auto& session:next)if(session.id==id){session.title=title.trimmed();session.titleLocked=true;}
    if(!commitConversationState(next,m_store.pendingCleanup()))return false;
    refreshAgents();if(m_settings->currentSection()=="archived-agents")m_settings->reload();return true;
}
bool MainWindow::forkConversation(const QString& id) {
    return cloneConversation(id,false);
}
bool MainWindow::createSideChat(const QString& sourceId) {
    const auto id=sourceId.isEmpty()?m_currentId:sourceId;
    if(const auto* source=find(id);!source||source->archived)return false;
    return cloneConversation(id,true);
}
bool MainWindow::cloneConversation(const QString& id,bool sideChat) {
    const auto* source=find(id);if(!source)return false;
    if((m_toolHosts.value(id)&&m_toolHosts.value(id)->mutationBusy())||(m_mutationUndoAgent==id&&!m_mutationUndoId.isEmpty())){conversationError("Wait for file changes to finish before forking this chat.");return false;}
    if(!source->sideChatParentId.isEmpty()){conversationError("Move this side chat to the sidebar before forking it again.");return false;}
    const bool temporarySourceHost=source->archived&&!m_engines.contains(id);
    Conversation fork=*source;
    if(const auto* engine=m_engines.value(id))fork.messages=engine->messages();
    fork.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    fork.title=(sideChat?"Side chat: ":"Fork: ")+(source->title.isEmpty()?QString("New chat"):source->title);fork.titleLocked=!sideChat;fork.sideChatParentId=sideChat?id:QString();
    fork.archived=false;fork.archivedAt=0;fork.draft.clear();fork.draftAttachments={};fork.queuedMessages={};fork.created=fork.updated=QDateTime::currentMSecsSinceEpoch();
    const QPointer<MainWindow> self(this);engineFor(id);
    const QPointer<AgentToolHost> host=m_toolHosts.value(id);QString error;
    if(!host||!host->cloneFileContext(fork.id,&error)) {
        if(self&&temporarySourceHost)disposeConversation(id);
        if(self)conversationError("Could not fork chat: "+error);
        return false;
    }
    if(!self)return false;
    fork.transcriptLinkProvenance=forkTranscriptLinkProvenance(id,fork.id,fork.workspace);
    if(temporarySourceHost)disposeConversation(id);
    if(!self)return false;
    auto next=m_sessions;next.prepend(fork);
    if(!commitConversationState(next,m_store.pendingCleanup())) {
        QString readError,attachmentError,shellError;
        const bool readsRemoved=FileReadSnapshotStore(m_store.directory()).removeAgent(fork.id,&readError);
        const bool attachmentsRemoved=AttachmentStore(m_store.directory()).removeAgent(fork.id,&attachmentError);
        const bool shellRemoved=SessionLogStore::removeAgent(m_store.directory(),fork.id,&shellError);
        if(!readsRemoved||!attachmentsRemoved||!shellRemoved)conversationError(m_status->text()+" Fork context cleanup failed: "+readError+attachmentError+shellError);
        return false;
    }
    m_agents->setProperty("showArchived",false);refreshAgents();if(sideChat)openSideChat(fork.id);else selectConversation(fork.id);return true;
}
void MainWindow::disposeConversation(const QString& id) {
    m_linkKnownRows.remove(id);m_linkProvenance.remove(id);m_linkTurnContext.remove(id);
    m_liveConversationOwners.remove(id);
    if(auto* utility=m_utilities.take(id).data()){utility->disconnect(this);utility->deleteLater();}
    auto* engine=m_engines.take(id);m_toolHosts.remove(id);
    if(engine){engine->disconnect(this);delete engine;}
}
void MainWindow::selectAfterConversationRemoval(const QString& id) {
    if(m_currentId!=id){refreshAgents();updateGitContext();return;}
    m_currentId.clear();
    for(const auto& session:m_sessions)if(!session.archived&&session.sideChatParentId.isEmpty()){const auto nextId=session.id;selectConversation(nextId);return;}
    // Retain no editable selection if creating the replacement cannot be saved.
    m_currentId.clear();m_send->setEnabled(false);m_stop->setEnabled(false);m_composer->clear();m_transcript->clear();
    newConversation();refreshAgents();updateGitContext();
}
bool MainWindow::archiveConversation(const QString& id) {
    if(!find(id))return false;
    const QPointer<MainWindow> self(this);
    QStringList children;for(const auto& session:m_sessions)if(session.sideChatParentId==id)children.append(session.id);
    auto family=children;family.append(id);
    for(const auto& target:family)if((m_toolHosts.value(target)&&m_toolHosts.value(target)->mutationBusy())||(m_mutationUndoAgent==target&&!m_mutationUndoId.isEmpty())){conversationError("Wait for file changes to finish before archiving this chat family.");return false;}
    for(const auto& child:children){if(auto* engine=m_engines.value(child);engine&&engine->isBusy())engine->cancel();if(!self)return false;}
    if(auto* engine=m_engines.value(id);engine&&engine->isBusy())engine->cancel();
    if(!self||!find(id))return false;
    QStringList currentChildren;for(const auto& session:m_sessions)if(session.sideChatParentId==id)currentChildren.append(session.id);
    if(currentChildren!=children){conversationError("The chat family changed while stopping. Retry the archive action.");return false;}
    // Stopping an engine emits callbacks that may start a file recovery/undo.
    // Keep the entire family loaded until those new mutations have settled.
    for(const auto& target:family)if((m_toolHosts.value(target)&&m_toolHosts.value(target)->mutationBusy())||(m_mutationUndoAgent==target&&!m_mutationUndoId.isEmpty())){conversationError("Wait for file changes to finish before archiving this chat family.");return false;}
    auto next=m_sessions;for(auto& session:next)if(session.id==id){session.archived=true;session.archivedAt=QDateTime::currentMSecsSinceEpoch();if(id==m_currentId)session.draft=m_composer->toPlainText();}
    if(!commitConversationState(next,m_store.pendingCleanup()))return false;
    if(m_sidebarNavigation)m_sidebarNavigation->forgetPinned(id);
    if(!self)return true;
    for(const auto& child:children){closeSideChatPane(child);if(!self)return true;disposeConversation(child);if(!self)return true;}
    disposeConversation(id);if(!self)return true;
    selectAfterConversationRemoval(id);if(m_settings->currentSection()=="archived-agents")m_settings->reload();return true;
}
bool MainWindow::restoreConversation(const QString& id) {
    if(!find(id))return false;
    auto next=m_sessions;for(auto& session:next)if(session.id==id){session.archived=false;session.archivedAt=0;}
    if(!commitConversationState(next,m_store.pendingCleanup()))return false;
    if(m_currentId==id)selectConversation(id);else {refreshAgents();updateGitContext();}
    refreshSideChats();
    if(m_settings->currentSection()=="archived-agents")m_settings->reload();
    return true;
}
void MainWindow::requestDeleteConversation(const QString& id) {
    const auto* session=find(id);if(!session)return;
    auto* box=new QMessageBox(QMessageBox::Warning,"Permanently delete chat?",
        "Delete \""+session->title+"\" and its history and attachments? This cannot be undone.",QMessageBox::Yes|QMessageBox::Cancel,this);
    box->setObjectName("deleteConversationDialog");box->setDefaultButton(QMessageBox::Cancel);box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box,&QMessageBox::finished,this,[this,id](int result){if(result==QMessageBox::Yes)deleteConversation(id);});box->open();
}
bool MainWindow::deleteConversation(const QString& id) {
    if(!find(id))return false;
    if(auto* engine=m_engines.value(id);engine&&engine->isBusy()){conversationError("Stop this chat before deleting it.");return false;}
    const QPointer<MainWindow> self(this);QStringList deleted{id};for(const auto& session:m_sessions)if(session.sideChatParentId==id)deleted.append(session.id);
    for(const auto& target:deleted)if((m_toolHosts.value(target)&&m_toolHosts.value(target)->mutationBusy())||(m_mutationUndoAgent==target&&!m_mutationUndoId.isEmpty())){conversationError("Wait for file changes to finish before deleting this chat family.");return false;}
    for(const auto& child:deleted){if(child==id)continue;if(auto* engine=m_engines.value(child);engine&&engine->isBusy())engine->cancel();if(!self)return false;}
    QStringList currentFamily{id};for(const auto& session:m_sessions)if(session.sideChatParentId==id)currentFamily.append(session.id);
    if(!find(id)||currentFamily!=deleted){conversationError("The chat family changed while stopping. Retry the delete action.");return false;}
    auto next=m_sessions;next.removeIf([&](const Conversation& session){return deleted.contains(session.id);});
    auto pending=m_store.pendingCleanup();for(const auto& target:deleted)if(!pending.contains(target))pending.append(target);
    if(!commitConversationState(next,pending))return false;
    if(m_sidebarNavigation)m_sidebarNavigation->forgetPinned(id);
    if(!self)return true;
    for(const auto& target:deleted){closeSideChatPane(target);if(!self)return true;disposeConversation(target);if(!self)return true;}
    selectAfterConversationRemoval(id);if(!self)return true;
    if(m_settings->currentSection()=="archived-agents")m_settings->reload();
    retryConversationCleanup();return true;
}
void MainWindow::retryConversationCleanup() {
    if(m_store.pendingCleanup().isEmpty())return;
    QString error;
    const bool complete=m_store.processPendingCleanup(m_sessions,[](const QString& id,const QString& directory,QString* failure){
        QString readError,attachmentError,mutationError,shellError;
        const bool readsRemoved=FileReadSnapshotStore(directory).removeAgent(id,&readError);
        const bool attachmentsRemoved=AttachmentStore(directory).removeAgent(id,&attachmentError);
        const bool mutationsRemoved=FileMutationJournal::removeAgent(directory,id,&mutationError);
        const bool shellRemoved=SessionLogStore::removeAgent(directory,id,&shellError);
        if(failure)*failure=QStringList{readError,attachmentError,mutationError,shellError}.join(" ").trimmed();
        return readsRemoved&&attachmentsRemoved&&mutationsRemoved&&shellRemoved;
    },&error);
    if(!complete)conversationError("The chat was deleted. File cleanup remains pending and will retry when the app opens: "+error);
}
}
