#pragma once
#include "git/GitTypes.h"
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace ade {
class GitService;
// Read-only, workspace-scoped requests on the host's existing Git service.
class WorkspaceChangesController final : public QObject {
  Q_OBJECT
public:
  enum class Phase { NoWorkspace, Loading, Ready, NotRepository, Error };
  explicit WorkspaceChangesController(GitService *service,
                                      QObject *parent = nullptr);
  ~WorkspaceChangesController() override;
  void setWorkspace(const QString &workspace);
  void setActive(bool active);
  void requestRefresh();
  void setPollInterval(int milliseconds);
  QString workspace() const { return m_workspace; }
  Phase phase() const { return m_phase; }
  bool busy() const { return m_request != 0; }
  bool hasSnapshot() const { return m_hasSnapshot; }
  const GitDiffSummary &summary() const { return m_summary; }
  QString error() const { return m_error; }
signals:
  void changed();

private:
  void start();
  void finish(quint64 request, const GitResult &result);
  void cancelRequest();
  QPointer<GitService> m_service;
  QTimer m_coalesce, m_poll;
  QString m_workspace, m_error, m_requestedPath;
  quint64 m_request = 0;
  GitOperation m_operation = GitOperation::Inspect;
  Phase m_phase = Phase::NoWorkspace;
  GitDiffSummary m_summary;
  bool m_active = false, m_dirty = false, m_hasSnapshot = false,
       m_invalidAuthority = false;
};
} // namespace ade
