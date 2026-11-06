#include "ConversationView.h"
#include "MainWindow.h"
#include "browser/BrowserToolBridge.h"
#include "browser/browserwidget.h"
#include "core/AgentEngine.h"
#include "editor/TextFile.h"
#include "runtime/AgentToolHost.h"
#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QVBoxLayout>

namespace ade {
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
}
void MainWindow::beginTranscriptLinkTurn(const QString& id) {
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
    for (int i = 0; i < m_tabs->count(); ++i) {
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
}
void MainWindow::routeTranscriptLink(const QString& id, const QString& rowId, const QString& partId,
                                     const QUrl& url) {
    if (id != m_currentId || m_transcript->conversationId() != id)
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
    if (!self || id != m_currentId || m_transcript->conversationId() != id)
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
    if (!self || !host || id != m_currentId || !action.isCurrent(id)) {
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
        if (!self || !host || !page || id != m_currentId || !action.isCurrent(id))
            return;
        revealContent();
        return;
    }
    case TranscriptLinkResolver::Kind::File:
        break;
    default:
        if (!error.isEmpty())
            m_status->setText(error);
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
} // namespace ade
