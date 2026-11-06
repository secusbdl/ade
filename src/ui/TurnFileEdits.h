#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QSet>
#include <QWidget>
class QLabel;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
namespace ade {
struct TurnFileSelection {QStringList mutationIds;bool incomplete=false;};
// Saved-change presentation only. Journal ownership and snapshot integrity are
// independently rechecked by the host for every query and action.
class TurnFileEdits final:public QWidget {
    Q_OBJECT
public:
    explicit TurnFileEdits(QWidget* parent=nullptr);
    void setSummary(const QJsonObject& summary,const QString& error={});
    void setPending();
    void setCollapsedFolders(const QSet<QString>& keys);
    QSet<QString> collapsedFolders()const{return m_collapsed;}
    QStringList mutationIds()const{return m_ids;}
    static bool validSummary(const QJsonObject& summary);
    static QHash<QString,TurnFileSelection> selections(const QJsonArray& rows);
signals:
    void fileRequested(QString path,QStringList mutationIds);
    void reviewRequested(QStringList mutationIds);
    void savedFileRequested(QString path,QStringList mutationIds);
    void undoReviewRequested(QStringList mutationIds);
    void retryRequested();
    void folderExpansionChanged(QString key,bool expanded);
protected:
    void changeEvent(QEvent* event)override;
    bool eventFilter(QObject* object,QEvent* event)override;
private:
    void measure();void colors();void toggleAll();void activateFile(QTreeWidgetItem* item);void showActions(const QPoint& globalPosition,const QString& path={});
    QLabel *m_title,*m_status;QToolButton *m_totals,*m_toggle,*m_retry,*m_more;QTreeWidget* m_tree;
    QJsonObject m_summary;QStringList m_ids;QSet<QString> m_collapsed;QHash<QString,QTreeWidgetItem*> m_folders;
    bool m_updating=false,m_coloring=false,m_colorPending=false;
};
}
