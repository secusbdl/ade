#pragma once
#include <QObject>
#include <QColor>
#include <QPoint>
#include <QSize>
#include <deque>
#include "vendor/libvterm-0.3.3/include/vterm.h"

namespace ade {
class TerminalModel final : public QObject {
    Q_OBJECT
public:
    struct Cell {QString text;QColor foreground,background;int width=1;bool bold=false,italic=false,underline=false,strike=false;};
    explicit TerminalModel(QSize cells={100,30},QObject *parent=nullptr);
    ~TerminalModel() override;
    void feed(const QByteArray &bytes);
    bool resize(QSize cells);
    void reset();
    void trimHistory(int maximumLines);
    QSize size()const{return m_size;}
    int historyLines()const{return int(m_history.size());}
    quint64 historyRevision()const{return m_historyRevision;}
    int totalLines()const{return historyLines()+m_size.height();}
    Cell cell(int absoluteRow,int column)const;
    QString lineText(int absoluteRow)const;
    QString plainText(bool includeHistory=true)const;
    QPoint cursor()const;
    bool cursorVisible()const{return m_cursorVisible;}
    bool cursorBlink()const{return m_cursorBlink;}
    int cursorShape()const{return m_cursorShape;}
    bool alternateScreen()const{return m_alt;}
    bool mouseReporting()const{return m_mouseMode!=0;}
    void key(int qtKey,Qt::KeyboardModifiers modifiers,const QString &text);
    void paste(const QString &text);
    void focus(bool focused);
    void mouseMove(int row,int column,Qt::KeyboardModifiers modifiers);
    void mouseButton(int button,bool pressed,Qt::KeyboardModifiers modifiers);
    void setDarkTheme(bool dark);
    QColor background()const{return m_background;}
    QColor foreground()const{return m_foreground;}
signals:
    void changed();
    void outputReady(const QByteArray &bytes);
    void titleChanged(const QString &title);
    void workingDirectoryChanged(const QString &path);
    void bell();
private:
    VTerm *m_terminal=nullptr;VTermScreen *m_screen=nullptr;VTermState *m_state=nullptr;
    QSize m_size;
    std::deque<QVector<VTermScreenCell>> m_history;
    bool m_cursorVisible=true,m_cursorBlink=true,m_alt=false,m_reverse=false;
    int m_cursorShape=VTERM_PROP_CURSORSHAPE_BLOCK,m_mouseMode=0;
    QByteArray m_title,m_osc;
    QByteArray m_pendingInput;
    bool m_inputTransaction=false;
    quint64 m_historyRevision=0;
    QColor m_background,m_foreground;
    static constexpr int HistoryLimit=5000;
    static int property(VTermProp,VTermValue*,void*);
    static int pushLine(int,const VTermScreenCell*,void*);
    static int popLine(int,VTermScreenCell*,void*);
    static int clearHistory(void*);
    static int osc(int,VTermStringFragment,void*);
    void sendOutput(const QByteArray &bytes);
};
}
