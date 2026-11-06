#include "WorkspaceChangesView.h"
#include "git/GitService.h"
#include <QApplication>
#include <QFileInfo>
#include <QLabel>
#include <QListView>
#include <QPainter>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace ade {
namespace {
QString statusName(char state) {
  switch (state) {
  case 'A':
    return "Added";
  case 'D':
    return "Deleted";
  case 'R':
    return "Renamed";
  case '?':
  case 'U':
    return "Untracked";
  default:
    return "Modified";
  }
}
class ChangeDelegate final : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;
  QSize sizeHint(const QStyleOptionViewItem &option,
                 const QModelIndex &) const override {
    return {120, QFontMetrics(option.font).height() * 2 + 12};
  }
  void paint(QPainter *painter, const QStyleOptionViewItem &original,
             const QModelIndex &index) const override {
    QStyleOptionViewItem option(original);
    initStyleOption(&option, index);
    option.text.clear();
    const auto *widget = option.widget;
    (widget ? widget->style() : QApplication::style())
        ->drawControl(QStyle::CE_ItemViewItem, &option, painter, widget);
    painter->save();
    painter->setClipRect(option.rect);
    const bool selected = option.state & QStyle::State_Selected;
    const auto text = option.palette.color(selected ? QPalette::HighlightedText
                                                    : QPalette::Text);
    const auto path = index.data(WorkspaceChangesModel::PathRole).toString();
    const auto old = index.data(WorkspaceChangesModel::OldPathRole).toString();
    const QString badge =
        index.data(WorkspaceChangesModel::StatusRole).toString() +
        (index.data(WorkspaceChangesModel::StagedRole).toBool() ? " S" : "");
    const auto counts =
        index.data(WorkspaceChangesModel::BinaryRole).toBool()
            ? QString("Binary")
            : QString("+%1 −%2")
                  .arg(
                      index.data(WorkspaceChangesModel::AddedRole).toLongLong())
                  .arg(index.data(WorkspaceChangesModel::DeletedRole)
                           .toLongLong());
    const QFontMetrics metrics(option.font);
    const auto row = option.rect.adjusted(7, 4, -7, -4);
    painter->setFont(option.font);
    painter->setPen(text);
    const bool dark = option.palette.color(QPalette::Base).lightness() < 128;
    const QColor added = selected ? text : QColor(dark ? "#83d7a1" : "#257a43");
    const QColor removed =
        selected ? text : QColor(dark ? "#ff9292" : "#b83030");
    const int badgeWidth = metrics.horizontalAdvance(badge) + 12,
              countsWidth = metrics.horizontalAdvance(counts) + 8;
    const auto state = index.data(WorkspaceChangesModel::StatusRole).toString();
    painter->setPen(state == "U" || state == "A" ? added
                    : state == "D"               ? removed
                                                 : text);
    painter->drawText(row, Qt::AlignRight | Qt::AlignTop, badge);
    painter->setPen(text);
    auto filename = QFileInfo(path).fileName();
    if (!old.isEmpty() && old != path)
      filename = QFileInfo(old).fileName() + " → " + filename;
    painter->drawText(row, Qt::AlignLeft | Qt::AlignTop,
                      metrics.elidedText(filename, Qt::ElideMiddle,
                                         qMax(0, row.width() - badgeWidth)));
    auto muted = text;
    muted.setAlpha(selected ? 255 : 165);
    painter->setPen(muted);
    auto bottom = row;
    bottom.setTop(row.bottom() - metrics.height() + 1);
    const auto slash = path.lastIndexOf('/');
    const auto directory = slash < 0 ? QString(".") : path.left(slash);
    painter->drawText(bottom, Qt::AlignLeft | Qt::AlignVCenter,
                      metrics.elidedText(directory, Qt::ElideMiddle,
                                         qMax(0, row.width() - countsWidth)));
    if (index.data(WorkspaceChangesModel::BinaryRole).toBool())
      painter->drawText(bottom, Qt::AlignRight | Qt::AlignVCenter, counts);
    else {
      const auto plus = QString("+%1").arg(
          index.data(WorkspaceChangesModel::AddedRole).toLongLong());
      const auto minus = QString("−%1").arg(
          index.data(WorkspaceChangesModel::DeletedRole).toLongLong());
      auto positive = bottom;
      positive.setRight(bottom.right() - metrics.horizontalAdvance(minus) -
                        metrics.horizontalAdvance(' '));
      painter->setPen(added);
      painter->drawText(positive, Qt::AlignRight | Qt::AlignVCenter, plus);
      painter->setPen(removed);
      painter->drawText(bottom, Qt::AlignRight | Qt::AlignVCenter, minus);
    }
    painter->restore();
  }
};
} // namespace
int WorkspaceChangesModel::rowCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : m_entries.size();
}
QString WorkspaceChangesModel::key(const GitDiffEntry &entry) {
  return (entry.staged ? QStringLiteral("s:") : QStringLiteral("w:")) +
         entry.path;
}
QVariant WorkspaceChangesModel::data(const QModelIndex &index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size())
    return {};
  const auto &e = m_entries[index.row()];
  switch (role) {
  case Qt::DisplayRole:
  case Qt::AccessibleTextRole:
    return QString("%1, %2%3, +%4 −%5")
        .arg(e.path, statusName(e.status), e.staged ? ", staged" : "")
        .arg(e.added)
        .arg(e.deleted);
  case Qt::ToolTipRole:
    return QString("<qt>%1<br>%2</qt>")
        .arg((e.oldPath.isEmpty() ? e.path : e.oldPath + " → " + e.path)
                 .toHtmlEscaped(),
             statusName(e.status) +
                 (e.staged ? " · Staged" : " · Working tree"));
  case PathRole:
    return e.path;
  case OldPathRole:
    return e.oldPath;
  case StagedRole:
    return e.staged;
  case StatusRole:
    return QString(QChar(e.status == '?' ? 'U' : e.status));
  case AddedRole:
    return e.added;
  case DeletedRole:
    return e.deleted;
  case BinaryRole:
    return e.binary;
  case KeyRole:
    return key(e);
  default:
    return {};
  }
}
void WorkspaceChangesModel::setEntries(const QList<GitDiffEntry> &entries) {
  ++m_revision;
  if (m_updating) {
    m_deferred = entries;
    m_hasDeferred = true;
    return;
  }
  m_updating = true;
  const QPointer<WorkspaceChangesModel> self(this);
  auto next = entries;
  std::stable_sort(next.begin(), next.end(), [](const auto &a, const auto &b) {
    return a.path == b.path ? a.staged > b.staged : a.path < b.path;
  });
  bool same = next.size() == m_entries.size();
  for (int i = 0; same && i < next.size(); ++i)
    same = key(next[i]) == key(m_entries[i]);
  if (!same) {
    beginResetModel();
    if (!self)
      return;
    m_entries = std::move(next);
    endResetModel();
    if (!self)
      return;
  } else
    for (int i = 0; i < next.size(); ++i) {
      const auto &a = m_entries[i];
      const auto &b = next[i];
      if (a.oldPath == b.oldPath && a.status == b.status &&
          a.added == b.added && a.deleted == b.deleted && a.binary == b.binary)
        continue;
      m_entries[i] = b;
      emit dataChanged(index(i), index(i));
      if (!self)
        return;
    }
  m_updating = false;
  if (m_hasDeferred) {
    const auto pending = std::move(m_deferred);
    const auto revision = m_revision;
    m_hasDeferred = false;
    QTimer::singleShot(0, this, [this, pending, revision] {
      if (m_revision == revision)
        setEntries(pending);
    });
  }
}
GitDiffEntry WorkspaceChangesModel::entry(int row) const {
  return row >= 0 && row < m_entries.size() ? m_entries[row] : GitDiffEntry{};
}
int WorkspaceChangesModel::indexForKey(const QString &wanted) const {
  for (int i = 0; i < m_entries.size(); ++i)
    if (key(m_entries[i]) == wanted)
      return i;
  return -1;
}
WorkspaceChangesView::WorkspaceChangesView(QWidget *parent) : QWidget(parent) {
  setObjectName("workspaceChangesView");
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(4);
  m_status = new QLabel;
  m_status->setObjectName("workspaceChangesStatus");
  m_status->setWordWrap(true);
  m_status->setTextFormat(Qt::PlainText);
  layout->addWidget(m_status);
  m_list = new QListView;
  m_list->setObjectName("workspaceChangesList");
  m_list->setUniformItemSizes(true);
  m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  m_model = new WorkspaceChangesModel(this);
  m_list->setModel(m_model);
  m_list->setItemDelegate(new ChangeDelegate(m_list));
  layout->addWidget(m_list, 1);
  const auto open = [this](const QModelIndex &index) {
    const auto entry = m_model->entry(index.row());
    if (entry.path.isEmpty() || entry.status == 'D')
      return;
    emit openRequested(m_workspace, entry.path, entry.staged, entry.oldPath);
  };
  connect(m_list, &QListView::clicked, this, open);
  connect(m_list, &QListView::activated, this, open);
}
void WorkspaceChangesView::setSnapshot(const QString &workspace,
                                       WorkspaceChangesController::Phase phase,
                                       const GitDiffSummary &summary,
                                       bool hasSnapshot, const QString &error,
                                       bool busy) {
  const auto epoch = ++m_epoch;
  const bool same =
      GitService::pathKey(m_workspace) == GitService::pathKey(workspace);
  const auto selected = same ? m_list->currentIndex()
                                   .data(WorkspaceChangesModel::KeyRole)
                                   .toString()
                             : QString();
  const int scroll = same ? m_list->verticalScrollBar()->value() : 0;
  m_workspace = workspace;
  const QPointer<WorkspaceChangesView> self(this);
  m_model->setEntries(summary.entries);
  if (!self || m_epoch != epoch)
    return;
  const int row = m_model->indexForKey(selected);
  if (row >= 0)
    m_list->setCurrentIndex(m_model->index(row));
  else
    m_list->setCurrentIndex({});
  if (!self || m_epoch != epoch)
    return;
  m_list->verticalScrollBar()->setValue(scroll);
  if (!self || m_epoch != epoch)
    return;
  QString status;
  if (phase == WorkspaceChangesController::Phase::NoWorkspace)
    status = "Connect a workspace to see changed files.";
  else if (phase == WorkspaceChangesController::Phase::NotRepository)
    status = "This workspace is not a Git repository.";
  else if (phase == WorkspaceChangesController::Phase::Error)
    status =
        (hasSnapshot ? "Refresh failed; showing the previous result.\n" : "") +
        error;
  else if (!hasSnapshot)
    status = "Loading changed files…";
  else if (summary.entries.isEmpty())
    status = "Working tree clean";
  // A same-workspace background refresh keeps the viewport geometry stable.
  Q_UNUSED(busy)
  m_status->setText(status);
  m_status->setVisible(!status.isEmpty());
  m_list->setVisible(hasSnapshot && !summary.entries.isEmpty());
}
} // namespace ade
