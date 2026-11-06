#include "AttachmentStrip.h"
#include "Icons.h"
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QScrollArea>
#include <QToolButton>
namespace ade {
AttachmentStrip::AttachmentStrip(QWidget* parent):QWidget(parent) {
    setObjectName("draftAttachments");
    auto* layout=new QHBoxLayout(this);layout->setContentsMargins(0,0,0,0);
    auto* scroll=new QScrollArea;scroll->setFrameShape(QFrame::NoFrame);scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea {background:transparent;}");scroll->viewport()->setAutoFillBackground(false);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);scroll->setFixedHeight(46);
    auto* contents=new QWidget;contents->setStyleSheet("background:transparent;");m_items=new QHBoxLayout(contents);m_items->setContentsMargins(0,0,0,0);m_items->setSpacing(5);
    scroll->setWidget(contents);layout->addWidget(scroll);hide();
}
void AttachmentStrip::setAttachments(const QJsonArray& references) {
    while(auto* item=m_items->takeAt(0)){if(item->widget()){item->widget()->setEnabled(false);item->widget()->hide();item->widget()->deleteLater();}delete item;}
    for(qsizetype i=0;i<references.size();++i) {
        const auto ref=references.at(i).toObject();const auto path=ref["path"].toString();
        const auto name=ref["originalFileName"].toString(QFileInfo(path).fileName());
        auto* chip=new QToolButton;chip->setObjectName("removeDraftAttachment");chip->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        chip->setStyleSheet("QToolButton {background:palette(button);border:1px solid palette(mid);border-radius:5px;padding:4px 6px;}");
        chip->setIcon(icon("close"));chip->setText(chip->fontMetrics().elidedText(name,Qt::ElideMiddle,180).replace('&',"&&"));
        chip->setToolTip("Remove attachment: "+name+"\n"+path);chip->setAccessibleName("Remove attachment "+name);
        connect(chip,&QToolButton::clicked,this,[this,i]{emit removeRequested(int(i));});m_items->addWidget(chip);
    }
    m_items->addStretch();setVisible(!references.isEmpty());
}
}
