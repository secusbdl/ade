#include "FileChangesPanel.h"
#include "MainWindow.h"
#include "core/AgentEngine.h"
#include "core/ToolExecutor.h"
#include "editor/EditorWidget.h"
#include "runtime/AgentToolHost.h"
#include <QLabel>
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
    if (m_fileChanges && m_fileChanges->agentId() == id)
      QTimer::singleShot(0, this, &MainWindow::refreshFileChanges);
  });
}
void MainWindow::undoFileChanges(const QString &id, const QStringList &ids) {
  if (!m_fileChanges || m_fileChanges->agentId() != id || ids.isEmpty() ||
      !m_mutationUndoId.isEmpty())
    return;
  auto *session = find(id);
  auto *host = m_toolHosts.value(id);
  auto *engine = m_engines.value(id);
  if (!session || session->archived || !host ||
      (engine && (engine->isBusy() || !engine->queuedMessages().isEmpty())) ||
      gitPathBusy(session->workspace))
    return;
  QString error;
  const auto history = host->mutationHistory(&error);
  QSet<QString> paths;
  ToolExecutor scope;
  scope.setWorkspace(session->workspace);
  for (const auto &value : history) {
    const auto row = value.toObject();
    if (!ids.contains(row["mutationId"].toString()))
      continue;
    for (const auto &entry : row["entries"].toArray()) {
      QString issue;
      const auto path =
          scope.resolvePath(entry.toObject()["path"].toString(), &issue);
      if (issue.isEmpty())
        paths.insert(path.toCaseFolded());
    }
  }
  for (auto *editor : m_tabs->findChildren<EditorWidget *>())
    if (editor->isModified() &&
        paths.contains(editor->filePath().toCaseFolded())) {
      if (m_fileChanges)
        m_fileChanges->setOperation(id, false,
                                    "Save or close your unsaved edits to " +
                                        editor->filePath() +
                                        " before undoing this change.");
      return;
    }
  m_mutationUndoAgent = id;
  m_mutationUndoId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (m_fileChanges)
    m_fileChanges->setOperation(id, true, "Restoring selected changes…");
  updateComposerState();
  host->undoMutations(m_mutationUndoId, ids);
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
