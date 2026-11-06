#include "FileChangesPanel.h"
#include "MainWindow.h"
#include "core/AgentEngine.h"
#include "core/ToolExecutor.h"
#include "editor/EditorWidget.h"
#include "runtime/AgentToolHost.h"
#include <QLabel>
#include <QDir>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <QUuid>
namespace ade {
void MainWindow::showFileChanges() {
  const QPointer<MainWindow> self(this);
  m_fileChangesAgent =
      m_tabs->currentWidget()
          ? m_tabs->currentWidget()->property("sideChatAgentId").toString()
          : QString();
  if (!m_fileChanges) {
    auto *page = new FileChangesPanel(m_tabs);
    m_fileChanges = page;
    connect(page, &FileChangesPanel::refreshRequested, this,
            &MainWindow::refreshFileChanges);
    connect(page, &FileChangesPanel::undoRequested, this,
            &MainWindow::undoFileChanges);
    connect(page, &FileChangesPanel::recoveryRequested, this,
            &MainWindow::recoverFileChanges);
    connect(page, &FileChangesPanel::cancelRequested, this,
            [this](const QString &id) {
              if (id == m_mutationUndoAgent)
                if (auto *host = m_toolHosts.value(id))
                  host->cancelUndo(m_mutationUndoId);
            });
    const QPointer<FileChangesPanel> guard(page);
    int index = -1;
    {
      const QSignalBlocker blocked(m_tabs);
      index = m_tabs->addTab(page, "File changes");
      m_tabs->setCurrentIndex(index);
    }
    if (!self || !guard)
      return;
    emit m_tabs->currentChanged(index);
    if (!self || !guard)
      return;
  } else
    m_tabs->setCurrentWidget(m_fileChanges);
  if (!self)
    return;
  revealContent();
  refreshFileChanges();
}
void MainWindow::refreshFileChanges() {
  if (!m_fileChanges)
    return;
  const auto id =
      m_fileChangesAgent.isEmpty() ? m_currentId : m_fileChangesAgent;
  m_fileChanges->setConversationTitle(find(id) ? find(id)->title : QString());
  if (!find(id)) {
    m_fileChanges->setHistory({}, {}, true);
    return;
  }
  const QPointer<MainWindow> self(this);
  engineFor(id);
  if (!self || !find(id) || !m_fileChanges)
    return;
  auto *host = m_toolHosts.value(id);
  auto *engine = m_engines.value(id);
  QString error;
  const auto history = host ? host->mutationHistory(&error) : QJsonArray{};
  m_fileChanges->setHistory(
      id, history,
      (engine && (engine->isBusy() || !engine->queuedMessages().isEmpty())) ||
          !m_mutationUndoId.isEmpty(),
      error);
  if (m_mutationUndoAgent == id && !m_mutationUndoId.isEmpty())
    m_fileChanges->setOperation(id, true, "Restoring selected changes…");
}
void MainWindow::connectMutationControls(const QString &id,
                                         AgentToolHost *host) {
  if (auto *engine = m_engines.value(id)) {
    const QPointer<AgentEngine> owner(engine);
    const auto update = [this, id, owner] {
      if (owner && m_fileChanges && m_fileChanges->agentId() == id)
        m_fileChanges->setBusy(owner->isBusy() ||
                               !owner->queuedMessages().isEmpty() ||
                               !m_mutationUndoId.isEmpty());
    };
    connect(engine, &AgentEngine::busyChanged, this, update);
    connect(engine, &AgentEngine::queuedMessagesChanged, this, update);
  }
  connect(host, &AgentToolHost::mutationUndoCompleted, this,
          [this, id](const QString &request, const QString &output, bool error,
                     const QJsonObject &) {
            if (m_mutationUndoAgent != id || m_mutationUndoId != request)
              return;
            m_mutationUndoAgent.clear();
            m_mutationUndoId.clear();
            if (m_fileChanges && m_fileChanges->agentId() == id) {
              refreshFileChanges();
              if (m_fileChanges)
                m_fileChanges->setOperation(
                    id, false,
                    error ? "Undo could not complete: " + output : output);
            }
            updateComposerState();
            updateGitContext();
          });
  connect(host, &QObject::destroyed, this, [this, id] {
    if (m_mutationUndoAgent == id) {
      m_mutationUndoAgent.clear();
      m_mutationUndoId.clear();
      updateComposerState();
    }
  });
  connect(host, &AgentToolHost::fileChanged, this, [this, id] {
    if(id==m_currentId)refreshWorkspaceChanges();
    if (m_fileChanges && m_fileChanges->agentId() == id)
      QTimer::singleShot(0, this, &MainWindow::refreshFileChanges);
  });
}
void MainWindow::undoFileChanges(const QString &id, const QStringList &ids) {
  if (!m_fileChanges || m_fileChanges->agentId() != id || ids.isEmpty() ||
      !m_mutationUndoId.isEmpty())
    return;
  auto *session = find(id);
  const QPointer<MainWindow> self(this);
  const QPointer<FileChangesPanel> panel=m_fileChanges;
  const QPointer<AgentToolHost> host=m_toolHosts.value(id);
  auto *engine = m_engines.value(id);
  if (!session || session->archived || !host ||
      (engine && (engine->isBusy() || !engine->queuedMessages().isEmpty())) ||
      gitPathBusy(session->workspace))
    return;
  QString error;
  QStringList selectedPaths;
  if(!panel->pathsForMutations(ids,&selectedPaths,&error)){
    panel->setOperation(id,false,error);return;
  }
  QSet<QString> paths;
  const auto physicalPath=[](const QString& path){
    const QFileInfo info(path);const auto canonical=info.canonicalFilePath();
    if(!canonical.isEmpty())return QDir::cleanPath(canonical).toCaseFolded();
    const auto parent=QFileInfo(info.absolutePath()).canonicalFilePath();
    return QDir::cleanPath(parent.isEmpty()?info.absoluteFilePath():parent+'/'+info.fileName()).toCaseFolded();
  };
  // The panel rows originate in exact owner-scoped journal reads. Use their
  // original roots, including old workspaces and apps, never today's ws1 alias.
  for(const auto& path:selectedPaths){
    if(gitPathBusy(path)){panel->setOperation(id,false,"Wait for the operation in the original workspace to finish.");return;}
    paths.insert(physicalPath(path));
  }
  for (auto *editor : m_tabs->findChildren<EditorWidget *>())
    if (editor->isModified() &&
        paths.contains(physicalPath(editor->filePath()))) {
      if (m_fileChanges)
        m_fileChanges->setOperation(id, false,
                                    "Save or close your unsaved edits to " +
                                        editor->filePath() +
                                        " before undoing this change.");
      return;
    }
  m_mutationUndoAgent = id;
  const auto requestId=QUuid::createUuid().toString(QUuid::WithoutBraces);
  m_mutationUndoId = requestId;
  const auto abandon=[self,panel,id,requestId]{
    if(!self||self->m_mutationUndoAgent!=id||self->m_mutationUndoId!=requestId)return;
    self->m_mutationUndoAgent.clear();self->m_mutationUndoId.clear();
    if(panel&&self->m_fileChanges==panel)panel->setOperation(id,false,"The conversation changed before undo could start.");
    if(self)self->updateComposerState();
  };
  if (m_fileChanges)
    m_fileChanges->setOperation(id, true, "Restoring selected changes…");
  if(!self||!panel||m_fileChanges!=panel||!host||m_toolHosts.value(id)!=host){abandon();return;}
  updateComposerState();
  if(!self||!panel||m_fileChanges!=panel||!host||m_toolHosts.value(id)!=host
      ||panel->agentId()!=id||m_mutationUndoAgent!=id||m_mutationUndoId!=requestId){abandon();return;}
  const auto* live=find(id);const auto* running=m_engines.value(id);
  if(!live||live->archived||(running&&(running->isBusy()||!running->queuedMessages().isEmpty()))||gitPathBusy(live->workspace)){
    abandon();return;
  }
  for(const auto& path:selectedPaths)if(gitPathBusy(path)){abandon();return;}
  host->undoMutations(requestId, ids);
}
void MainWindow::recoverFileChanges(const QString &id) {
  if (!m_fileChanges || m_fileChanges->agentId() != id ||
      !m_mutationUndoId.isEmpty())
    return;
  const auto *session = find(id);
  auto *host = m_toolHosts.value(id);
  auto *engine = m_engines.value(id);
  if (!session || session->archived || !host ||
      (engine && (engine->isBusy() || !engine->queuedMessages().isEmpty())) ||
      gitPathBusy(session->workspace))
    return;
  for (auto *editor : m_tabs->findChildren<EditorWidget *>())
    if (editor->isModified()) {
      if (m_fileChanges)
        m_fileChanges->setOperation(
            id, false, "Save or close unsaved editor changes before recovery.");
      return;
    }
  m_mutationUndoAgent = id;
  m_mutationUndoId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (m_fileChanges)
    m_fileChanges->setOperation(id, true, "Recovering interrupted changes…");
  updateComposerState();
  host->recoverMutations(m_mutationUndoId);
}
} // namespace ade
