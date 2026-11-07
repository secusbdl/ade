#pragma once
#include "ComposerWorkspaceSearch.h"
#include <QAbstractItemModel>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QHash>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
namespace ade {
// A Files-view model. Authority is the captured physical workspace root, never
// a QFileSystemModel path cache. Links are leaves; safe in-root file links open
// through a short-lived validated handle loan.
class WorkspaceFileTreeModel final : public QAbstractItemModel {
  Q_OBJECT
public:
  enum Role { PathRole = Qt::UserRole + 1, LinkRole, ErrorRole, LimitedRole };
  explicit WorkspaceFileTreeModel(QObject *parent = nullptr);
  ~WorkspaceFileTreeModel() override;
  // Explicit user reconnection may accept a new physical root; refresh never
  // does.
  QModelIndex setRootPath(const QString &path, bool reconnect = false);
  QString rootPath() const { return m_requestedRoot; }
  QString physicalRoot() const { return m_root; }
  quint64 generation() const { return m_generation; }
  bool watchCoverageComplete() const { return m_watchCoverageComplete; }
  QModelIndex index(const QString &absolutePath, int column = 0);
  QModelIndex index(int row, int column,
                    const QModelIndex &parent = {}) const override;
  QModelIndex parent(const QModelIndex &) const override;
  int rowCount(const QModelIndex &parent = {}) const override;
  int columnCount(const QModelIndex & = {}) const override { return 1; }
  QVariant data(const QModelIndex &, int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex &) const override;
  bool hasChildren(const QModelIndex &parent = {}) const override;
  bool canFetchMore(const QModelIndex &parent) const override;
  void fetchMore(const QModelIndex &parent) override;
  QString filePath(const QModelIndex &) const;
  QString fileName(const QModelIndex &) const;
  bool isDir(const QModelIndex &) const;
  ComposerPathPin pinFile(const QModelIndex &, QString *error = nullptr) const;
  void refresh(const QModelIndex &directory = {});
  static constexpr int MaxEntries = 10000, MaxNodes = 20000, MaxWatches = 200;
  static constexpr int PageSize = 100;
signals:
  void directoryLoaded(QString path);
  void directoryError(QString path, QString error);
  void rootIndexChanged(QModelIndex index);
  void watchCoverageChanged(bool complete);

private:
  struct Node;
  struct Listing;
  Node *node(const QModelIndex &) const;
  Node *get(quint64 id) const;
  QModelIndex at(const Node *) const;
  Node *add(Node *parent, const QString &name, bool directory, bool link,
            bool status = false, int position = -1);
  void erase(Node *node);
  void enqueue(Node *node);
  void startNext();
  void publish(quint64 id, const Listing &listing);
  void reconcileWatches();
  void scheduleChanged(const QString &path);
  void resetRoot(const QString &path);
  bool current(quint64 generation) const { return generation == m_generation; }
  QString m_requestedRoot, m_root, m_identity;
  quint64 m_generation = 0, m_nextId = 0, m_rootId = 0;
  QHash<quint64, std::shared_ptr<Node>> m_nodes;
  QHash<QString, quint64> m_paths;
  QQueue<quint64> m_pending;
  QSet<quint64> m_queued;
  QSet<QString> m_dirty;
  QFileSystemWatcher m_watcher;
  QTimer m_debounce, m_probe;
  QFutureWatcher<Listing> *m_worker = nullptr;
  std::shared_ptr<std::atomic_bool> m_cancel;
  bool m_publishing = false, m_watchCoverageComplete = true;
  QString m_deferredRoot;
  bool m_hasDeferredRoot = false, m_deferredReconnect = false;
#ifdef ADE_WORKSPACE_TREE_TESTING
public:
  // Test scheduling barrier; copied value-only callback executes in the worker.
  std::function<void()> beforeScan;
#endif
};
} // namespace ade
