#include "MainWindow.h"
#include "SideChatPane.h"
#include "ui/ChatSurface.h"
#include "ui/Composer.h"
#include "ui/ConversationView.h"
#include "ui/ModelSelection.h"
#include "ui/QuestionPanel.h"
#include "ui/MessageQueuePanel.h"
#include "ui/SettingsPage.h"
#include "ui/Icons.h"
#include "attachments/AttachmentStore.h"
#include "core/AgentEngine.h"
#include "runtime/AgentToolHost.h"
#include <QBuffer>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>

namespace ade {
void MainWindow::initializeSideChats(){
    if(findChild<QToolButton*>("openSideChatButton"))return;
    auto add=[this](const QString& name,QWidget* parent){auto* button=new QToolButton(parent);button->setObjectName(name);button->setIcon(icon("branch"));button->setToolTip("Open side chat");button->setAccessibleName("Open side chat");button->setFixedSize(26,26);connect(button,&QToolButton::clicked,this,[this]{createSideChat();});return button;};
    m_chat->headerLayout()->addWidget(add("openSideChatButton",m_chat));
    if(auto* corner=m_tabs->cornerWidget();corner&&corner->layout())corner->layout()->addWidget(add("newSideChatTabButton",corner));
    refreshSideChats();
}
void MainWindow::refreshSideChats(){
    if(!findChild<QToolButton*>("openSideChatButton"))return;
    const QPointer<MainWindow> self(this);QStringList active;
    for(const auto& session:m_sessions){if(session.sideChatParentId.isEmpty()||session.archived)continue;const auto* parent=find(session.sideChatParentId);if(parent&&!parent->archived)active.append(session.id);}
    const auto existing=m_sideChats.keys();for(const auto& id:existing)if(!active.contains(id)){closeSideChatPane(id);if(!self)return;}
    const QPointer<QWidget> selected=m_tabs->currentWidget();
    for(const auto& id:active){if(!m_sideChats.value(id))openSideChat(id);else refreshSideChat(id);if(!self)return;}
    if(selected&&m_tabs->indexOf(selected)>=0)m_tabs->setCurrentWidget(selected);
}
void MainWindow::refreshSideChat(const QString& id){
    const QPointer<SideChatPane> pane=m_sideChats.value(id);const auto* session=find(id);if(!pane||!session)return;const auto* engine=m_engines.value(id);const Conversation snapshot=*session;
    QJsonArray models;for(const auto& selection:availableModelSelections(m_providers,m_settings->preference("modelCatalogs").toObject(),m_settings->preference("modelPresets").toArray()))models.append(selection.toJson());
    const QPointer<MainWindow> self(this);updateTranscriptLinkRows(id);
    pane->setState(snapshot,engine?engine->displayMessages():AgentEngine::displayHistory(snapshot.messages),engine&&engine->isBusy(),models,engine?engine->pendingQuestion():QJsonObject(),engine?engine->queuedMessages():snapshot.queuedMessages);
    if(!self||!pane)return;
    pane->setUiScale(m_settings->preference("personalization.uiZoom").toInt(100)/100.0);const int index=m_tabs->indexOf(pane);if(index>=0){m_tabs->setTabText(index,snapshot.title.left(36));m_tabs->setTabToolTip(index,snapshot.title);}
}
void MainWindow::openSideChat(const QString& id){
    const auto* session=find(id);if(!session||session->archived||session->sideChatParentId.isEmpty())return;const auto* parent=find(session->sideChatParentId);if(!parent||parent->archived)return;
    if(const auto existing=m_sideChats.value(id)){m_tabs->setCurrentWidget(existing);revealContent();return;}
    const auto initialTitle=session->title;
    const QPointer<MainWindow> self(this);auto* rawPane=new SideChatPane(id,m_tabs);const QPointer<SideChatPane> pane(rawPane);m_sideChats.insert(id,pane);
    const QPointer<AgentEngine> engine(engineFor(id));if(!self||!pane||!engine)return;
    initializeComposerCompletions(pane->composer(),id);
    connect(pane,&QObject::destroyed,this,[this,id,pane]{if(m_sideChats.value(id)==pane)m_sideChats.remove(id);});
    auto currentEngine=[this,id,pane]()->AgentEngine*{const auto* chat=find(id);if(!pane||m_sideChats.value(id)!=pane||!chat||chat->archived||chat->sideChatParentId.isEmpty())return nullptr;const auto* parent=find(chat->sideChatParentId);return parent&&!parent->archived?m_engines.value(id):nullptr;};
    auto* refresh=new QTimer(pane);refresh->setInterval(16);refresh->setSingleShot(true);connect(refresh,&QTimer::timeout,this,[this,id]{refreshSideChat(id);});
    auto schedule=[refresh]{if(!refresh->isActive())refresh->start();};connect(engine,&AgentEngine::displayChanged,pane,schedule);connect(engine,&AgentEngine::queuedMessagesChanged,pane,schedule);connect(engine,&AgentEngine::questionChanged,pane,schedule);connect(engine,&AgentEngine::busyChanged,pane,[schedule](bool){schedule();});connect(engine,&AgentEngine::finished,pane,schedule);
    connect(engine,&AgentEngine::failed,pane,[pane](const QString& error){if(pane)pane->setError(error);});connect(engine,&AgentEngine::queueError,pane,[pane](const QString& error){if(pane)pane->setError("Queue: "+error);});
    connect(engine,&AgentEngine::questionError,pane,[pane,id](const QString& token,const QString& error){if(pane)pane->questions()->setError(id,token,error);});
    auto* draftSave=new QTimer(pane);draftSave->setSingleShot(true);draftSave->setInterval(250);connect(draftSave,&QTimer::timeout,this,[this,pane]{if(!pane)return;QString error;if(!m_store.save(m_sessions,&error))pane->setError("Could not save draft: "+error);});
    connect(pane,&SideChatPane::draftChanged,this,[this,id,pane,draftSave](const QString& chat,const QString& text){if(chat!=id||m_sideChats.value(id)!=pane)return;if(auto* session=find(id)){session->draft=text;draftSave->start();}});
    connect(pane,&SideChatPane::submitted,this,&MainWindow::sendSideChat);connect(pane,&SideChatPane::stopRequested,this,[currentEngine](const QString&){if(auto* active=currentEngine())active->cancel();});connect(pane,&SideChatPane::promoteRequested,this,[this](const QString& chat){promoteSideChat(chat);});
    connect(pane,&SideChatPane::changesRequested,this,[this,id,pane](const QString& chat){if(chat==id&&pane&&m_tabs->currentWidget()==pane)showFileChanges();});
    connect(pane,&SideChatPane::modelChanged,this,[this,id,pane,currentEngine](const QString& chat,const QJsonObject& value){auto* active=currentEngine();if(chat!=id||!active||active->isBusy())return;const auto selected=ModelSelection::fromJson(value);ProviderConfig provider;if(!selected.resolve(m_providers,&provider)){if(pane)pane->setError("Select an available model.");refreshSideChat(id);return;}auto next=m_sessions;for(auto& session:next)if(session.id==id){session.providerId=selected.providerId;session.model=selected.modelId;session.presetId=selected.presetId;}const QPointer<MainWindow> self(this);if(!commitConversationState(next,m_store.pendingCleanup())&&pane)pane->setError("Could not save the selected model.");if(self)refreshSideChat(id);});
    connect(pane,&SideChatPane::permissionChanged,this,[this,id,pane,currentEngine](const QString& chat,bool allowed){auto* active=currentEngine();if(chat!=id||!active||active->isBusy())return;auto next=m_sessions;for(auto& session:next)if(session.id==id)session.alwaysAllow=allowed;const QPointer<MainWindow> self(this);if(!commitConversationState(next,m_store.pendingCleanup())&&pane)pane->setError("Could not save the approval setting.");if(self)refreshSideChat(id);});
    auto saveAttachments=[this,id,pane](const QJsonArray& attachments){auto next=m_sessions;bool found=false;for(auto& chat:next)if(chat.id==id){chat.draftAttachments=attachments;found=true;}if(!found||!pane)return;const QPointer<MainWindow> self(this);if(!commitConversationState(next,m_store.pendingCleanup()))pane->setError("Could not save the attachment change.");if(self)refreshSideChat(id);};
    connect(pane,&SideChatPane::removeAttachmentRequested,this,[this,id,saveAttachments](const QString& chat,int index){const auto* session=find(id);if(chat!=id||!session||index<0||index>=session->draftAttachments.size())return;auto refs=session->draftAttachments;refs.removeAt(index);saveAttachments(refs);});
    connect(pane,&SideChatPane::filesAttached,this,[this,id,pane,currentEngine,saveAttachments](const QString& chat,const QStringList& paths){if(chat!=id||!currentEngine())return;const auto* session=find(id);if(!session)return;auto refs=session->draftAttachments;AttachmentMounts mounts;if(!session->workspace.isEmpty())mounts.insert("ws1",session->workspace);AttachmentStore store(m_store.directory());QStringList errors;for(const auto& path:paths){if(refs.size()>=32){errors.append("A message can contain at most 32 attachments.");break;}QString error;const auto ref=store.importFile(id,path,mounts,&error);if(!error.isEmpty()){errors.append(error);continue;}bool duplicate=false;for(const auto& value:refs)duplicate|=value.toObject()["path"]==ref.path;if(!duplicate)refs.append(ref.toJson());}saveAttachments(refs);if(pane&&!errors.isEmpty())pane->setError(errors.join('\n'));});
    connect(pane,&SideChatPane::imageAttached,this,[this,id,pane,currentEngine,saveAttachments](const QString& chat,const QImage& image){if(chat!=id||!currentEngine()||image.isNull())return;const auto* session=find(id);if(!session)return;if(session->draftAttachments.size()>=32||qint64(image.width())*image.height()>64*1024*1024){pane->setError("The image or attachment count exceeds the supported limit.");return;}auto refs=session->draftAttachments;QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);if(!image.save(&buffer,"PNG")){pane->setError("The pasted image could not be encoded.");return;}QString error;const auto ref=AttachmentStore(m_store.directory()).write(id,"Pasted image.png",bytes,&error);if(!error.isEmpty()){pane->setError(error);return;}refs.append(ref.toJson());saveAttachments(refs);});
    connect(pane->questions(),&QuestionPanel::submitStep,this,[id,currentEngine](const QString& chat,const QString& token,int step,const QJsonObject& answers){if(chat!=id)return;if(auto* active=currentEngine())active->submitQuestionStep(token,step,answers);});
    connect(pane->questions(),&QuestionPanel::draftChanged,this,[id,currentEngine](const QString& chat,const QString& token,int step,const QJsonObject& answers){if(chat!=id)return;if(auto* active=currentEngine())active->updateQuestionDraft(token,step,answers);});
    connect(pane->questions(),&QuestionPanel::goBack,this,[id,currentEngine](const QString& chat,const QString& token,int step){if(chat!=id)return;if(auto* active=currentEngine())active->goBackQuestion(token,step);});
    connect(pane->questions(),&QuestionPanel::cancelled,this,[id,currentEngine](const QString& chat,const QString& token,const QJsonObject& answers){if(chat!=id)return;if(auto* active=currentEngine())active->cancelQuestion(token,answers);});
    connect(pane->queue(),&MessageQueuePanel::editRequested,this,[id,currentEngine](const QString& chat,const QString& queued,const QString& text){if(chat!=id)return;if(auto* active=currentEngine())active->editQueuedMessage(queued,text);});
    connect(pane->queue(),&MessageQueuePanel::removeRequested,this,[id,currentEngine](const QString& chat,const QString& queued){if(chat!=id)return;if(auto* active=currentEngine())active->removeQueuedMessage(queued);});
    connect(pane->queue(),&MessageQueuePanel::moveRequested,this,[id,currentEngine](const QString& chat,const QString& queued,int index){if(chat!=id)return;if(auto* active=currentEngine())active->moveQueuedMessage(queued,index);});
    auto resume=[this,id,pane,currentEngine](const QString& chat,const QString& queued){if(chat!=id)return;auto* active=currentEngine();const auto* session=find(id);if(!active||!session)return;if(gitPathBusy(session->workspace)){pane->setError("Wait for the workspace operation before resuming queued messages.");return;}if(!active->isBusy()){const ModelSelection selected{session->providerId,session->model,session->presetId,{},{}};ProviderConfig provider;if(!selected.resolve(m_providers,&provider)){pane->setError("Select an available model before resuming the queue.");return;}active->setModelChain(selectedModelChain(selected,m_providers,m_settings->preference("modelPresets").toArray(),m_settings->preference("modelCatalogs").toObject()),selected.presetId);active->setAlwaysAllow(session->alwaysAllow);}if(queued.isEmpty())active->resumeQueue();else active->flushQueuedMessage(queued);};
    connect(pane->queue(),&MessageQueuePanel::resumeRequested,this,[resume](const QString& chat){resume(chat,{});});connect(pane->queue(),&MessageQueuePanel::sendNowRequested,this,resume);
    const QPointer<ConversationView> view=pane->transcript();
    connect(view,&ConversationView::scopedLinkActivated,this,[this,view](const QString& chat,const QString& row,const QString& part,const QUrl& url){if(view)routeTranscriptLink(chat,row,part,url,view);});
    connect(view,&ConversationView::imageRequested,this,[this,view](quint64 request,const QString& chat,const QString& row,const QString& part,const QUrl& url){if(view)provideTranscriptImage(request,chat,row,part,url,view);});
    connect(view,&ConversationView::approvalDecided,this,[this,id,view,currentEngine](const QString& chat,const QString& rowId,const QString& partId,const QString& call,bool allowed){auto* active=currentEngine();if(chat!=id||!active||!visibleTranscript(id,view))return;for(const auto& value:active->displayMessages()){const auto row=value.toObject();if(row["id"]!=rowId)continue;for(const auto& entry:row["parts"].toArray()){const auto part=entry.toObject();if(part["id"]==partId&&part["type"]=="tool"&&part["toolCallId"]==call&&part["state"]=="approval-requested"){active->approveTool(call,allowed);return;}}}});
    const QPointer<QTabWidget> tabs=m_tabs;int index=-1;
    {
        QSignalBlocker blocker(tabs);
        index=tabs->addTab(pane,icon("branch"),initialTitle.left(36));
        if(!self||!pane||!tabs){blocker.dismiss();return;}
        tabs->setCurrentIndex(index);
        if(!self||!pane||!tabs){blocker.dismiss();return;}
    }
    emit tabs->currentChanged(index);
    if(!self||!pane||!tabs||tabs->indexOf(pane)<0)return;
    refreshSideChat(id);if(!self||!pane)return;revealContent();if(self&&pane)pane->composer()->setFocus();
}
void MainWindow::closeSideChatPane(const QString& id){const QPointer<SideChatPane> pane=m_sideChats.take(id);if(!pane)return;const QPointer<MainWindow> self(this);const int index=m_tabs->indexOf(pane);if(index>=0)m_tabs->removeTab(index);if(pane)pane->deleteLater();if(self)refreshWorkbench();}
bool MainWindow::promoteSideChat(const QString& id){
    const auto* session=find(id);if(!session||session->sideChatParentId.isEmpty())return false;const auto* engine=m_engines.value(id);const auto* host=m_toolHosts.value(id);if((engine&&engine->isBusy())||(host&&host->mutationBusy())||(m_mutationUndoAgent==id&&!m_mutationUndoId.isEmpty())){if(auto pane=m_sideChats.value(id))pane->setError("Wait for this side chat to finish before moving it to the sidebar.");return false;}
    auto next=m_sessions;for(auto& chat:next)if(chat.id==id)chat.sideChatParentId.clear();if(!commitConversationState(next,m_store.pendingCleanup()))return false;const QPointer<MainWindow> self(this);closeSideChatPane(id);if(!self)return true;refreshAgents();selectConversation(id);return true;
}
bool MainWindow::discardSideChat(const QString& id){
    const auto* session=find(id);if(!session||session->sideChatParentId.isEmpty())return false;const auto* host=m_toolHosts.value(id);if((host&&host->mutationBusy())||(m_mutationUndoAgent==id&&!m_mutationUndoId.isEmpty())){if(auto pane=m_sideChats.value(id))pane->setError("Wait for file changes to finish before closing this side chat.");return false;}
    const QPointer<MainWindow> self(this);if(auto* engine=m_engines.value(id);engine&&engine->isBusy())engine->cancel();if(!self)return false;session=find(id);if(!session||session->sideChatParentId.isEmpty())return false;return deleteConversation(id);
}
void MainWindow::sendSideChat(const QString& id){
    const QPointer<SideChatPane> pane=m_sideChats.value(id);auto* session=find(id);if(!pane||!session||session->archived||session->sideChatParentId.isEmpty()||(pane->composer()->toPlainText().trimmed().isEmpty()&&session->draftAttachments.isEmpty()))return;
    if(gitPathBusy(session->workspace)){pane->setError("Wait for the workspace operation before sending.");return;}
    const QPointer<MainWindow> self(this);const QPointer<AgentEngine> engine(engineFor(id));if(!self||!pane||!engine)return;session=find(id);if(!session)return;
    const QPointer<AgentToolHost> host=m_toolHosts.value(id);if((host&&host->mutationBusy())||(m_mutationUndoAgent==id&&!m_mutationUndoId.isEmpty())){pane->setError("Wait for file changes to finish before sending.");return;}
    if(!engine->isBusy()){const ModelSelection selected{session->providerId,session->model,session->presetId,{},{}};ProviderConfig provider;if(!selected.resolve(m_providers,&provider)){pane->setError("Select an available model before sending.");return;}engine->setModelChain(selectedModelChain(selected,m_providers,m_settings->preference("modelPresets").toArray(),m_settings->preference("modelCatalogs").toObject()),selected.presetId);engine->setAlwaysAllow(session->alwaysAllow);engine->setWorkspace(session->workspace);if(host){QString error;if(!host->setWorkspace(session->workspace,&error)){pane->setError(error);return;}}}
    session=find(id);if(!session)return;QString error;const auto metadata=host?host->captureAttachments(session->draftAttachments,&error):QJsonObject();if(!error.isEmpty()){pane->setError(error);return;}
    Conversation previous=*session;previous.draft=pane->composer()->toPlainText();const auto draft=previous.draft;const auto history=engine->messages(),queue=engine->queuedMessages();session->draft.clear();session->draftAttachments={};pane->setError({});
    const bool accepted=engine->sendUserMessage(draft.trimmed(),metadata["attachments"].toArray(),metadata["pathReferences"].toObject(),metadata["pathMetadata"].toObject());if(!self||!pane||!engine)return;
    if(!accepted&&engine->messages()==history&&engine->queuedMessages()==queue){if(auto* rejected=find(id))*rejected=previous;QString saveError;if(!m_store.save(m_sessions,&saveError))pane->setError("Draft retained here, but could not be saved: "+saveError);refreshSideChat(id);return;}
    if(accepted&&pane->composer()->toPlainText()==draft){QSignalBlocker blocker(pane->composer());pane->composer()->clear();}refreshSideChat(id);if(self)refreshAgents();
}
}
