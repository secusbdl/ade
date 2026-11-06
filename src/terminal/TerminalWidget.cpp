#include "TerminalWidget.h"
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShortcut>

namespace ade {
TerminalWidget::TerminalWidget(QWidget *parent):TerminalWidget(nullptr,nullptr,parent){}
TerminalWidget::TerminalWidget(TerminalModel* model,ConPtyProcess* process,QWidget *parent):QAbstractScrollArea(parent),m_model(model),m_process(process),m_borrowed(model||process){
    if(!m_borrowed){m_model=new TerminalModel({100,30},this);m_process=new ConPtyProcess(this);}
    setObjectName("nativeTerminal");setFrameShape(QFrame::NoFrame);setFocusPolicy(Qt::StrongFocus);setAttribute(Qt::WA_InputMethodEnabled);setMouseTracking(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    viewport()->setCursor(Qt::IBeamCursor);viewport()->setAutoFillBackground(false);
    if(!m_borrowed)connect(m_process,&ConPtyProcess::outputReady,m_model,&TerminalModel::feed);
    connect(m_process,&ConPtyProcess::errorOccurred,this,&TerminalWidget::errorOccurred);
    connect(m_process,&ConPtyProcess::exited,this,[this](quint32 code){viewport()->update();emit exited(code);});
    if(!m_borrowed)connect(m_model,&TerminalModel::outputReady,this,[this](const QByteArray &bytes){if(m_process&&m_process->isRunning()&&!m_process->write(bytes))emit errorOccurred("Terminal input queue is full or the shell has exited.");});
    connect(m_model,&TerminalModel::changed,this,&TerminalWidget::refresh);
    connect(m_model,&TerminalModel::titleChanged,this,&TerminalWidget::titleChanged);
    connect(m_model,&TerminalModel::workingDirectoryChanged,this,[this](const QString &cwd){m_cwd=cwd;emit workingDirectoryChanged(cwd);});
    connect(m_model,&TerminalModel::bell,this,[]{QApplication::beep();});
    connect(verticalScrollBar(),&QScrollBar::valueChanged,viewport(),qOverload<>(&QWidget::update));
    m_blink.setInterval(550);connect(&m_blink,&QTimer::timeout,this,[this]{m_cursorOn=!m_cursorOn;viewport()->update();});m_blink.start();
    if(m_borrowed){
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        connect(horizontalScrollBar(),&QScrollBar::valueChanged,viewport(),qOverload<>(&QWidget::update));
        if(m_model)connect(m_model,&QObject::destroyed,this,[this]{m_model=nullptr;setEnabled(false);viewport()->update();});
        if(m_process)connect(m_process,&QObject::destroyed,this,[this]{m_process=nullptr;viewport()->update();});
    }
    setZoomPercent(100);
}
TerminalWidget::~TerminalWidget(){stop();}
bool TerminalWidget::start(const QString &cwd,QString *error,const QStringList &command){
    if(m_borrowed||!m_model||!m_process){if(error)*error="This borrowed shell is controlled by its chat.";return false;}
    if(m_process->isRunning()){if(error)*error="The terminal is already running.";return false;}
    m_model->reset();clearSelection();fit();
    if(!m_process->start(cwd,command,m_model->size(),error))return false;
    m_cwd=m_process->workingDirectory();emit workingDirectoryChanged(m_cwd);return true;
}
void TerminalWidget::stop(){if(!m_borrowed&&m_process)m_process->stop();}
void TerminalWidget::setDarkTheme(bool dark){if(!m_model)return;m_model->setDarkTheme(dark);viewport()->update();}
void TerminalWidget::setZoomPercent(int percent){m_zoom=qBound(50,percent,200);QFont f("Roboto Mono");f.setPixelSize(qMax(7,qRound(13.*m_zoom/100.)));f.setStyleHint(QFont::Monospace);setFont(f);const QFontMetrics metrics(f);m_cell={qMax(1,metrics.horizontalAdvance('M')),qMax(1,metrics.height())};fit();viewport()->update();}
void TerminalWidget::fit(){
    if(!m_model||!m_process||m_cell.isEmpty())return;
    if(m_borrowed){refresh();return;}
    const QSize cells(qBound(2,(viewport()->width()-m_padding*2)/m_cell.width(),500),qBound(1,(viewport()->height()-m_padding*2)/m_cell.height(),300));
    if(cells!=m_model->size()){const QPointer<TerminalWidget> self(this);m_model->resize(cells);if(!self||!m_process)return;if(m_process->isRunning()){QString error;if(!m_process->resize(cells,&error)&&!error.isEmpty())emit errorOccurred(error);}}
}
int TerminalWidget::visibleRows()const{return m_borrowed?qMax(1,(viewport()->height()-m_padding*2)/qMax(1,m_cell.height())):(m_model?m_model->size().height():1);}
int TerminalWidget::contentEnd()const{
    if(!m_model)return 0;
    if(!m_borrowed||m_model->alternateScreen())return m_model->totalLines();
    int end=m_model->historyLines()+m_model->cursor().y()+1;
    for(int row=m_model->totalLines()-1;row>=end;--row)if(!m_model->lineText(row).isEmpty()){end=row+1;break;}
    return end;
}
void TerminalWidget::refresh(){
    if(!m_model)return;
    if(m_historyRevision!=m_model->historyRevision()){m_historyRevision=m_model->historyRevision();clearSelection();}
    auto* bar=verticalScrollBar();const bool bottom=bar->value()==bar->maximum();const QPointer<TerminalWidget> self(this);
    const int rows=visibleRows();
    const int minimum=m_model->alternateScreen()?m_model->historyLines():0;
    bar->setRange(minimum,qMax(minimum,contentEnd()-rows));if(!self||!m_model)return;
    bar->setPageStep(rows);if(bottom||m_model->alternateScreen())bar->setValue(bar->maximum());if(!self||!m_model)return;
    if(m_borrowed){horizontalScrollBar()->setRange(0,qMax(0,m_model->size().width()-qMax(1,(viewport()->width()-m_padding*2)/qMax(1,m_cell.width()))));if(!self)return;}
    viewport()->update();
}
void TerminalWidget::resizeEvent(QResizeEvent *event){QAbstractScrollArea::resizeEvent(event);fit();}
bool TerminalWidget::selected(int row,int column)const{if(m_anchor.x()<0||m_selection.x()<0)return false;auto a=m_anchor,b=m_selection;if(a.y()>b.y()||(a.y()==b.y()&&a.x()>b.x()))std::swap(a,b);return (row>a.y()||(row==a.y()&&column>=a.x()))&&(row<b.y()||(row==b.y()&&column<=b.x()));}
void TerminalWidget::paintEvent(QPaintEvent*){
    QPainter painter(viewport());if(!m_model){painter.fillRect(viewport()->rect(),palette().base());return;}painter.fillRect(viewport()->rect(),m_model->background());const int top=verticalScrollBar()->value();
    const int baseline=QFontMetrics(font()).ascent();
    for(int row=0;row<qMin(visibleRows(),m_model->totalLines()-top);++row)for(int col=0;col<m_model->size().width();++col){
        const auto cell=m_model->cell(top+row,col);if(!cell.width)continue;
        const QRect rect(m_padding+(col-horizontalScrollBar()->value())*m_cell.width(),m_padding+row*m_cell.height(),m_cell.width()*cell.width,m_cell.height());
        painter.fillRect(rect,selected(top+row,col)?QColor(104,130,175,100):cell.background);
        if(cell.text.isEmpty())continue;
        QFont f=font();f.setBold(cell.bold);f.setItalic(cell.italic);f.setUnderline(cell.underline);f.setStrikeOut(cell.strike);painter.setFont(f);painter.setPen(cell.foreground);painter.save();painter.setClipRect(rect);painter.drawText(rect.topLeft()+QPoint(0,baseline),cell.text);painter.restore();
    }
    if(m_model->cursorVisible()&&(!m_model->cursorBlink()||m_cursorOn||!hasFocus())){
        const auto cursor=m_model->cursor();const int row=m_model->historyLines()+cursor.y()-top;
        if(row>=0&&row<visibleRows()){
            QRect rect(m_padding+(cursor.x()-horizontalScrollBar()->value())*m_cell.width(),m_padding+row*m_cell.height(),m_cell.width(),m_cell.height());painter.setPen(m_model->foreground());
            if(!hasFocus())painter.drawRect(rect.adjusted(0,0,-1,-1));else if(m_model->cursorShape()==VTERM_PROP_CURSORSHAPE_BAR_LEFT)painter.fillRect(QRect(rect.topLeft(),QSize(2,rect.height())),m_model->foreground());
            else if(m_model->cursorShape()==VTERM_PROP_CURSORSHAPE_UNDERLINE)painter.fillRect(QRect(rect.bottomLeft()-QPoint(0,1),QSize(rect.width(),2)),m_model->foreground());
            else{auto color=m_model->foreground();color.setAlpha(90);painter.fillRect(rect,color);}
        }
    }
}
void TerminalWidget::clearSelection(){m_anchor={-1,-1};m_selection={-1,-1};viewport()->update();}
QString TerminalWidget::selectedText()const{
    if(!m_model||m_anchor.x()<0)return {};
    auto a=m_anchor,b=m_selection;if(a.y()>b.y()||(a.y()==b.y()&&a.x()>b.x()))std::swap(a,b);QStringList lines;
    for(int row=a.y();row<=b.y();++row){QString line;for(int col=row==a.y()?a.x():0;col<=(row==b.y()?b.x():m_model->size().width()-1);++col){const auto cell=m_model->cell(row,col);if(cell.width)line+=cell.text.isEmpty()?QStringLiteral(" "):cell.text;}while(line.endsWith(' '))line.chop(1);lines<<line;}return lines.join('\n');
}
void TerminalWidget::copySelection(){const auto text=selectedText();if(!text.isEmpty())QApplication::clipboard()->setText(text);}
void TerminalWidget::pasteClipboard(){if(!m_model||!isRunning())return;clearSelection();verticalScrollBar()->setValue(verticalScrollBar()->maximum());m_model->paste(QApplication::clipboard()->text());}
void TerminalWidget::keyPressEvent(QKeyEvent *event){
    if(!m_model||!m_process){event->ignore();return;}
    const auto mods=event->modifiers();const bool ctrl=mods.testFlag(Qt::ControlModifier),shift=mods.testFlag(Qt::ShiftModifier);
    if(ctrl&&shift&&event->key()==Qt::Key_C){copySelection();event->accept();return;}
    if((ctrl&&shift&&event->key()==Qt::Key_V)||(shift&&event->key()==Qt::Key_Insert)){pasteClipboard();event->accept();return;}
    if(ctrl&&(event->key()==Qt::Key_Plus||event->key()==Qt::Key_Equal||event->key()==Qt::Key_Minus||event->key()==Qt::Key_0)){setZoomPercent(event->key()==Qt::Key_0?100:m_zoom+(event->key()==Qt::Key_Minus?-10:10));event->accept();return;}
    if(shift&&(event->key()==Qt::Key_PageUp||event->key()==Qt::Key_PageDown)){verticalScrollBar()->setValue(verticalScrollBar()->value()+(event->key()==Qt::Key_PageUp?-1:1)*m_model->size().height());event->accept();return;}
    clearSelection();verticalScrollBar()->setValue(verticalScrollBar()->maximum());m_cursorOn=true;m_model->key(event->key(),mods,event->text());event->accept();
}
QPoint TerminalWidget::position(const QPointF &point)const{if(!m_model)return {};return {qBound(0,(int(point.x())-m_padding)/m_cell.width()+horizontalScrollBar()->value(),m_model->size().width()-1),qMin(m_model->totalLines()-1,qBound(0,(int(point.y())-m_padding)/m_cell.height(),visibleRows()-1)+verticalScrollBar()->value())};}
bool TerminalWidget::reportMouse(QMouseEvent *event,bool pressed){if(!m_model||!m_model->mouseReporting()||event->modifiers().testFlag(Qt::ShiftModifier))return false;const auto p=position(event->position());m_model->mouseMove(p.y()-m_model->historyLines(),p.x(),event->modifiers());int button=event->button()==Qt::LeftButton?1:event->button()==Qt::MiddleButton?2:event->button()==Qt::RightButton?3:0;if(button)m_model->mouseButton(button,pressed,event->modifiers());return true;}
void TerminalWidget::mousePressEvent(QMouseEvent *event){if(!m_model)return;setFocus(Qt::MouseFocusReason);if(reportMouse(event,true)){event->accept();return;}if(event->button()==Qt::LeftButton){m_selecting=true;m_anchor=m_selection=position(event->position());viewport()->update();event->accept();}else QAbstractScrollArea::mousePressEvent(event);}
void TerminalWidget::mouseMoveEvent(QMouseEvent *event){if(!m_model)return;if(m_model->mouseReporting()&&!event->modifiers().testFlag(Qt::ShiftModifier)){const auto p=position(event->position());m_model->mouseMove(p.y()-m_model->historyLines(),p.x(),event->modifiers());}else if(m_selecting){m_selection=position(event->position());viewport()->update();}event->accept();}
void TerminalWidget::mouseReleaseEvent(QMouseEvent *event){if(!m_model)return;if(!reportMouse(event,false)&&m_selecting){m_selecting=false;m_selection=position(event->position());viewport()->update();}event->accept();}
void TerminalWidget::wheelEvent(QWheelEvent *event){if(!m_model)return;if(event->modifiers().testFlag(Qt::ControlModifier)){setZoomPercent(m_zoom+(event->angleDelta().y()>0?10:-10));event->accept();return;}if(m_model->mouseReporting()&&!event->modifiers().testFlag(Qt::ShiftModifier)){const auto p=position(event->position());m_model->mouseMove(p.y()-m_model->historyLines(),p.x(),event->modifiers());const int button=event->angleDelta().y()>0?4:5;for(int i=0;i<qMax(1,qAbs(event->angleDelta().y())/120);++i){m_model->mouseButton(button,true,event->modifiers());m_model->mouseButton(button,false,event->modifiers());}event->accept();return;}QAbstractScrollArea::wheelEvent(event);}
void TerminalWidget::focusInEvent(QFocusEvent *event){QAbstractScrollArea::focusInEvent(event);if(m_model)m_model->focus(true);m_cursorOn=true;viewport()->update();}
void TerminalWidget::focusOutEvent(QFocusEvent *event){QAbstractScrollArea::focusOutEvent(event);if(m_model)m_model->focus(false);viewport()->update();}
void TerminalWidget::inputMethodEvent(QInputMethodEvent *event){if(m_model&&!event->commitString().isEmpty())m_model->key(0,Qt::NoModifier,event->commitString());event->accept();}
QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery query)const{if(m_model&&query==Qt::ImCursorRectangle){const auto p=m_model->cursor();return QRect(m_padding+(p.x()-horizontalScrollBar()->value())*m_cell.width(),m_padding+(m_model->historyLines()+p.y()-verticalScrollBar()->value())*m_cell.height(),m_cell.width(),m_cell.height());}if(query==Qt::ImEnabled)return bool(m_model)&&isRunning();return QAbstractScrollArea::inputMethodQuery(query);}
void TerminalWidget::contextMenuEvent(QContextMenuEvent *event){QMenu menu(this);auto *copy=menu.addAction("Copy",this,&TerminalWidget::copySelection);copy->setEnabled(!selectedText().isEmpty());auto *paste=menu.addAction("Paste",this,&TerminalWidget::pasteClipboard);paste->setEnabled(isRunning());menu.addSeparator();menu.addAction("Reset zoom",this,[this]{setZoomPercent(100);});menu.exec(event->globalPos());}
bool TerminalWidget::event(QEvent *event){
    if(event->type()==QEvent::ShortcutOverride){
        auto *key=static_cast<QKeyEvent*>(event);
        const bool ctrl=key->modifiers().testFlag(Qt::ControlModifier),alt=key->modifiers().testFlag(Qt::AltModifier);
        bool applicationShortcut=key->key()==Qt::Key_F11||(ctrl&&(key->key()==Qt::Key_W||key->key()==Qt::Key_T||key->key()==Qt::Key_QuoteLeft||(alt&&(key->key()==Qt::Key_Left||key->key()==Qt::Key_Right))));
        // Host-marked navigation wins over terminal input. Other terminal
        // bindings (notably Ctrl+C) retain their existing input behavior.
        if(!applicationShortcut)for(auto* shortcut:window()->findChildren<QShortcut*>()){
            if(!shortcut->isEnabled()||!shortcut->property("adeDominantNavigation").toBool())continue;
            for(const auto& sequence:shortcut->keys())
                if(sequence.matches(QKeySequence(key->keyCombination()))==QKeySequence::ExactMatch){applicationShortcut=true;break;}
            if(applicationShortcut)break;
        }
        if(applicationShortcut){event->ignore();return false;}
        event->accept();return true;
    }
    return QAbstractScrollArea::event(event);
}
}
