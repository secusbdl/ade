#pragma once
#include "WorkspaceChangesController.h"
#include <QAbstractListModel>
#include <QWidget>
class QLabel;
class QListView;
namespace ade {
class WorkspaceChangesModel final : public QAbstractListModel {
  Q_OBJECT
public:
  enum Role {
    PathRole = Qt::UserRole + 1,
    OldPathRole,
    StagedRole,
    StatusRole,
    AddedRole,
    DeletedRole,
    BinaryRole,
    KeyRole
  };
  explicit WorkspaceChangesModel(QObject *parent = nullptr)
      : QAbstractListModel(parent) {}
  int rowCount(const QModelIndex &parent = {}) const override;
  QVariant data(const QModelIndex &index,
                int role = Qt::DisplayRole) const override;
  void setEntries(const QList<GitDiffEntry> &entries);
  GitDiffEntry entry(int row) const;
  int indexForKey(const QString &key) const;
  static QString key(const GitDiffEntry &entry);

private:
  QList<GitDiffEntry> m_entries;
  bool m_updating = false, m_hasDeferred = false;
  quint64 m_revision = 0;
  QList<GitDiffEntry> m_deferred;
};
class WorkspaceChangesView final : public QWidget {
  Q_OBJECT
public:
  explicit WorkspaceChangesView(QWidget *parent = nullptr);
  void setSnapshot(const QString &workspace,
                   WorkspaceChangesController::Phase phase,
                   const GitDiffSummary &summary, bool hasSnapshot,
                   const QString &error, bool busy);
  WorkspaceChangesModel *model() const { return m_model; }
  QListView *list() const { return m_list; }
signals:
  void openRequested(const QString &workspace, const QString &path, bool staged,
                     const QString &oldPath);

private:
  QString m_workspace;
  WorkspaceChangesModel *m_model;
  QListView *m_list;
  QLabel *m_status;
  quint64 m_epoch = 0;
};
} // namespace ade
