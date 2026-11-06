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
            if(m_applying||!m_initialized||count()<2)return;
            const auto current=sizes();const bool collapsed=current[0]==0;
            if(!collapsed)m_logicalWidth=qBound(176.0,current[0]/m_scale,672.0);
            const bool changed=m_collapsed!=collapsed;m_collapsed=collapsed;persist();
            if(changed)emit collapsedChanged(m_collapsed);
        });
    }
    bool isCollapsed()const{return m_collapsed;}
    double logicalWidth()const{return m_logicalWidth;}
    void setUiScale(double scale){
        m_scale=qBound(0.7,scale,1.3);
        if(count()<2)return;
        setCollapsible(0,true);setStretchFactor(0,0);setStretchFactor(1,1);
        if(m_initialized)applyWidth();
    }
public slots:
    void toggle(){setCollapsed(!m_collapsed);}
    void setCollapsed(bool collapsed){
        if(m_collapsed==collapsed)return;
        m_collapsed=collapsed;if(m_initialized)applyWidth();persist();emit collapsedChanged(collapsed);
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
    void resizeEvent(QResizeEvent* event)override{QSplitter::resizeEvent(event);if(m_initialized)applyWidth();}
private:
    void applyWidth(){
        if(count()<2)return;
        widget(0)->setMinimumWidth(m_collapsed?0:qRound(176*m_scale));
        widget(0)->setMaximumWidth(qMax(qRound(176*m_scale),qMin(qRound(672*m_scale),width()/2)));
        m_applying=true;const int side=m_collapsed?0:qRound(qBound(176.0,m_logicalWidth,672.0)*m_scale);
        setSizes({side,qMax(1,width()-handleWidth()-side)});m_applying=false;
    }
    void persist()const{QSettings saved(m_settingsFile,QSettings::IniFormat);saved.setValue("sidebarCollapsed",m_collapsed);if(m_logicalWidth>0)saved.setValue("sidebarWidth",m_logicalWidth);}
    QString m_settingsFile;double m_scale=1,m_logicalWidth=0;bool m_collapsed=false,m_initialized=false,m_applying=false;
};
}
