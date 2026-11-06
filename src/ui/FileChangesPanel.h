#pragma once
#include <QJsonArray>
#include <QWidget>
class QLabel;
class QPushButton;
class QTreeWidget;
namespace ade {
class FileChangesPanel final : public QWidget {
  Q_OBJECT
public:
  explicit FileChangesPanel(QWidget *parent = nullptr);
  void setHistory(const QString &agentId, const QJsonArray &rows, bool busy,
                  const QString &message = {});
  void setOperation(const QString &agentId, bool pending,
                    const QString &message);
  QString agentId() const { return m_agentId; }
  void setConversationTitle(const QString &title);
  QStringList selectedMutations() const;
  bool selectMutations(const QStringList& ids);
  bool pathsForMutations(const QStringList& ids,QStringList* paths,QString* error) const;
  void setBusy(bool busy) {
    m_busy = busy;
    refreshActions();
  }
signals:
  void undoRequested(QString agentId, QStringList mutationIds);
  void refreshRequested();
  void cancelRequested(QString agentId);
  void recoveryRequested(QString agentId);

private:
  void refreshActions();
  QString m_agentId;
  bool m_busy = false, m_pending = false;
  QTreeWidget *m_tree;
  QLabel *m_status, *m_title;
  QPushButton *m_undo, *m_cancel, *m_recover;
  bool m_hasIncomplete = false;
};
} // namespace ade
