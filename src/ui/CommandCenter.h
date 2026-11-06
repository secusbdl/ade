#pragma once
#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
class QLineEdit;class QListWidget;class QLabel;class QComboBox;class QCheckBox;class QToolButton;class QTimer;
namespace ade {
class CommandCenter final:public QDialog {
    Q_OBJECT
public:
    explicit CommandCenter(QWidget* parent=nullptr);
    void present(const QString& mode="global",bool contentSearch=false);
    void setWorkspaces(const QJsonArray& workspaces); // {id,title}; empty id means all.
    void setIncludeIgnored(bool value);
    bool setResults(quint64 request,const QJsonArray& items,const QString& status={});
    void refresh();
    QString mode()const{return m_mode;}
    quint64 requestId()const{return m_request;}
    static QJsonArray rank(const QJsonArray& items,const QString& query,int limit=128);
signals:
    void queryRequested(quint64 request,QString mode,QString query,QString workspace,bool contentSearch,bool includeIgnored);
    void actionRequested(quint64 request,QJsonObject item,QString action);
    void includeIgnoredChanged(bool value);
protected:
    bool eventFilter(QObject*,QEvent*)override;
    void resizeEvent(QResizeEvent*)override;
    void hideEvent(QHideEvent*)override;
    void changeEvent(QEvent*)override;
private:
    void chooseMode(const QString& mode);void invalidate();void dispatch();void activate(const QString& action="open");void updateActions();void position();void applyAppearance();
    QLineEdit* m_query=nullptr;QListWidget* m_results=nullptr;QLabel* m_status=nullptr;QComboBox* m_workspace=nullptr;QCheckBox *m_content=nullptr,*m_ignored=nullptr;
    QList<QToolButton*> m_modes;QToolButton *m_rename=nullptr,*m_pin=nullptr,*m_delete=nullptr,*m_copy=nullptr;
    QToolButton* m_filterMenu=nullptr;
    QTimer* m_debounce=nullptr;QString m_mode="global";quint64 m_request=0;QPointer<QWidget> m_returnFocus;
};
}
