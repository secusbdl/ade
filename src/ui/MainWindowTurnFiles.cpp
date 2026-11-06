#include "MainWindow.h"
#include "ConversationView.h"
#include "TurnFileEdits.h"
#include "FileChangesPanel.h"
#include "FileDiffView.h"
#include "core/AgentEngine.h"
#include "runtime/AgentToolHost.h"
#include <QFileInfo>
#include <QLabel>
#include <QPointer>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QVBoxLayout>
#include <atomic>
namespace ade {
void MainWindow::connectTurnFileControls(ConversationView* transcript){
    if(!transcript)return;
    const QPointer<MainWindow> self(this);const QPointer<ConversationView> view(transcript);
    const auto valid=[self,view](const QString& chat,const QString& row,const QStringList& ids,bool action){
        if(!self||!view||!self->visibleTranscript(chat,view,action)||view->turnMutationIds(row)!=ids||ids.isEmpty())return false;
        const auto* session=self->find(chat);if(!session||session->archived)return false;
        const auto* engine=self->m_engines.value(chat);
        const auto projected=engine?engine->displayMessages():AgentEngine::displayHistory(session->messages);
        const auto selection=TurnFileEdits::selections(projected).value(row);
        return !selection.incomplete&&selection.mutationIds==ids;
    };
    const auto request=[this,self,view,valid](quint64 viewRequest,const QString& chat,const QString& row,const QStringList& ids,const QString& path,bool review,bool current){
        const bool action=!viewRequest;
        if(!valid(chat,row,ids,action))return;
        engineFor(chat);if(!self||!valid(chat,row,ids,action))return;
        QPointer<AgentToolHost> host=m_toolHosts.value(chat);if(!host)return;
        static std::atomic<quint64> next{0};const auto requestId=++next;
        auto connection=std::make_shared<QMetaObject::Connection>();
        *connection=connect(host,&AgentToolHost::turnFilesReady,this,[this,self,view,host,valid,connection,requestId,viewRequest,chat,row,ids,path,review,action,current](quint64 completed,const QJsonObject& summary,const QString& error){
            if(completed!=requestId)return;
            QObject::disconnect(*connection);
            if(!self||!host||self->m_toolHosts.value(chat)!=host||!valid(chat,row,ids,action))return;
            if(viewRequest){view->provideTurnFileEdits(viewRequest,summary,error);return;}
            if(!error.isEmpty()){m_status->setText(error);return;}
            // Every click queries the journal again. Rendered/cached metadata
            // does not authorize access to another chat or a replacement row.
            if(summary["agentId"]!=chat||!TurnFileEdits::validSummary(summary))return;
            if(current){
                const auto* session=find(chat);if(!session||session->workspace.isEmpty()){m_status->setText("Connect this chat to a workspace to open current changes.");return;}
                const auto workspace=session->workspace;
                if(path.isEmpty()){showWorkspaceChanges(workspace,chat);return;}
                bool member=false;for(const auto& file:summary["files"].toArray())if(file.toObject()["path"]==path)member=true;
                if(!member||!path.startsWith("ws1/")){m_status->setText("Current Git changes are available for the connected workspace. Use Open saved diff for this recorded mount.");return;}
                const auto stillValid=[self,host,valid,chat,row,ids,workspace]{return self&&host&&self->m_toolHosts.value(chat)==host&&valid(chat,row,ids,false)&&self->find(chat)&&self->find(chat)->workspace==workspace;};
                openWorkspaceChangedFile(workspace,path.mid(4),stillValid,chat);return;
            }
            if(review){
                showFileChanges();if(!self||!view||!host||m_toolHosts.value(chat)!=host||!m_fileChanges||!find(chat)||!valid(chat,row,ids,false))return;
                const QPointer<FileChangesPanel> panel=m_fileChanges;
                m_fileChangesAgent=chat;m_fileChanges->setConversationTitle(find(chat)->title);
                const auto* engine=m_engines.value(chat);const bool busy=(engine&&(engine->isBusy()||!engine->queuedMessages().isEmpty()))||host->mutationBusy()||gitPathBusy(find(chat)->workspace);
                m_fileChanges->setHistory(chat,summary["records"].toArray(),busy,"Saved changes from this turn. Undo checks the current files before restoring them.");
                if(!self||!panel||m_fileChanges!=panel||!host||m_toolHosts.value(chat)!=host||!valid(chat,row,ids,false))return;
                panel->selectMutations(ids);return;
            }
            bool member=false;for(const auto& file:summary["files"].toArray())if(file.toObject()["path"]==path)member=true;if(!member)return;
            const auto preview=summary["preview"].toObject();
            auto* page=new QWidget(m_tabs);page->setObjectName("savedTurnDiff");page->setProperty("savedMutationAgent",chat);page->setProperty("savedMutationIds",ids);page->setProperty("savedMutationPath",path);
            auto* layout=new QVBoxLayout(page);auto* title=new QLabel("Saved change · "+path,page);title->setTextFormat(Qt::PlainText);title->setWordWrap(true);layout->addWidget(title);
            auto* hint=new QLabel("Exact saved state before and after this turn. Current working files are unchanged.",page);hint->setTextFormat(Qt::PlainText);hint->setWordWrap(true);hint->setProperty("muted",true);layout->addWidget(hint);
            if(preview.contains("unavailable")){auto* message=new QLabel(preview["unavailable"].toString(),page);message->setTextFormat(Qt::PlainText);message->setWordWrap(true);layout->addWidget(message);}
            else{auto* diff=new FileDiffView(page);diff->setObjectName("savedTurnDiffView");diff->setDiff(preview);layout->addWidget(diff,1);}
            const QPointer<QWidget> pageGuard(page);int index=-1;{const QSignalBlocker blocker(m_tabs);index=m_tabs->addTab(page,"Saved diff · "+QFileInfo(path).fileName());m_tabs->setCurrentIndex(index);}
            if(!self||!pageGuard||m_tabs->indexOf(pageGuard)<0)return;
            emit m_tabs->currentChanged(index);if(self&&pageGuard&&m_tabs->indexOf(pageGuard)>=0)revealContent();
        });
        host->requestTurnFiles(requestId,ids,path);
    };
    connect(transcript,&ConversationView::turnFileEditsRequested,this,[request](quint64 id,const QString& chat,const QString& row,const QStringList& mutations){request(id,chat,row,mutations,{},false,false);});
    connect(transcript,&ConversationView::turnFileRequested,this,[request](const QString& chat,const QString& row,const QStringList& mutations,const QString& path){request(0,chat,row,mutations,path,false,true);});
    connect(transcript,&ConversationView::turnFilesReviewRequested,this,[request](const QString& chat,const QString& row,const QStringList& mutations){request(0,chat,row,mutations,{},false,true);});
    connect(transcript,&ConversationView::savedTurnFileRequested,this,[request](const QString& chat,const QString& row,const QStringList& mutations,const QString& path){request(0,chat,row,mutations,path,false,false);});
    connect(transcript,&ConversationView::turnUndoReviewRequested,this,[request](const QString& chat,const QString& row,const QStringList& mutations){request(0,chat,row,mutations,{},true,false);});
}
}
