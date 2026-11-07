#include "WorkspaceFileTreeModel.h"
#include "core/ScopedDirectory.h"
#include "harness/HarnessWorkspace.h"
#include <QApplication>
#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QPalette>
#include <QPointer>
#include <QStyle>
#include <QtConcurrent>
#include <algorithm>
#include <vector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace ade {
namespace {
QString key(const QString &path) {
#ifdef Q_OS_WIN
  return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
#else
  return QDir::cleanPath(path);
#endif
}
bool contains(const QString &root, const QString &path) {
  const auto r = key(root), p = key(path);
  return p == r || p.startsWith(r.endsWith('/') ? r : r + '/');
}
QString relative(const QString &root, const QString &path) {
  return QDir(root).relativeFilePath(path);
}
struct FilePins {
  std::shared_ptr<void> root, file, target;
};
ComposerPathPin openFilePin(const QString &root, const QString &identity,
                            const QString &path, QString *error) {
  if (error)
    error->clear();
  const auto fail = [&](const QString &message) {
    if (error)
      *error = message;
    return ComposerPathPin{};
  };
  if (root.isEmpty() || !contains(root, path))
    return fail("The file is outside the selected workspace.");
  auto rootScope = HarnessWorkspace::resolve(root);
  if (rootScope.identity != identity || key(rootScope.path) != key(root))
    return fail("The selected workspace was replaced or is unavailable.");
  const auto rel = relative(root, path);
  if (rel == "." || rel.split('/').contains(".git"))
    return fail("This entry is not an openable workspace file.");
  const auto parentRelative = relative(root, QFileInfo(path).absolutePath());
  auto parent = ComposerWorkspaceSearch::pin(
      root, parentRelative == "." ? QString() : parentRelative, {}, error);
  if (!parent)
    return {};
#ifdef Q_OS_WIN
  const auto native = QDir::toNativeSeparators(path).toStdWString();
  // Follow only the final leaf. Intermediate directories are pinned and links
  // rejected above; validate the final physical file before returning its path.
  HANDLE handle =
      CreateFileW(native.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                  FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return fail("The selected file is unavailable.");
  auto file = std::shared_ptr<void>(
      handle, [](void *h) { CloseHandle(static_cast<HANDLE>(h)); });
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(handle, &info) ||
      (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    return fail(
        "Directory links are shown as leaves and cannot be opened as files.");
  const DWORD needed = GetFinalPathNameByHandleW(
      handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (!needed || needed > 32768)
    return fail("Cannot validate the selected file.");
  std::vector<wchar_t> buffer(needed + 1);
  const DWORD n =
      GetFinalPathNameByHandleW(handle, buffer.data(), DWORD(buffer.size()),
                                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (!n || n >= buffer.size())
    return fail("Cannot validate the selected file.");
  auto physical = QString::fromWCharArray(buffer.data(), int(n));
  if (physical.startsWith("\\\\?\\UNC\\"))
    physical = "\\\\" + physical.mid(8);
  else if (physical.startsWith("\\\\?\\"))
    physical.remove(0, 4);
  physical = QDir::cleanPath(QDir::fromNativeSeparators(physical));
  if (!contains(root, physical))
    return fail("The file link resolves outside the selected workspace.");
  auto target =
      ComposerWorkspaceSearch::pin(root, relative(root, physical), {}, error);
  if (!target || target.directory)
    return {};
  auto held = std::make_shared<FilePins>();
  held->root = rootScope.pin;
  held->file = std::move(file);
  held->target = target.handles;
  return {target.absolutePath, identity, false, held};
#else
  Q_UNUSED(parent);
  return fail("Scoped Files-view opening currently requires Windows handle "
              "validation.");
#endif
}
} // namespace
struct WorkspaceFileTreeModel::Node {
  quint64 id = 0, parent = 0;
  QString name, path, error;
  bool directory = false, link = false, status = false, loaded = false,
       loading = false, limited = false;
  QList<quint64> children;
  int visible = 0;
};
struct WorkspaceFileTreeModel::Listing {
  ScopedDirectorySnapshot snapshot;
  QString error;
  bool limited = false, cancelled = false;
};
WorkspaceFileTreeModel::WorkspaceFileTreeModel(QObject *parent)
    : QAbstractItemModel(parent), m_watcher(this) {
  m_debounce.setSingleShot(true);
  m_debounce.setInterval(100);
  m_probe.setInterval(5000);
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this,
          &WorkspaceFileTreeModel::scheduleChanged);
  connect(&m_debounce, &QTimer::timeout, this, [this] {
    const auto dirty = m_dirty;
    m_dirty.clear();
    for (const auto &path : dirty) {
      auto *n = get(m_paths.value(key(path)));
      if (n)
        enqueue(n);
    }
    startNext();
  });
  connect(&m_probe, &QTimer::timeout, this, [this] {
    if (m_root.isEmpty())
      return;
    const auto scope = HarnessWorkspace::resolve(m_root);
    if (scope.identity != m_identity || key(scope.path) != key(m_root))
      refresh();
    else {
      const bool before = m_watchCoverageComplete;
      reconcileWatches();
      if (before != m_watchCoverageComplete)
        emit watchCoverageChanged(m_watchCoverageComplete);
    }
  });
}
WorkspaceFileTreeModel::~WorkspaceFileTreeModel() {
  if (m_cancel)
    m_cancel->store(true);
  if (m_worker)
    m_worker->disconnect(this);
}
WorkspaceFileTreeModel::Node *WorkspaceFileTreeModel::get(quint64 id) const {
  return m_nodes.value(id).get();
}
WorkspaceFileTreeModel::Node *
WorkspaceFileTreeModel::node(const QModelIndex &index) const {
  return index.isValid() && index.model() == this ? get(index.internalId())
                                                  : nullptr;
}
QModelIndex WorkspaceFileTreeModel::at(const Node *n) const {
  if (!n)
    return {};
  const auto *p = get(n->parent);
  const auto row = p ? p->children.indexOf(n->id) : 0;
  return row >= 0 ? createIndex(row, 0, n->id) : QModelIndex{};
}
QModelIndex WorkspaceFileTreeModel::index(int row, int column,
                                          const QModelIndex &parent) const {
  if (column || row < 0)
    return {};
  if (!parent.isValid())
    return row == 0 ? at(get(m_rootId)) : QModelIndex{};
  const auto *p = node(parent);
  return p && row < p->visible && row < p->children.size()
             ? at(get(p->children[row]))
             : QModelIndex{};
}
QModelIndex WorkspaceFileTreeModel::parent(const QModelIndex &i) const {
  const auto *n = node(i);
  return n ? at(get(n->parent)) : QModelIndex{};
}
int WorkspaceFileTreeModel::rowCount(const QModelIndex &p) const {
  if (!p.isValid())
    return m_rootId ? 1 : 0;
  const auto *n = node(p);
  return n ? n->visible : 0;
}
QVariant WorkspaceFileTreeModel::data(const QModelIndex &i, int role) const {
  const auto *n = node(i);
  if (!n)
    return {};
  if (role == Qt::DisplayRole)
    return n->name;
  if (role == PathRole)
    return n->path;
  if (role == LinkRole)
    return n->link;
  if (role == ErrorRole)
    return n->error;
  if (role == LimitedRole)
    return n->limited;
  if (role == Qt::ToolTipRole)
    return n->status ? n->error
           : n->link ? QString("Link: ") + n->path
                     : n->path;
  if (role == Qt::DecorationRole && !n->status)
    return QApplication::style()->standardIcon(n->directory ? QStyle::SP_DirIcon
                                               : n->link
                                                   ? QStyle::SP_FileLinkIcon
                                                   : QStyle::SP_FileIcon);
  if (role == Qt::ForegroundRole && n->status)
    return QApplication::palette().color(QPalette::Disabled, QPalette::Text);
  return {};
}
Qt::ItemFlags WorkspaceFileTreeModel::flags(const QModelIndex &i) const {
  const auto *n = node(i);
  return n && !n->status ? Qt::ItemIsSelectable | Qt::ItemIsEnabled
                         : Qt::NoItemFlags;
}
bool WorkspaceFileTreeModel::hasChildren(const QModelIndex &p) const {
  if (!p.isValid())
    return bool(m_rootId);
  const auto *n = node(p);
  return n && n->directory && (!n->loaded || n->visible > 0);
}
bool WorkspaceFileTreeModel::canFetchMore(const QModelIndex &p) const {
  const auto *n = node(p);
  return n && n->directory && !n->loading &&
         (!n->loaded || n->visible < n->children.size());
}
void WorkspaceFileTreeModel::fetchMore(const QModelIndex &p) {
  auto *n = node(p);
  if (!n || !n->directory || n->loading || m_publishing)
    return;
  if (!n->loaded) {
    enqueue(n);
    startNext();
    return;
  }
  const auto next = qMin(n->visible + PageSize, int(n->children.size()));
  if (next == n->visible)
    return;
  const QPointer<WorkspaceFileTreeModel> self(this);
  m_publishing = true;
  beginInsertRows(at(n), n->visible, next - 1);
  if (!self)
    return;
  n->visible = next;
  endInsertRows();
  if (!self)
    return;
  m_publishing = false;
  if (m_hasDeferredRoot) {
    const auto deferred = m_deferredRoot;
    const bool reconnect = m_deferredReconnect;
    m_hasDeferredRoot = false;
    setRootPath(deferred, reconnect);
    return;
  }
  startNext();
}
QString WorkspaceFileTreeModel::filePath(const QModelIndex &i) const {
  const auto *n = node(i);
  return n && !n->status ? n->path : QString();
}
QString WorkspaceFileTreeModel::fileName(const QModelIndex &i) const {
  const auto *n = node(i);
  return n ? n->name : QString();
}
bool WorkspaceFileTreeModel::isDir(const QModelIndex &i) const {
  const auto *n = node(i);
  return n && n->directory;
}
ComposerPathPin WorkspaceFileTreeModel::pinFile(const QModelIndex &i,
                                                QString *error) const {
  const auto *n = node(i);
  if (!n || n->status || n->directory) {
    if (error)
      *error = "The Files selection is no longer an openable file.";
    return {};
  }
  return openFilePin(m_root, m_identity, n->path, error);
}
WorkspaceFileTreeModel::Node *
WorkspaceFileTreeModel::add(Node *parent, const QString &name, bool directory,
                            bool link, bool status, int position) {
  auto n = std::make_shared<Node>();
  n->id = ++m_nextId;
  n->parent = parent ? parent->id : 0;
  n->name = name;
  n->directory = directory;
  n->link = link;
  n->status = status;
  n->path = parent ? QDir(parent->path).filePath(name) : m_root;
  auto *raw = n.get();
  m_nodes.insert(n->id, n);
  if (!status)
    m_paths.insert(key(n->path), n->id);
  if (parent) {
    if (position < 0)
      position = parent->children.size();
    parent->children.insert(position, n->id);
  }
  return raw;
}
void WorkspaceFileTreeModel::erase(Node *n) {
  if (!n)
    return;
  for (const auto id : n->children)
    erase(get(id));
  m_queued.remove(n->id);
  if (!n->status)
    m_paths.remove(key(n->path));
  m_nodes.remove(n->id);
}
QModelIndex WorkspaceFileTreeModel::setRootPath(const QString &path,
                                                bool reconnect) {
  if (m_publishing) {
    m_deferredRoot = path;
    m_deferredReconnect = reconnect;
    m_hasDeferredRoot = true;
    return {};
  }
  if (!reconnect && path == m_requestedRoot && m_rootId &&
      !m_identity.isEmpty())
    return at(get(m_rootId));
  const QPointer<WorkspaceFileTreeModel> self(this);
  resetRoot(path);
  return self ? at(get(m_rootId)) : QModelIndex{};
}
void WorkspaceFileTreeModel::resetRoot(const QString &path) {
  ++m_generation;
  if (m_cancel)
    m_cancel->store(true);
  m_pending.clear();
  m_queued.clear();
  m_dirty.clear();
  m_debounce.stop();
  m_probe.stop();
  if (!m_watcher.directories().isEmpty())
    m_watcher.removePaths(m_watcher.directories());
  const auto scope = HarnessWorkspace::resolve(path);
  const bool coverageChanged = !m_watchCoverageComplete;
  m_watchCoverageComplete = true;
  m_requestedRoot = path;
  m_root = scope.path;
  m_identity = scope.identity;
  const QPointer<WorkspaceFileTreeModel> self(this);
  m_publishing = true;
  beginResetModel();
  if (!self)
    return;
  m_nodes.clear();
  m_paths.clear();
  m_rootId = 0;
  if (!path.isEmpty()) {
    if (m_root.isEmpty())
      m_root = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    auto *root = add(nullptr, QFileInfo(m_root).fileName(), true, false);
    m_rootId = root->id;
  }
  endResetModel();
  if (!self)
    return;
  m_publishing = false;
  const auto generation = m_generation;
  emit rootIndexChanged(at(get(m_rootId)));
  if (!self || !current(generation))
    return;
  if (m_hasDeferredRoot) {
    const auto deferred = m_deferredRoot;
    const bool reconnect = m_deferredReconnect;
    m_hasDeferredRoot = false;
    setRootPath(deferred, reconnect);
    return;
  }
  if (coverageChanged) {
    emit watchCoverageChanged(true);
    if (!self || !current(generation))
      return;
  }
  if (m_rootId) {
    m_probe.start();
    refresh();
  }
}
QModelIndex WorkspaceFileTreeModel::index(const QString &raw, int column) {
  if (column || m_root.isEmpty() || !contains(m_root, raw) || m_publishing)
    return {};
  const auto rel = relative(m_root, raw);
  if (rel == ".")
    return at(get(m_rootId));
  if (rel.split('/').contains(".git") || rel.split('/').size() > 128)
    return {};
  const auto scope = HarnessWorkspace::resolve(m_root);
  if (scope.identity != m_identity)
    return {};
  auto loan = ComposerWorkspaceSearch::pin(m_root, rel, {});
  if (!loan)
    return {};
  const QPointer<WorkspaceFileTreeModel> self(this);
  m_publishing = true;
  auto *p = get(m_rootId);
  QString currentPath = m_root;
  const auto parts = rel.split('/');
  for (int i = 0; i < parts.size(); ++i) {
    currentPath = QDir(currentPath).filePath(parts[i]);
    auto *next = get(m_paths.value(key(currentPath)));
    if (!next) {
      if (m_nodes.size() >= MaxNodes) {
        m_publishing = false;
        if (m_hasDeferredRoot) {
          const auto deferred = m_deferredRoot;
          const bool reconnect = m_deferredReconnect;
          m_hasDeferredRoot = false;
          setRootPath(deferred, reconnect);
        }
        return {};
      }
      const int row = p->visible;
      beginInsertRows(at(p), row, row);
      if (!self)
        return {};
      next = add(p, parts[i], i + 1 < parts.size() || loan.directory, false,
                 false, row);
      ++p->visible;
      endInsertRows();
      if (!self)
        return {};
    } else {
      const int row = p->children.indexOf(next->id);
      if (row >= p->visible) {
        beginInsertRows(at(p), p->visible, row);
        if (!self)
          return {};
        p->visible = row + 1;
        endInsertRows();
        if (!self)
          return {};
      }
    }
    p = next;
  }
  m_publishing = false;
  if (m_hasDeferredRoot) {
    const auto deferred = m_deferredRoot;
    const bool reconnect = m_deferredReconnect;
    m_hasDeferredRoot = false;
    setRootPath(deferred, reconnect);
    return {};
  }
  return at(p);
}
void WorkspaceFileTreeModel::refresh(const QModelIndex &directory) {
  auto *n = directory.isValid() ? node(directory) : get(m_rootId);
  if (!n || !n->directory)
    return;
  enqueue(n);
  startNext();
}
void WorkspaceFileTreeModel::enqueue(Node *n) {
  if (!n || !n->directory || m_queued.contains(n->id) ||
      m_pending.size() >= MaxWatches)
    return;
  m_pending.enqueue(n->id);
  m_queued.insert(n->id);
}
void WorkspaceFileTreeModel::startNext() {
  if (m_publishing || m_worker || m_pending.isEmpty())
    return;
  const auto id = m_pending.dequeue();
  m_queued.remove(id);
  auto *n = get(id);
  if (!n) {
    startNext();
    return;
  }
  n->loading = true;
  const auto path = n->path, root = m_root, identity = m_identity;
  const auto generation = m_generation;
  auto cancel = std::make_shared<std::atomic_bool>(false);
  m_cancel = cancel;
  auto *watcher = new QFutureWatcher<Listing>(this);
  m_worker = watcher;
#ifdef ADE_WORKSPACE_TREE_TESTING
  const auto barrier = beforeScan;
#else
  const std::function<void()> barrier;
#endif
  connect(watcher, &QFutureWatcher<Listing>::finished, this,
          [this, watcher, id, generation] {
            auto result = watcher->result();
            if (m_worker != watcher) {
              watcher->deleteLater();
              return;
            }
            m_worker = nullptr;
            watcher->deleteLater();
            const QPointer<WorkspaceFileTreeModel> self(this);
            if (current(generation) && !result.cancelled) {
              const auto identity = HarnessWorkspace::resolve(m_root).identity;
              if (identity != m_identity) {
                result.snapshot = {};
                result.error =
                    "The selected workspace was replaced or is unavailable.";
              }
              publish(id, result);
            }
            if (self)
              startNext();
          });
  watcher->setFuture(QtConcurrent::run([path, root, identity, cancel, barrier] {
    Listing result;
    if (barrier)
      barrier();
    if (cancel->load()) {
      result.cancelled = true;
      return result;
    }
    const auto scope = HarnessWorkspace::resolve(root);
    if (scope.identity != identity || key(scope.path) != key(root)) {
      result.error = "The selected workspace was replaced or is unavailable.";
      return result;
    }
    const auto parents = ComposerWorkspaceSearch::pin(
        root, relative(root, path) == "." ? QString() : relative(root, path),
        {}, &result.error);
    if (!parents)
      return result;
    if (relative(root, path).split('/').size() > 128) {
      result.error = "Directory depth exceeds the native bound of 128.";
      return result;
    }
    if (!readScopedDirectory(path, root, &result.snapshot, &result.error,
                             MaxEntries, cancel.get(), false,
                             &result.limited)) {
      result.cancelled = cancel->load();
      return result;
    }
    auto &entries = result.snapshot.entries;
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [](const auto &e) { return e.name == ".git"; }),
        entries.end());
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(entries.begin(), entries.end(),
              [&](const auto &a, const auto &b) {
                const bool ad = a.isDirectory && !a.isReparsePoint,
                           bd = b.isDirectory && !b.isReparsePoint;
                if (ad != bd)
                  return ad;
                return collator.compare(a.name, b.name) < 0;
              });
    result.snapshot.pin.reset();
    return result;
  }));
}
void WorkspaceFileTreeModel::publish(quint64 id, const Listing &listing) {
  auto *n = get(id);
  if (!n)
    return;
  n->loading = false;
  n->loaded = true;
  n->limited = listing.limited;
  n->error = listing.error;
  const auto generation = m_generation;
  const QPointer<WorkspaceFileTreeModel> self(this);
  const bool oldCoverage = m_watchCoverageComplete;
  m_publishing = true;
  // Retain unchanged nodes and persistent indexes; only real additions/removals
  // change rows. A linked directory is deliberately a leaf, as in the source.
  QSet<QString> wanted;
  for (const auto &entry : listing.snapshot.entries)
    wanted.insert(entry.name);
  for (int row = n->children.size() - 1; row >= 0; --row) {
    auto *child = get(n->children[row]);
    if (!child)
      continue;
    if (!listing.error.isEmpty() || child->status ||
        !wanted.contains(child->name)) {
      const bool shown = row < n->visible;
      if (shown) {
        beginRemoveRows(at(n), row, row);
        if (!self)
          return;
      }
      n->children.removeAt(row);
      if (shown)
        --n->visible;
      erase(child);
      if (shown) {
        endRemoveRows();
        if (!self)
          return;
      }
    }
  }
  int row = 0;
  for (const auto &entry : listing.snapshot.entries) {
    auto *child = get(m_paths.value(key(entry.absolutePath)));
    const bool directory = entry.isDirectory && !entry.isReparsePoint;
    if (child && (child->directory != directory ||
                  child->link != entry.isReparsePoint)) {
      const int old = n->children.indexOf(child->id);
      const bool shown = old < n->visible;
      if (shown) {
        beginRemoveRows(at(n), old, old);
        if (!self)
          return;
      }
      n->children.removeAt(old);
      if (shown)
        --n->visible;
      erase(child);
      if (shown) {
        endRemoveRows();
        if (!self)
          return;
      }
      child = nullptr;
    }
    if (!child) {
      if (m_nodes.size() >= MaxNodes) {
        n->limited = true;
        continue;
      }
      const bool shown = row < n->visible || n->visible < PageSize;
      if (shown) {
        beginInsertRows(at(n), row, row);
        if (!self)
          return;
      }
      child = add(n, entry.name, directory, entry.isReparsePoint, false, row);
      if (shown)
        ++n->visible;
      if (shown) {
        endInsertRows();
        if (!self)
          return;
      }
    } else {
      const int old = n->children.indexOf(child->id);
      if (old != row) {
        const bool visibleMove = old < n->visible && row < n->visible;
        if (visibleMove) {
          beginMoveRows(at(n), old, old, at(n), old < row ? row + 1 : row);
          if (!self)
            return;
        }
        n->children.move(old, row);
        if (visibleMove) {
          endMoveRows();
          if (!self)
            return;
        }
      }
    }
    ++row;
  }
  if (!n->error.isEmpty() || n->limited) {
    const auto message =
        !n->error.isEmpty()
            ? n->error
            : QString("Directory limit reached; showing a bounded listing.");
    const int row = n->visible;
    beginInsertRows(at(n), row, row);
    if (!self)
      return;
    auto *status = add(n, message, false, false, true, row);
    status->error = message;
    status->limited = n->limited;
    ++n->visible;
    endInsertRows();
    if (!self)
      return;
  }
  m_publishing = false;
  reconcileWatches();
  if (m_hasDeferredRoot) {
    const auto deferred = m_deferredRoot;
    const bool reconnect = m_deferredReconnect;
    m_hasDeferredRoot = false;
    setRootPath(deferred, reconnect);
    return;
  }
  const auto path = n->path, error = n->error;
  if (oldCoverage != m_watchCoverageComplete) {
    emit watchCoverageChanged(m_watchCoverageComplete);
    if (!self || !current(generation) || !get(id))
      return;
  }
  if (!error.isEmpty()) {
    emit directoryError(path, error);
    if (!self || !current(generation) || !get(id))
      return;
  }
  emit directoryLoaded(path);
}
void WorkspaceFileTreeModel::scheduleChanged(const QString &path) {
  m_dirty.insert(path);
  if (key(path) == key(QFileInfo(m_root).absolutePath()))
    m_dirty.insert(m_root);
  m_debounce.start();
}
void WorkspaceFileTreeModel::reconcileWatches() {
  QStringList desired;
  if (!m_root.isEmpty() && QFileInfo(m_root).absolutePath() != m_root)
    desired << QFileInfo(m_root).absolutePath();
  if (!m_root.isEmpty() && QFileInfo(m_root).isDir())
    desired << m_root;
  for (auto it = m_nodes.constBegin(); it != m_nodes.constEnd(); ++it) {
    const auto &n = *it.value();
    if (n.directory && n.loaded && n.error.isEmpty() &&
        QFileInfo(n.path).isDir() && !desired.contains(n.path))
      desired << n.path;
  }
  const bool bounded = desired.size() <= MaxWatches;
  // Keep existing useful watches stable. A changing hash iteration order must
  // not churn registrations at the cap, and failed native removals do not grant
  // new slots. Account against QFileSystemWatcher's actual current registry.
  auto existing = m_watcher.directories();
  QStringList remove;
  for (const auto &path : existing)
    if (!desired.contains(path))
      remove << path;
  if (!remove.isEmpty())
    m_watcher.removePaths(remove);
  existing = m_watcher.directories();
  QStringList add;
  const int room = qMax(0, MaxWatches - int(existing.size()));
  for (const auto &path : desired) {
    if (!existing.contains(path) && add.size() < room)
      add << path;
  }
  if (!add.isEmpty())
    m_watcher.addPaths(add);
  const auto registered = m_watcher.directories();
  m_watchCoverageComplete = bounded;
  for (const auto &path : desired)
    if (!registered.contains(path)) {
      m_watchCoverageComplete = false;
      break;
    }
}
} // namespace ade
