#include "FileChangesPanel.h"
#include <QDateTime>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QRegularExpression>
#include <QPointer>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>
namespace ade {
FileChangesPanel::FileChangesPanel(QWidget *parent) : QWidget(parent) {
  setObjectName("fileChangesPanel");
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(12, 12, 12, 12);
  auto *title = new QLabel("File changes");
  m_title = title;
  title->setTextFormat(Qt::PlainText);
  title->setWordWrap(true);
  auto font = title->font();
  font.setBold(true);
  font.setPointSize(font.pointSize() + 2);
  title->setFont(font);
  layout->addWidget(title);
  auto *help = new QLabel(
      "Review agent changes and restore selected changes to their earlier "
      "state. Files edited since then are kept and reported as conflicts.");
  help->setWordWrap(true);
  help->setProperty("muted", true);
  layout->addWidget(help);
  m_tree = new QTreeWidget;
  m_tree->setObjectName("mutationHistory");
  m_tree->setColumnCount(3);
  m_tree->setHeaderLabels({"Change", "State", "When"});
  m_tree->setRootIsDecorated(true);
  m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  m_tree->setMinimumSize(0, 0);
  layout->addWidget(m_tree, 1);
  m_status = new QLabel;
  m_status->setTextFormat(Qt::PlainText);
  m_status->setObjectName("mutationStatus");
  m_status->setWordWrap(true);
  layout->addWidget(m_status);
  auto *actions = new QHBoxLayout;
  auto *refresh = new QPushButton("Refresh");
  refresh->setObjectName("refreshMutationHistory");
  m_undo = new QPushButton("Undo selected");
  m_undo->setObjectName("undoSelectedMutations");
  m_cancel = new QPushButton("Cancel undo");
  m_cancel->setObjectName("cancelMutationUndo");
  actions->addWidget(refresh);
  actions->addStretch();
  actions->addWidget(m_cancel);
  actions->addWidget(m_undo);
  layout->addLayout(actions);
  connect(refresh, &QPushButton::clicked, this,
          &FileChangesPanel::refreshRequested);
  m_recover = new QPushButton("Recover interrupted changes");
  m_recover->setObjectName("recoverInterruptedMutations");
  layout->insertWidget(2, m_recover, 0, Qt::AlignLeft);
  connect(m_recover, &QPushButton::clicked, this, [this] {
    if (!m_busy && !m_pending && m_hasIncomplete)
      emit recoveryRequested(m_agentId);
  });
  connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
          &FileChangesPanel::refreshActions);
  connect(m_undo, &QPushButton::clicked, this, [this] {
    const auto ids = selectedMutations();
    if (!m_busy && !m_pending && !ids.isEmpty())
      emit undoRequested(m_agentId, ids);
  });
  connect(m_cancel, &QPushButton::clicked, this, [this] {
    if (m_pending)
      emit cancelRequested(m_agentId);
  });
  refreshActions();
}
void FileChangesPanel::setConversationTitle(const QString &title) {
  m_title->setText(title.isEmpty() ? QString("File changes")
                                   : QString("File changes · ") + title);
}
QStringList FileChangesPanel::selectedMutations() const {
  QStringList result;
  for (auto *item : m_tree->selectedItems()) {
    const auto id = item->data(0, Qt::UserRole).toString();
    if (!id.isEmpty() && item->data(0, Qt::UserRole + 1).toBool() &&
        !result.contains(id))
      result.append(id);
  }
  return result;
}
void FileChangesPanel::refreshActions() {
  const QPointer<FileChangesPanel> self(this);
  const auto count = selectedMutations().size();
  m_undo->setText(count ? QString("Undo selected (%1)").arg(count)
                        : "Undo selected");
  if(!self)return;
  m_undo->setEnabled(!m_busy && !m_pending && count > 0 && count <= 100);
  if(!self)return;
  m_cancel->setVisible(m_pending);
  if(!self)return;
  m_recover->setVisible(m_hasIncomplete);
  if(!self)return;
  m_recover->setEnabled(!m_busy && !m_pending);
}
bool FileChangesPanel::selectMutations(const QStringList& ids) {
  QSet<QString> found;const QSignalBlocker blocker(m_tree);
  for(int i=0;i<m_tree->topLevelItemCount();++i){auto* item=m_tree->topLevelItem(i);const auto id=item->data(0,Qt::UserRole).toString();item->setSelected(ids.contains(id));if(ids.contains(id))found.insert(id);}
  refreshActions();return found.size()==ids.size();
}
bool FileChangesPanel::pathsForMutations(const QStringList& ids,QStringList* paths,QString* error) const {
  if(paths)paths->clear();if(error)error->clear();QSet<QString> found;QStringList result;
  const auto fail=[&]{if(error)*error="Selected saved changes are missing complete original-path information. Refresh the review before undoing.";return false;};
  if(ids.isEmpty()||ids.size()>100||QSet<QString>(ids.cbegin(),ids.cend()).size()!=ids.size())return fail();
  for(int i=0;i<m_tree->topLevelItemCount();++i){const auto* item=m_tree->topLevelItem(i);const auto id=item->data(0,Qt::UserRole).toString();if(!ids.contains(id))continue;if(!item->data(0,Qt::UserRole+1).toBool())return fail();found.insert(id);
    for(int j=0;j<item->childCount();++j){const auto* child=item->child(j);if(!child->data(0,Qt::UserRole+3).toBool())continue;const auto path=child->data(0,Qt::UserRole+2).toString();if(!QRegularExpression("^[A-Za-z]:/").match(path).hasMatch()||path.contains(QRegularExpression("[\\x00-\\x1f]"))||path.split('/').contains(".."))return fail();if(!result.contains(path))result.append(path);}}
  if(found.size()!=ids.size())return fail();if(paths)*paths=result;return true;
}
void FileChangesPanel::setHistory(const QString &agent, const QJsonArray &rows,
                                  bool busy, const QString &message) {
  const auto selected =
      agent == m_agentId ? selectedMutations() : QStringList{};
  if (agent != m_agentId)
    m_pending = false;
  m_agentId = agent;
  m_busy = busy;
  m_hasIncomplete = false;
  const QSignalBlocker blocker(m_tree);
  m_tree->clear();
  for (const auto &value : rows) {
    const auto row = value.toObject();
    const auto id = row["mutationId"].toString();
    if (id.isEmpty())
      continue;
    const auto state = row["state"].toString();
    auto *item = new QTreeWidgetItem(
        m_tree,
        {row["tool"].toString(), state,
         QDateTime::fromMSecsSinceEpoch(qint64(row["createdAt"].toDouble()))
             .toLocalTime()
             .toString("hh:mm")});
    const bool incomplete =
        row["recoveryRequired"].toBool() ||
        (state != "committed" && state != "rolled_back" && state != "failed");
    m_hasIncomplete |= incomplete;
    item->setData(0, Qt::UserRole, id);
    item->setData(0, Qt::UserRole + 1, state == "committed" && !incomplete);
    item->setToolTip(0, row["error"].toString().isEmpty()
                            ? row["output"].toObject()["message"].toString()
                            : row["error"].toString());
    for (const auto &entry : row["entries"].toArray()) {
      const auto file = entry.toObject();
      const auto before = file["beforeExists"].toBool(),
                 after = file["afterExists"].toBool();
      auto *child = new QTreeWidgetItem(item, {file["path"].toString(),
                                               !before  ? "Added"
                                               : !after ? "Deleted"
                                                        : "Modified",
                                               {}});
      child->setFlags(child->flags() & ~Qt::ItemIsSelectable);
      child->setToolTip(0, file["path"].toString());
      child->setData(0,Qt::UserRole+2,file["absolutePath"].toString());
      child->setData(0,Qt::UserRole+3,true);
    }
    if (row["directoryCount"].toInt() > 0) {
      auto *child = new QTreeWidgetItem(
          item,
          {QString("%1 directory changes").arg(row["directoryCount"].toInt()),
           {},
           {}});
      child->setFlags(child->flags() & ~Qt::ItemIsSelectable);
    }
    item->setExpanded(rows.size() < 12);
    item->setSelected(selected.contains(id));
  }
  if (!message.isEmpty())
    m_status->setText(message);
  else
    m_status->setText(rows.isEmpty() ? "No recorded file changes in this chat."
                      : busy
                          ? "Wait for the current task before undoing changes."
                          : QString());
  refreshActions();
}
void FileChangesPanel::setOperation(const QString &agent, bool pending,
                                    const QString &message) {
  if (agent != m_agentId)
    return;
  m_pending = pending;
  m_status->setText(message);
  refreshActions();
}
} // namespace ade
