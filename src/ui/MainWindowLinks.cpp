#include "ConversationView.h"
#include "ImagePreview.h"
#include "TrustedImage.h"
#include "MainWindow.h"
#include "browser/BrowserToolBridge.h"
#include "browser/browserwidget.h"
#include "core/AgentEngine.h"
#include "editor/TextFile.h"
#include "runtime/AgentToolHost.h"
#include "links/TranscriptLinkProvenanceCodec.h"
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QImageReader>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QVBoxLayout>

namespace ade {
QJsonObject MainWindow::forkTranscriptLinkProvenance(const QString& sourceId,const QString& targetId,const QString& workspace) {
    auto* host=m_toolHosts.value(sourceId);if(!host)return {};
    TranscriptLinkOptions source;source.agentId=sourceId;source.dataDirectory=m_store.directory();source.lease=host->fileContextLease();source.mounts=host->fileContextMounts();
    const auto sourceScope=TranscriptLinkResolver::create(source);const auto original=sourceScope.mountBindings();
    TranscriptLinkOptions target;target.agentId=targetId;target.dataDirectory=m_store.directory();target.lease=FileReadSnapshotStore(m_store.directory()).acquire(targetId);
    if(!workspace.isEmpty())target.mounts.insert("ws1",workspace);
    const auto targetScope=TranscriptLinkResolver::create(target);const auto copied=targetScope.mountBindings();
    auto provenance=m_linkProvenance.value(sourceId);
    for(auto it=provenance.begin();it!=provenance.end();++it){
        const auto old=it->mountBindings;it->mountBindings={};
        if(!original["ws1"].toString().isEmpty()&&old["ws1"]==original["ws1"]&&!copied["ws1"].toString().isEmpty())it->mountBindings["ws1"]=copied["ws1"];
    }
    return encodeLinkProvenance(provenance);
}
void MainWindow::updateTranscriptLinkRows(const QString& id) {
    const auto* engine = m_engines.value(id);
    if (!engine)
        return;
    auto& known = m_linkKnownRows[id];
    for (const auto& value : engine->displayMessages()) {
        const auto row = value.toObject();
        const auto rowId = row["id"].toString();
        if (rowId.isEmpty() || known.contains(rowId))
            continue;
        known.insert(rowId);
        // Only rows first observed during a submission owned by this window get
        // current mount authority. Loading/forking old history never upgrades it.
        if (m_linkTurnContext.contains(id))
            m_linkProvenance[id].insert(rowId, m_linkTurnContext.value(id));
    }
    if(auto* session=find(id))session->transcriptLinkProvenance=encodeLinkProvenance(m_linkProvenance.value(id));
}
void MainWindow::beginTranscriptLinkTurn(const QString& id,bool acceptedUserMessage) {
    updateTranscriptLinkRows(id);
    m_linkTurnContext.remove(id);
    const auto* host = m_toolHosts.value(id);
    if (!host)
        return;
    TranscriptLinkOptions options;
    options.agentId = id;
    options.dataDirectory = m_store.directory();
    options.lease = host->fileContextLease();
    options.mounts = host->fileContextMounts();
    const auto scope = TranscriptLinkResolver::create(options);
    if (!scope.isCurrent(id))
        return;
    TranscriptLinkProvenance provenance;
    provenance.mountBindings = scope.mountBindings();
    provenance.browserSessionId = m_linkBrowserSessionId;
    for (int i = 0; i < m_tabs->count() && provenance.historicalTabUrls.size()<128; ++i) {
        const auto* page = m_tabs->widget(i);
        const auto tabId = page->property("browserTabId").toString();
        const auto* browser = page->findChild<BrowserWidget*>();
        if (tabId.isEmpty() || !browser || !m_browserBridge->canAccessTab(tabId, id))
            continue;
        if (browser->property("miniAppBrowser").toBool() ||
            !page->property("appInstanceId").toString().isEmpty() ||
            !page->property("appAgentId").toString().isEmpty())
            continue;
        const auto url = browser->currentUrl();
        if (url.isValid() && (url.scheme() == "https" || url.scheme() == "http") && !url.host().isEmpty() &&
            url.userInfo().isEmpty())
            provenance.historicalTabUrls.insert(tabId, url);
    }
    m_linkTurnContext.insert(id, provenance);
    if(acceptedUserMessage){
        if(const auto* engine=m_engines.value(id)){const auto rows=engine->displayMessages();if(!rows.isEmpty()){const auto row=rows.last().toObject();const auto rowId=row["id"].toString();if(row["role"]=="user"&&!rowId.isEmpty()){m_linkProvenance[id].insert(rowId,provenance);if(auto* session=find(id))session->transcriptLinkProvenance=encodeLinkProvenance(m_linkProvenance.value(id));}}}
    }
}
bool MainWindow::visibleTranscript(const QString& id,ConversationView* view,bool requireCurrentTab) const{
    if(!view||view->conversationId()!=id)return false;
    if(view==m_transcript)return id==m_currentId;
    for(int i=0;i<m_tabs->count();++i){const auto* page=m_tabs->widget(i);if(page&&page->property("sideChatAgentId").toString()==id&&page->isAncestorOf(view))return !requireCurrentTab||page==m_tabs->currentWidget();}
    return false;
}
void MainWindow::routeTranscriptLink(const QString& id, const QString& rowId, const QString& partId,
                                     const QUrl& url,ConversationView* target) {
    const QPointer<ConversationView> view=target?target:m_transcript;
    if (!visibleTranscript(id,view))
        return;
    const auto* session = find(id);
    if (!session)
        return;
    const auto* engine = m_engines.value(id);
    const auto rows = engine ? engine->displayMessages() : AgentEngine::displayHistory(session->messages);
    bool present = false;
    for (const auto& value : rows) {
        const auto row = value.toObject();
        if (row["id"] != rowId)
            continue;
        for (const auto& entry : row["parts"].toArray()) {
            const auto part = entry.toObject();
            if (part["id"] == partId && (part["type"] == "text" || part["type"] == "reasoning"))
                present = true;
        }
    }
    if (!present)
        return;
    const QPointer<MainWindow> self(this);
    engineFor(id);
    if (!self || !visibleTranscript(id,view))
        return;
    const QPointer<AgentToolHost> host = m_toolHosts.value(id);
    if (!host)
        return;
    TranscriptLinkOptions options;
    options.agentId = id;
    options.dataDirectory = m_store.directory();
    options.lease = host->fileContextLease();
    options.mounts = host->fileContextMounts();
    options.browserSessionId = m_linkBrowserSessionId;
    options.maxFileBytes=16*1024*1024;
    for(const auto& value:host->shells()->snapshot()["sessions"].toArray())options.liveShellIds.insert(value.toObject()["id"].toString());
    auto provenance = m_linkProvenance.value(id).value(rowId);
    QHash<QString, QPointer<QWidget>> liveTabs;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto* page = m_tabs->widget(i);
        const auto tabId = page->property("browserTabId").toString();
        auto* browser = page->findChild<BrowserWidget*>();
        if (tabId.isEmpty() || !browser)
            continue;
        if (!m_browserBridge->canAccessTab(tabId, id)) {
            provenance.historicalTabUrls.remove(tabId);
            continue;
        }
        options.liveTabIds.insert(tabId);
        liveTabs.insert(tabId, page);
        if (browser->property("miniAppBrowser").toBool() ||
            !page->property("appInstanceId").toString().isEmpty() ||
            !page->property("appAgentId").toString().isEmpty())
            provenance.historicalTabUrls.remove(tabId);
    }
    QString error;
    const auto scope = TranscriptLinkResolver::create(options, &error);
    if (!scope.isCurrent(id)) {
        m_status->setText(error);
        return;
    }
    for (const auto& value : rows)
        for (const auto& entry : value.toObject()["attachments"].toArray()) {
            const auto ref = entry.toObject();
            provenance.attachmentNames.insert(ref["path"].toString(), ref["originalFileName"].toString());
        }
    auto action = TranscriptLinkResolver::resolve(scope, id, url.toString(QUrl::FullyEncoded), provenance,
                                                  nullptr, &error);
    if (!self || !host || !visibleTranscript(id,view) || !action.isCurrent(id)) {
        if (self && !error.isEmpty())
            m_status->setText(error);
        return;
    }
    switch (action.kind) {
    case TranscriptLinkResolver::Kind::ExternalUrl:
        addBrowserTab(action.url);
        return;
    case TranscriptLinkResolver::Kind::CopyColor:
        QApplication::clipboard()->setText(action.color);
        return;
    case TranscriptLinkResolver::Kind::NoOp:
        return;
    case TranscriptLinkResolver::Kind::ActivateTab: {
        const auto page = liveTabs.value(action.tabId);
        if (!page || page->property("browserTabId").toString() != action.tabId || m_tabs->indexOf(page) < 0 ||
            !m_browserBridge->canAccessTab(action.tabId, id))
            return;
        m_tabs->setCurrentWidget(page);
        if (!self || !host || !page || !view || view->conversationId()!=id || !action.isCurrent(id))
            return;
        revealContent();
        return;
    }
    case TranscriptLinkResolver::Kind::ActivateShell:
        if(host->shells()->ownsSession(action.shellId))showAgentShell(id,action.shellId);
        return;
    case TranscriptLinkResolver::Kind::File:
        break;
    default:
        if (!error.isEmpty())
            m_status->setText(error);
        return;
    }
    QSize originalImageSize;
    const auto decodedImage=readTrustedImage(action.bytes,nullptr,&originalImageSize);
    if(!decodedImage.isNull()){
        auto* page=new ImagePreview(action.displayName,decodedImage,originalImageSize,m_tabs);page->setProperty("ownerConversationId",id);page->setProperty("attachmentPath",action.mountedPath);connect(host,&QObject::destroyed,page,&QObject::deleteLater);const QPointer<ImagePreview> preview(page);int index=-1;
        {const QSignalBlocker blocked(m_tabs);index=m_tabs->addTab(page,action.displayName);m_tabs->setCurrentIndex(index);}
        if(!self||!preview||!host||!action.isCurrent(id))return;
        emit m_tabs->currentChanged(index);
        if(self&&preview&&host&&action.isCurrent(id))revealContent();
        return;
    }
    if (!action.readOnly) {
        // The action owns non-delete-sharing directory/file handles until this
        // synchronous editor load returns. Release before any later atomic save.
        openFile(action.canonicalPath);
        return;
    }
    TextFile decoded;
    if (!TextFileIO::decode(action.bytes, &decoded, &error)) {
        m_status->setText("Read-only attachment preview is currently limited to supported text files: " +
                          error);
        return;
    }
    auto* page = new QWidget;
    page->setObjectName("transcriptAttachmentPreview");
    page->setProperty("attachmentPath", action.mountedPath);
    page->setProperty("ownerConversationId", id);
    auto* layout = new QVBoxLayout(page);
    auto* name = new QLabel(action.displayName + " (read-only)", page);
    name->setTextFormat(Qt::PlainText);
    layout->addWidget(name);
    auto* text = new QPlainTextEdit(page);
    text->setObjectName("transcriptAttachmentText");
    text->setReadOnly(true);
    text->setPlainText(decoded.text);
    layout->addWidget(text);
    connect(host, &QObject::destroyed, page, &QObject::deleteLater);
    const QPointer<QWidget> preview(page);
    const auto index = m_tabs->addTab(page, action.displayName);
    if (!self || !preview || !host || !action.isCurrent(id))
        return;
    m_tabs->setCurrentIndex(index);
    if (!self || !preview || !host || !action.isCurrent(id))
        return;
    revealContent();
}
void MainWindow::provideTranscriptImage(quint64 requestId,const QString& id,const QString& rowId,const QString& partId,const QUrl& url,ConversationView* target) {
    const QPointer<ConversationView> view=target?target:m_transcript;
    if(!visibleTranscript(id,view,false))return;
    const auto* session=find(id);if(!session)return;
    const auto* engine=m_engines.value(id);const auto rows=engine?engine->displayMessages():AgentEngine::displayHistory(session->messages);
    bool present=false;
    for(const auto& value:rows){const auto row=value.toObject();if(row["id"]!=rowId)continue;for(const auto& entry:row["parts"].toArray()){const auto part=entry.toObject();if(part["id"]==partId&&(part["type"]=="text"||part["type"]=="reasoning"))present=true;}}
    if(!present)return;
    // Loading a row never creates new mount authority. Restore only the host's
    // durable provenance, and revalidate every image through the same resolver.
    const QPointer<MainWindow> self(this);engineFor(id);if(!self||!visibleTranscript(id,view,false))return;
    const QPointer<AgentToolHost> host=m_toolHosts.value(id);if(!host)return;
    TranscriptLinkOptions options;options.agentId=id;options.dataDirectory=m_store.directory();options.lease=host->fileContextLease();options.mounts=host->fileContextMounts();options.maxFileBytes=16*1024*1024;
    QString error;const auto scope=TranscriptLinkResolver::create(options,&error);
    const auto action=TranscriptLinkResolver::resolve(scope,id,url.toString(QUrl::FullyEncoded),m_linkProvenance.value(id).value(rowId),nullptr,&error);
    if(!self||!host||!visibleTranscript(id,view,false))return;
    if(action.kind!=TranscriptLinkResolver::Kind::File||!action.isCurrent(id)){view->provideImage(requestId,{},error.isEmpty()?"Image is not a trusted local file.":error);return;}
    const auto image=readTrustedImage(action.bytes,&error);
    if(self&&host&&visibleTranscript(id,view,false))view->provideImage(requestId,image,error);
}
} // namespace ade
