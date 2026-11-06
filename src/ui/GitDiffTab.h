#pragma once
#include "git/GitTypes.h"
#include <QPointer>
#include <QWidget>
class QLabel;
class QPushButton;
namespace ade {
class CodeEditor;
class GitService;
class GitDiffTab final : public QWidget {
  Q_OBJECT
public:
  explicit GitDiffTab(GitService *service, QWidget *parent = nullptr);
  ~GitDiffTab() override;
  void load(const QString &workspace, const QString &path, bool staged,
            const QString &oldPath = {});
  QString workspace() const { return m_workspace; }
  QString path() const { return m_path; }
  QString oldPath() const { return m_oldPath; }
  bool staged() const { return m_staged; }
  bool busy() const { return m_request != 0; }
  bool ready() const { return m_ready; }
  QString error() const { return m_error; }
  QString tabKey() const;
  static QString tabKey(const QString &workspace, const QString &path,
                        bool staged, const QString &oldPath);
  void refresh();
signals:
  void stateChanged();

private:
  void finish(quint64 id, const GitResult &result);
  void cancel();
  QPointer<GitService> m_service;
  QString m_workspace, m_path, m_oldPath, m_error;
  quint64 m_request = 0;
  bool m_staged = false, m_ready = false;
  QLabel *m_status, *m_originalTitle, *m_modifiedTitle;
  QPushButton *m_refresh;
  CodeEditor *m_original, *m_modified;
};
} // namespace ade
