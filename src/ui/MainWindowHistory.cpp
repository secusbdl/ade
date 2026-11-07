#include "MainWindow.h"
#include "ConversationView.h"
#include "ModelSelection.h"
#include "SettingsPage.h"
#include "SideChatPane.h"
#include "core/AgentEngine.h"
#include "runtime/AgentToolHost.h"
#include <QLabel>
#include <QPointer>
namespace ade {
void MainWindow::connectHistoryControls(ConversationView* view){
    if(!view)return;
    const QPointer<MainWindow> self(this);const QPointer<ConversationView> target(view);
    connect(view,&ConversationView::failedTurnRetryRequested,this,[this,self,target](const QString& chat,const QString& row){
        if(!self||!target)return;
        // A view acts only for the conversation it currently shows: the main
        // transcript for the selected chat, a side pane for its own chat.
        const auto* session=find(chat);
        bool owned=session&&!session->archived&&visibleTranscript(chat,target,true);
        if(owned&&!session->sideChatParentId.isEmpty()){const auto* parent=find(session->sideChatParentId);owned=parent&&!parent->archived&&target!=m_transcript;}
        if(!owned){if(target->conversationId()==chat)pushHistoryActions(chat,target);return;}
        retryFailedTurn(chat,row,target);
    });
}
void MainWindow::pushHistoryActions(const QString& id,ConversationView* view){
    if(!view||view->conversationId()!=id)return;
    ConversationView::HistoryActions actions;
    const auto* session=find(id);
    if(session){
        const auto* engine=m_engines.value(id);
        // A loaded engine owns the live record; otherwise only a saved record
        // that still identifies the last accepted user row is offered.
        const auto record=engine?engine->turnFailure():(AgentEngine::validTurnFailure(session->messages,session->turnFailure)?session->turnFailure:QJsonObject{});
        actions.failedUserRowId=record["userRowId"].toString();
        actions.failureText=record["error"].toString();
        actions.queuedCount=(engine?engine->queuedMessages():session->queuedMessages).size();
        actions.busy=engine&&engine->isBusy();
        actions.archived=session->archived;
        if(!session->sideChatParentId.isEmpty()){const auto* parent=find(session->sideChatParentId);actions.archived|=!parent||parent->archived;}
    }else actions.archived=true;
    view->setHistoryActions(id,actions);
}
bool MainWindow::retryFailedTurn(const QString& id,const QString& userRowId,ConversationView* view){
    const QPointer<MainWindow> self(this);const QPointer<ConversationView> target(view);
    const bool side=view&&view!=m_transcript;
    // Refusals overwrite any "Queue:" text and always re-enable the button: a
    // rejected Retry changes no engine state, so no engine signal refreshes it.
    const auto refuse=[this,self,target,id,side](const QString& error){
        if(!self)return false;
        if(side){if(const auto pane=m_sideChats.value(id))pane->setError(error);}
        else if(id==m_currentId)m_status->setText(error);
        if(self&&target)pushHistoryActions(id,target);
        return false;
    };
    if(m_sessionTransition)return refuse("Wait for conversation storage to finish before retrying.");
    auto* session=find(id);if(!session||session->archived)return refuse("Archived or removed chats cannot be retried.");
    if(gitPathBusy(session->workspace))return refuse("Wait for the workspace operation to finish before retrying.");
    if(const auto* host=m_toolHosts.value(id);(host&&host->mutationBusy())||(m_mutationUndoAgent==id&&!m_mutationUndoId.isEmpty()))return refuse("Wait for file changes to finish before retrying.");
    const QPointer<AgentEngine> engine(engineFor(id));
    if(!self)return false;
    session=find(id);
    if(!engine||!session||session->archived)return refuse("Archived or removed chats cannot be retried.");
    if(!session->sideChatParentId.isEmpty()){const auto* parent=find(session->sideChatParentId);if(!parent||parent->archived)return refuse("Archived or removed chats cannot be retried.");}
    if(engine->isBusy())return refuse("Wait for the agent to finish before retrying.");
    // Retry uses the chat's saved selection, never an unsent composer choice.
    // The engine setters below can save through the committer, which replaces
    // m_sessions, so nothing reads through this pointer after this point.
    const ModelSelection selection{session->providerId,session->model,session->presetId,{},{}};ProviderConfig provider;
    const QString workspace=session->workspace;const bool alwaysAllow=session->alwaysAllow;
    session=nullptr;
    if(!selection.resolve(m_providers,&provider))return refuse("Select an available model before retrying.");
    engine->setModelChain(selectedModelChain(selection,m_providers,m_settings->preference("modelPresets").toArray(),m_settings->preference("modelCatalogs").toObject()),selection.presetId);
    if(!self)return false;
    if(!engine)return refuse("Archived or removed chats cannot be retried.");
    engine->setWorkspace(workspace);
    if(!self)return false;
    if(!engine)return refuse("Archived or removed chats cannot be retried.");
    engine->setAlwaysAllow(alwaysAllow);
    if(!self)return false;
    if(const auto* current=find(id);!engine||!current||current->archived)return refuse("Archived or removed chats cannot be retried.");
    if(const QPointer<AgentToolHost> host=m_toolHosts.value(id)){
        QString error;if(!host->setWorkspace(workspace,&error))return refuse(error);
        if(!self)return false;
        if(!engine)return refuse("Archived or removed chats cannot be retried.");
    }
    // Cleared before dispatch so errors the start reports synchronously (turn
    // context configuration, an immediate fail()) remain visible.
    if(side){if(const auto pane=m_sideChats.value(id))pane->setError({});}
    else if(id==m_currentId)m_status->clear();
    // The composer draft and draft attachments are never read or cleared.
    QString error;const bool accepted=engine->retryFailedTurn(userRowId,&error);
    if(!self)return accepted;
    if(!accepted)return refuse(error.isEmpty()?QString("The failed turn could not be retried."):error);
    if(side)refreshSideChat(id);
    else if(id==m_currentId)renderConversation();
    if(self)refreshAgents();
    return true;
}
}
