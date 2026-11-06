#pragma once
#include <QAbstractScrollArea>
#include <QTimer>
#include "TerminalModel.h"
#include "ConPtyProcess.h"

namespace ade {
class TerminalWidget final : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit TerminalWidget(QWidget *parent=nullptr);
    ~TerminalWidget() override;
    bool start(const QString &workingDirectory,QString *error=nullptr,const QStringList &command={});
    void stop();
    bool isRunning()const{return m_process->isRunning();}
    quint32 processId()const{return m_process->processId();}
    TerminalModel *terminalModel()const{return m_model;}
    ConPtyProcess *process()const{return m_process;}
    QString workingDirectory()const{return m_cwd;}
    QString selectedText()const;
    void setDarkTheme(bool dark);
    void setZoomPercent(int percent);
    int zoomPercent()const{return m_zoom;}
    QSize cellSize()const{return m_cell;}
public slots:
    void copySelection();
    void pasteClipboard();
signals:
    void titleChanged(const QString &title);
    void workingDirectoryChanged(const QString &path);
    void exited(quint32 exitCode);
    void errorOccurred(const QString &message);
protected:
    void paintEvent(QPaintEvent*)override;
    void resizeEvent(QResizeEvent*)override;
    void keyPressEvent(QKeyEvent*)override;
    void mousePressEvent(QMouseEvent*)override;
    void mouseMoveEvent(QMouseEvent*)override;
    void mouseReleaseEvent(QMouseEvent*)override;
    void wheelEvent(QWheelEvent*)override;
    void focusInEvent(QFocusEvent*)override;
    void focusOutEvent(QFocusEvent*)override;
    void inputMethodEvent(QInputMethodEvent*)override;
    QVariant inputMethodQuery(Qt::InputMethodQuery)const override;
    void contextMenuEvent(QContextMenuEvent*)override;
    bool event(QEvent*)override;
    bool focusNextPrevChild(bool)override{return false;}
private:
    TerminalModel *m_model;
    ConPtyProcess *m_process;
    QString m_cwd;
    QSize m_cell;
    QTimer m_blink;
    bool m_cursorOn=true,m_selecting=false;
    QPoint m_anchor{-1,-1},m_selection{-1,-1};
    int m_zoom=100,m_padding=8;
    quint64 m_historyRevision=0;
    void fit();
    void refresh();
    QPoint position(const QPointF &point)const;
    bool selected(int row,int column)const;
    void clearSelection();
    bool reportMouse(QMouseEvent*,bool pressed);
};
}
