#include "WorkspaceChangesController.h"
#include "git/GitService.h"
#include <QDir>

namespace ade {
WorkspaceChangesController::WorkspaceChangesController(GitService *service,
                                                       QObject *parent)
    : QObject(parent), m_service(service) {
  m_coalesce.setSingleShot(true);
  m_coalesce.setInterval(80);
  m_poll.setInterval(5000);
  connect(&m_coalesce, &QTimer::timeout, this,
          &WorkspaceChangesController::start);
  connect(&m_poll, &QTimer::timeout, this,
          &WorkspaceChangesController::requestRefresh);
  if (service) {
    connect(service, &GitService::completed, this,
            &WorkspaceChangesController::finish);
    connect(service, &GitService::repositoryTopologyChanged, this,
            [this] { requestRefresh(); });
    connect(service, &QObject::destroyed, this, [this] {
      m_service = nullptr;
      m_request = 0;
      m_coalesce.stop();
      m_poll.stop();
      m_error = "Git service is unavailable.";
      m_phase = Phase::Error;
      emit changed();
    });
  }
}
WorkspaceChangesController::~WorkspaceChangesController() { cancelRequest(); }
void WorkspaceChangesController::cancelRequest() {
  const auto request = m_request;
  m_request = 0;
  if (request && m_service)
    m_service->cancel(request);
}
void WorkspaceChangesController::setWorkspace(const QString &workspace) {
  // Freeze the currently authorized physical root; a later alias retarget is
  // not a request to inspect a different repository.
  const auto canonical =
      workspace.isEmpty() ? QString() : GitService::canonicalPath(workspace);
  const auto requested =
      workspace.isEmpty()
          ? QString()
          : QDir::cleanPath(QDir(workspace).absolutePath()).toCaseFolded();
  if (requested == m_requestedPath && !m_workspace.isEmpty() &&
      canonical.compare(m_workspace, Qt::CaseInsensitive) != 0) {
    if (m_invalidAuthority)
      return;
    cancelRequest();
    m_coalesce.stop();
    m_invalidAuthority = true;
    m_dirty = false;
    m_summary = {};
    m_hasSnapshot = false;
    m_phase = Phase::Error;
    m_error = "The workspace path changed its physical destination. Reconnect "
              "the intended workspace.";
    emit changed();
    return;
  }
  if (!m_invalidAuthority && requested == m_requestedPath &&
      canonical.compare(m_workspace, Qt::CaseInsensitive) == 0)
    return;
  cancelRequest();
  m_coalesce.stop();
  m_workspace = canonical;
  m_requestedPath = requested;
  m_invalidAuthority = false;
  m_summary = {};
  m_hasSnapshot = false;
  m_error.clear();
  m_dirty = !canonical.isEmpty();
  m_phase = canonical.isEmpty() ? Phase::NoWorkspace : Phase::Loading;
  if (!workspace.isEmpty() && canonical.isEmpty()) {
    m_phase = Phase::Error;
    m_error = "The workspace is unavailable.";
  }
  if (m_active && m_dirty)
    m_coalesce.start();
  emit changed();
}
void WorkspaceChangesController::setActive(bool active) {
  if (m_active == active)
    return;
  m_active = active;
  if (active) {
    m_poll.start();
    requestRefresh();
  } else {
    m_poll.stop();
    m_coalesce.stop();
    cancelRequest();
    m_dirty = !m_workspace.isEmpty();
  }
}
void WorkspaceChangesController::setPollInterval(int milliseconds) {
  m_poll.setInterval(qMax(20, milliseconds));
}
void WorkspaceChangesController::requestRefresh() {
  if (m_workspace.isEmpty() || m_invalidAuthority)
    return;
  m_dirty = true;
  if (m_active && !m_request && !m_coalesce.isActive())
    m_coalesce.start();
}
void WorkspaceChangesController::start() {
  if (!m_active || m_request || !m_dirty || m_workspace.isEmpty() ||
      m_invalidAuthority)
    return;
  m_dirty = false;
  if (GitService::canonicalPath(m_workspace)
          .compare(m_workspace, Qt::CaseInsensitive) != 0) {
    m_invalidAuthority = true;
    m_summary = {};
    m_hasSnapshot = false;
    m_phase = Phase::Error;
    m_error =
        "The workspace destination changed. Reconnect the intended workspace.";
    emit changed();
    return;
  }
  if (!m_service) {
    m_phase = Phase::Error;
    m_error = "Git service is unavailable.";
    emit changed();
    return;
  }
  m_operation = GitOperation::Inspect;
  m_request = m_service->inspect(m_workspace);
  if (!m_hasSnapshot)
    m_phase = Phase::Loading;
  emit changed();
}
void WorkspaceChangesController::finish(quint64 id, const GitResult &result) {
  if (id != m_request || !id || result.operation != m_operation)
    return;
  m_request = 0;
  if (!result.ok) {
    m_error = result.error.message;
    m_phase = result.error.code == GitErrorCode::NotRepository
                  ? Phase::NotRepository
                  : Phase::Error;
    if (m_phase == Phase::NotRepository) {
      m_summary = {};
      m_hasSnapshot = false;
    }
  } else if (result.operation == GitOperation::Inspect) {
    if (!m_service)
      return;
    m_operation = GitOperation::Diff;
    m_request = m_service->diff(m_workspace);
    return;
  } else {
    m_summary = result.diff;
    m_hasSnapshot = true;
    m_error.clear();
    m_phase = Phase::Ready;
  }
  if (m_dirty && m_active)
    m_coalesce.start();
  emit changed();
}
} // namespace ade
