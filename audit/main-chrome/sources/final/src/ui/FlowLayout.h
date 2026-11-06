#pragma once
#include <QLayout>
#include <QWidget>
namespace ade {
// The selectors remain ordinary keyboard-accessible Qt widgets, wrapping as a row.
class FlowLayout final:public QLayout {
public:
    explicit FlowLayout(QWidget* parent=nullptr):QLayout(parent){setContentsMargins(0,0,0,0);setSpacing(8);}
    ~FlowLayout()override{while(auto* item=takeAt(0))delete item;}
    void addItem(QLayoutItem* item)override{m_items.append(item);}
    int count()const override{return m_items.size();}
    QLayoutItem* itemAt(int index)const override{return m_items.value(index);}
    QLayoutItem* takeAt(int index)override{return index>=0&&index<m_items.size()?m_items.takeAt(index):nullptr;}
    Qt::Orientations expandingDirections()const override{return {};}
    bool hasHeightForWidth()const override{return true;}
    int heightForWidth(int width)const override{return arrange({0,0,width,0},true);}
    QSize minimumSize()const override{int height=0;for(auto* item:m_items)height=qMax(height,item->minimumSize().height());return {0,height};}
    QSize sizeHint()const override{int width=0,height=0;for(auto* item:m_items){width+=item->sizeHint().width()+spacing();height=qMax(height,item->sizeHint().height());}return {qMax(0,width-spacing()),height};}
    void setGeometry(const QRect& rect)override{QLayout::setGeometry(rect);arrange(rect,false);}
private:
    int arrange(const QRect& rect,bool measure)const{
        int x=rect.x(),y=rect.y(),lineHeight=0;const int available=qMax(0,rect.width());
        for(auto* item:m_items){
            if(item->isEmpty())continue;
            const auto hint=item->sizeHint();const int itemWidth=qMin(available,hint.width());
            if(x>rect.x()&&x+itemWidth>rect.x()+available){x=rect.x();y+=lineHeight+spacing();lineHeight=0;}
            if(!measure)item->setGeometry({x,y,itemWidth,hint.height()});
            x+=itemWidth+spacing();lineHeight=qMax(lineHeight,hint.height());
        }
        return y-rect.y()+lineHeight;
    }
    QList<QLayoutItem*> m_items;
};
}
