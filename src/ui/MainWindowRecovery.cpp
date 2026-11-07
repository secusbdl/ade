#include "Composer.h"
#include "MainWindow.h"
#include "SideChatPane.h"
#include "core/AgentEngine.h"
#include "git/WorktreeSettingsController.h"
#include "runtime/AgentToolHost.h"
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTimer>

namespace ade {
void MainWindow::initializeSessionRecovery() {
  m_sessionRecoveryBar = new QStatusBar(this);
  m_sessionRecoveryBar->setObjectName("sessionRecoveryBar");
  m_sessionRecoveryBar->setSizeGripEnabled(false);
  m_sessionRecoveryLabel = new QLabel(m_sessionRecoveryBar);
  m_sessionRecoveryLabel->setObjectName("sessionRecoveryMessage");
  m_sessionRecoveryLabel->setWordWrap(true);
  m_sessionRecoveryLabel->setMinimumWidth(0);
  m_sessionRecoveryLabel->setSizePolicy(QSizePolicy::Ignored,
                                        QSizePolicy::Preferred);
  auto *reload = new QPushButton("Reload conversations", m_sessionRecoveryBar);
  reload->setObjectName("reloadConversationsButton");
  m_sessionRecoveryBar->addWidget(m_sessionRecoveryLabel, 1);
  m_sessionRecoveryBar->addPermanentWidget(reload);
  setStatusBar(m_sessionRecoveryBar);
  connect(reload, &QPushButton::clicked, this,
          &MainWindow::requestSessionReload);
  m_sessionRecoveryBar->hide();
}

void MainWindow::showSessionRecovery(const QString &error) {
  m_sessionRecoveryError = error;
  if (!m_sessionRecoveryBar)
    return;
  m_sessionRecoveryLabel->setText(
      error.isEmpty() ? QString() : "Conversation storage: " + error);
  m_sessionRecoveryBar->setVisible(!error.isEmpty());
}

void MainWindow::showCloseRecovery() {
  if (m_sessionRecoveryDialog)
    return;
  auto *dialog = new QMessageBox(
      QMessageBox::Warning, "Conversations could not be saved",
      "The original file was preserved. Keep working, retry saving, or close "
      "without saving changes from this window.",
      QMessageBox::NoButton, this);
  dialog->setObjectName("sessionCloseRecoveryDialog");
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setInformativeText(m_sessionRecoveryError);
  auto *keep = dialog->addButton("Keep working", QMessageBox::RejectRole);
  keep->setObjectName("recoveryKeepWorking");
  auto *retry = dialog->addButton("Retry saving", QMessageBox::AcceptRole);
  retry->setObjectName("recoveryRetrySave");
  auto *discard =
      dialog->addButton("Close without saving", QMessageBox::DestructiveRole);
  discard->setObjectName("recoveryCloseWithoutSaving");
  dialog->setDefaultButton(keep);
  dialog->setEscapeButton(keep);
  m_sessionRecoveryDialog = dialog;
  const auto serial = ++m_sessionRecoverySerial;
  const QPointer<QMessageBox> box(dialog);
  const QPointer<MainWindow> self(this);
  connect(dialog, &QDialog::finished, this,
          [this, self, box, retry, discard, serial] {
            if (!self || !box || m_sessionRecoveryDialog != box)
              return;
            auto *choice = box->clickedButton();
            m_sessionRecoveryDialog.clear();
            if (choice != discard && choice != retry)
              return;
            const bool discardChanges = choice == discard;
            QTimer::singleShot(0, this, [this, self, serial, discardChanges] {
              if (!self || m_sessionTransition || m_sessionRecoveryDialog ||
                  m_sessionRecoverySerial != serial)
                return;
              m_discardSessionChangesOnClose = discardChanges;
              close();
            });
          });
  dialog->open();
}

bool MainWindow::sessionReloadIsSafe() const {
  for (auto *engine : m_engines)
    if (engine && engine->isBusy())
      return false;
  for (auto *host : m_toolHosts)
    if (host && host->mutationBusy())
      return false;
  if (m_worktreeSettings && m_worktreeSettings->isMutating())
    return false;
  for (const auto &session : m_sessions)
    if (gitPathBusy(session.workspace))
      return false;
  return true;
}

void MainWindow::requestSessionReload() {
  if (m_sessionTransition || m_sessionRecoveryDialog)
    return;
  if (!sessionReloadIsSafe()) {
    showSessionRecovery(
        "Stop running agents and wait for workspace operations before "
        "reloading. Current conversations and drafts are retained.");
    return;
  }
  if (m_sessions.isEmpty() && m_composer->toPlainText().isEmpty()) {
    reloadSessionsConfirmed();
    return;
  }
  auto *dialog = new QMessageBox(
      QMessageBox::Warning, "Reload conversations?",
      "Reload the conversations currently saved on disk? Unsaved conversation "
      "changes and drafts in this window will be discarded. The file on disk "
      "will not be overwritten.",
      QMessageBox::NoButton, this);
  dialog->setObjectName("sessionReloadConfirmation");
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  auto *keep = dialog->addButton("Keep working", QMessageBox::RejectRole);
  keep->setObjectName("recoveryKeepWorking");
  auto *reload = dialog->addButton("Discard changes and reload",
                                   QMessageBox::DestructiveRole);
  reload->setObjectName("recoveryConfirmReload");
  dialog->setDefaultButton(keep);
  dialog->setEscapeButton(keep);
  m_sessionRecoveryDialog = dialog;
  const auto serial = ++m_sessionRecoverySerial;
  const QPointer<QMessageBox> box(dialog);
  const QPointer<MainWindow> self(this);
  connect(dialog, &QDialog::finished, this, [this, self, box, reload, serial] {
    if (!self || !box || m_sessionRecoveryDialog != box)
      return;
    const bool accepted = box->clickedButton() == reload;
    m_sessionRecoveryDialog.clear();
    if (accepted)
      QTimer::singleShot(0, this, [this, self, serial] {
        if (self && !m_sessionTransition && !m_sessionRecoveryDialog &&
            m_sessionRecoverySerial == serial)
          reloadSessionsConfirmed();
      });
  });
  dialog->open();
}

void MainWindow::reloadSessionsConfirmed() {
  if (m_sessionTransition || !sessionReloadIsSafe()) {
    showSessionRecovery("A running operation prevents reload. Current "
                        "conversations and drafts are retained.");
    return;
  }
  // Candidate loading never changes the active store's revision or authority.
  SessionStore candidate(m_store.directory());
  QString error;
  const auto sessions = candidate.load(&error);
  if (!error.isEmpty() || !m_store.canAdoptReload(candidate, &error)) {
    showSessionRecovery(error);
    return;
  }
  const QString previous = m_currentId;
  const QPointer<MainWindow> self(this);
  m_sessionTransition = true;
  const auto sideIds = m_sideChats.keys();
  for (const auto &id : sideIds) {
    if (const auto pane = m_sideChats.value(id)) {
      pane->disconnect(this);
      for (auto *child : pane->findChildren<QObject *>())
        disconnect(child, nullptr, this, nullptr);
    }
    closeSideChatPane(id);
    if (!self)
      return;
  }
  auto retiredIds = m_engines.keys();
  for (const auto &session : m_sessions)
    if (!retiredIds.contains(session.id))
      retiredIds.append(session.id);
  for (const auto &id : retiredIds) {
    disposeConversation(id);
    if (!self)
      return;
  }
  m_currentId.clear();
  m_sessions = sessions;
  m_store = std::move(candidate);
  {
    const QSignalBlocker blocker(m_composer);
    m_composer->clear();
  }
  m_sessionTransition = false;
  showSessionRecovery({});
  refreshAgents();
  if (!self)
    return;
  if (const auto *old = find(previous);
      old && !old->archived && old->sideChatParentId.isEmpty())
    selectConversation(previous);
  else
    for (const auto &session : m_sessions)
      if (!session.archived && session.sideChatParentId.isEmpty()) {
        const auto id = session.id;
        selectConversation(id);
        break;
      }
  if (!self)
    return;
  // An empty valid store remains empty until the user's explicit New Agent
  // action. Reload itself never overwrites the selected disk snapshot.
  if (m_currentId.isEmpty()) {
    refreshAttachments();
    renderConversation();
    updateComposerState();
  }
  if (!self)
    return;
  m_status->setText("Conversations reloaded from disk.");
  updateGitUsageTracking();
}
} // namespace ade
