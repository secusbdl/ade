#pragma once
#include <QMouseEvent>
#include <QSettings>
#include <QShowEvent>
#include <QResizeEvent>
#include <QSplitter>
#include <QSplitterHandle>
#include <functional>

namespace ade {
// Owns only chrome geometry. Conversation and browser lifetimes stay in MainWindow.
class SidebarSplitter final:public QSplitter {
    Q_OBJECT
public:
    explicit SidebarSplitter(QString settingsFile,QWidget* parent=nullptr)
        :QSplitter(Qt::Horizontal,parent),m_settingsFile(std::move(settingsFile)){
        setObjectName("sidebarSplitter");setHandleWidth(2);setChildrenCollapsible(false);
        QSettings saved(m_settingsFile,QSettings::IniFormat);
        m_collapsed=saved.value("sidebarCollapsed",false).toBool();
        const double width=saved.value("sidebarWidth",0).toDouble();
        if(qIsFinite(width)&&width>0)m_logicalWidth=qBound(176.0,width,672.0);
        connect(this,&QSplitter::splitterMoved,this,[this]{
            if(m_applying||!m_initialized||m_compact||count()<2)return;
            const auto current=sizes();const bool collapsed=current[0]==0;
            if(!collapsed)m_logicalWidth=qBound(176.0,current[0]/m_scale,672.0);
            const bool changed=m_collapsed!=collapsed;m_collapsed=collapsed;persist();
            if(changed)emit collapsedChanged(m_collapsed);
        });
    }
    bool isCollapsed()const{return m_compact?!m_compactNavigation:m_collapsed;}
    bool isCompact()const{return m_compact;}
    QSize minimumSizeHint()const override{return {0,0};}
    void setWorkbenchFullscreen(bool enabled){m_fullscreen=enabled;applyWidth();}
    void showWorkbench(){if(m_compact)setCollapsed(true);}
    double logicalWidth()const{return m_logicalWidth;}
    void setUiScale(double scale){
        m_scale=qBound(0.7,scale,1.3);
        if(count()<2)return;
        setCollapsible(0,true);setStretchFactor(0,0);setStretchFactor(1,1);
        if(m_initialized)updateResponsiveWidth();
    }
public slots:
    void toggle(){setCollapsed(!isCollapsed());}
    void setCollapsed(bool collapsed){
        if(m_compact){if(isCollapsed()==collapsed)return;m_compactNavigation=!collapsed;applyWidth();emit collapsedChanged(isCollapsed());return;}
        if(m_collapsed==collapsed)return;
        m_collapsed=collapsed;if(m_initialized)updateResponsiveWidth(false);persist();emit collapsedChanged(isCollapsed());
    }
    void resetWidth(){m_logicalWidth=qBound(176.0,width()*0.18/m_scale,672.0);m_collapsed=false;applyWidth();persist();emit collapsedChanged(false);}
signals:
    void collapsedChanged(bool collapsed);
protected:
    class Handle final:public QSplitterHandle{
    public:
        Handle(QSplitter* owner,std::function<void()> reset):QSplitterHandle(Qt::Horizontal,owner),m_reset(std::move(reset)){}
    protected:
        void mouseDoubleClickEvent(QMouseEvent* event)override{if(event->button()==Qt::LeftButton){m_reset();event->accept();}else QSplitterHandle::mouseDoubleClickEvent(event);}
    private:std::function<void()> m_reset;
    };
    QSplitterHandle* createHandle()override{return new Handle(this,[this]{resetWidth();});}
    void showEvent(QShowEvent* event)override{
        QSplitter::showEvent(event);
        if(!m_initialized){m_initialized=true;if(m_logicalWidth<=0)m_logicalWidth=qBound(176.0,width()*0.18/m_scale,672.0);setUiScale(m_scale);}
    }
    void resizeEvent(QResizeEvent* event)override{QSplitter::resizeEvent(event);if(m_initialized)updateResponsiveWidth();}
private:
    void updateResponsiveWidth(bool notify=true){
        const bool before=isCollapsed();const bool compact=width()<qRound(520*m_scale);
        if(compact!=m_compact){m_compact=compact;m_compactNavigation=false;}
        applyWidth();if(notify&&before!=isCollapsed())emit collapsedChanged(isCollapsed());
    }
    void applyWidth(){
        if(count()<2)return;
        if(m_fullscreen){widget(0)->hide();widget(1)->show();m_applying=true;setSizes({0,width()});m_applying=false;return;}
        widget(1)->setVisible(!m_compact||!m_compactNavigation);
        widget(0)->setVisible(!m_compact||m_compactNavigation);
        if(m_compact){widget(0)->setMinimumWidth(0);widget(0)->setMaximumWidth(QWIDGETSIZE_MAX);m_applying=true;setSizes(m_compactNavigation?QList<int>{width(),0}:QList<int>{0,width()});m_applying=false;return;}
        widget(0)->setMinimumWidth(m_collapsed?0:qRound(176*m_scale));
        widget(0)->setMaximumWidth(qMax(qRound(176*m_scale),qMin(qRound(672*m_scale),width()/2)));
        m_applying=true;const int side=m_collapsed?0:qMin(widget(0)->maximumWidth(),qRound(qBound(176.0,m_logicalWidth,672.0)*m_scale));
        setSizes({side,qMax(1,width()-handleWidth()-side)});m_applying=false;
    }
    void persist()const{QSettings saved(m_settingsFile,QSettings::IniFormat);saved.setValue("sidebarCollapsed",m_collapsed);if(m_logicalWidth>0)saved.setValue("sidebarWidth",m_logicalWidth);}
    QString m_settingsFile;double m_scale=1,m_logicalWidth=0;bool m_collapsed=false,m_initialized=false,m_applying=false,m_compact=false,m_compactNavigation=false,m_fullscreen=false;
};
}
