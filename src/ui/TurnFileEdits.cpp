#include "TurnFileEdits.h"
#include <QHeaderView>
#include <QApplication>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QMouseEvent>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QToolButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <cmath>
#include <algorithm>
#include <functional>
namespace ade {
namespace {
constexpr int FolderRole=Qt::UserRole,KeyRole=Qt::UserRole+1,PathRole=Qt::UserRole+2,AddedRole=Qt::UserRole+3,RemovedRole=Qt::UserRole+4;
bool token(const QString& value){return QRegularExpression("^[A-Za-z0-9_-]{1,128}$").match(value).hasMatch();}
bool count(const QJsonValue& value){return value.isDouble()&&value.toDouble()>=0&&value.toDouble()<=100000000&&std::floor(value.toDouble())==value.toDouble();}
QString normalized(QString path){return path.replace('\\','/');}
QStringList strings(const QJsonArray& values){QStringList result;for(const auto& value:values)result.append(value.toString());return result;}
class FilesTree final:public QTreeWidget {
public:using QTreeWidget::QTreeWidget;
protected:
    void mousePressEvent(QMouseEvent* event)override{
        auto* item=itemAt(event->position().toPoint());
        if(event->button()==Qt::LeftButton&&item&&item->data(0,FolderRole).toBool()){setCurrentItem(item);setFocus();item->setExpanded(!item->isExpanded());event->accept();return;}
        QTreeWidget::mousePressEvent(event);
    }
};
QIcon fileIcon(bool folder,const QColor& ink){QPixmap image(14,14);image.fill(Qt::transparent);QPainter p(&image);p.setRenderHint(QPainter::Antialiasing);p.setPen(QPen(folder?QColor("#ad873e"):ink,1));if(folder){p.drawPolyline(QPolygonF{{1,4},{1,2},{5,2},{7,4},{13,4},{13,12},{1,12},{1,4}});}else{p.drawPolyline(QPolygonF{{3,1},{9,1},{12,4},{12,13},{3,13},{3,1}});p.drawLine(9,1,9,4);p.drawLine(9,4,12,4);}return QIcon(image);}
}
QHash<QString,TurnFileSelection> TurnFileEdits::selections(const QJsonArray& rows){
    QHash<QString,TurnFileSelection> result;QString last;TurnFileSelection current;QSet<QString> seen;
    const auto flush=[&]{if(!last.isEmpty()&&(!current.mutationIds.isEmpty()||current.incomplete))result.insert(last,current);};
    for(const auto& value:rows){const auto row=value.toObject();
        if(row["role"]!="assistant"){flush();last.clear();current={};seen.clear();continue;}
        last=row["id"].toString();current.incomplete|=row["mutationsIncomplete"].toBool();
        for(const auto& entry:row["mutations"].toArray()){const auto record=entry.toObject();const auto id=record["mutationId"].toString();if(!token(id)){current.incomplete=true;continue;}if(!seen.contains(id)){seen.insert(id);current.mutationIds.append(id);}if(current.mutationIds.size()>100)current.incomplete=true;}
    }
    flush();return result;
}
bool TurnFileEdits::validSummary(const QJsonObject& value){
    if(value["version"]!=1||!value["mutationIds"].isArray()||!value["files"].isArray()||value["files"].toArray().size()>1000)return false;
    const auto ids=strings(value["mutationIds"].toArray());if(ids.isEmpty()||ids.size()>100)return false;QSet<QString> known;
    for(const auto& id:ids){if(!token(id)||known.contains(id))return false;known.insert(id);}
    QSet<QString> paths;
    for(const auto& entry:value["files"].toArray()){
        if(!entry.isObject())return false;
        const auto file=entry.toObject();const auto path=normalized(file["path"].toString());
        if(path.isEmpty()||path.size()>32768||path.contains(QRegularExpression("[\\x00-\\x1f\\x7f]"))||paths.contains(path)||path.split('/').contains(".."))return false;
        paths.insert(path);
        if(!count(file["added"])||!count(file["removed"])||!file["statsKnown"].isBool()||!file["mutationIds"].isArray()||file["mutationIds"].toArray().isEmpty())return false;
        for(const auto& id:file["mutationIds"].toArray())if(!id.isString()||!known.contains(id.toString()))return false;
        if(!file["statsKnown"].toBool()&&!QStringList{"created","deleted","modified"}.contains(file["changeType"].toString()))return false;
    }
    return !value.contains("directoryCount")||count(value["directoryCount"]);
}
TurnFileEdits::TurnFileEdits(QWidget* parent):QWidget(parent){
    for(auto* ancestor=parentWidget();ancestor;ancestor=ancestor->parentWidget())ancestor->installEventFilter(this);
    setObjectName("turnFileEdits");setAttribute(Qt::WA_StyledBackground);setMinimumWidth(0);
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(8,3,8,6);layout->setSpacing(3);
    auto* header=new QHBoxLayout;header->setSpacing(5);m_title=new QLabel("Changed files");m_title->setObjectName("turnFilesTitle");m_title->setTextFormat(Qt::PlainText);
    m_totals=new QToolButton;m_totals->setObjectName("turnFilesReview");m_totals->setToolTip("Open changed files");m_totals->setCursor(Qt::PointingHandCursor);
    m_toggle=new QToolButton;m_toggle->setObjectName("turnFilesToggleAll");m_toggle->setArrowType(Qt::UpArrow);m_toggle->setFixedSize(24,24);
    m_more=new QToolButton;m_more->setText("...");m_more->setToolTip("Saved change actions");m_more->setAccessibleName(m_more->toolTip());m_more->setObjectName("turnFilesSavedActions");m_more->setFixedWidth(24);
    header->addWidget(m_title);header->addStretch();header->addWidget(m_totals);header->addWidget(m_toggle);header->addWidget(m_more);layout->addLayout(header);
    m_tree=new FilesTree;m_tree->setObjectName("turnFilesTree");m_tree->setColumnCount(3);m_tree->setHeaderHidden(true);m_tree->setFrameShape(QFrame::NoFrame);m_tree->setIndentation(14);m_tree->setUniformRowHeights(true);m_tree->setRootIsDecorated(true);m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);m_tree->setTextElideMode(Qt::ElideMiddle);m_tree->setSelectionMode(QAbstractItemView::SingleSelection);m_tree->header()->setSectionResizeMode(0,QHeaderView::Stretch);m_tree->header()->setSectionResizeMode(1,QHeaderView::ResizeToContents);m_tree->header()->setSectionResizeMode(2,QHeaderView::ResizeToContents);m_tree->header()->setStretchLastSection(false);layout->addWidget(m_tree);
    m_status=new QLabel;m_status->setObjectName("turnFilesStatus");m_status->setTextFormat(Qt::PlainText);m_status->setWordWrap(true);layout->addWidget(m_status);
    m_retry=new QToolButton;m_retry->setText("Retry saved changes");m_retry->setObjectName("turnFilesRetry");layout->addWidget(m_retry,0,Qt::AlignLeft);
    connect(m_retry,&QToolButton::clicked,this,&TurnFileEdits::retryRequested);
    connect(m_totals,&QToolButton::clicked,this,[this]{const auto ids=m_ids;if(!ids.isEmpty())emit reviewRequested(ids);});
    connect(m_toggle,&QToolButton::clicked,this,&TurnFileEdits::toggleAll);
    connect(m_more,&QToolButton::clicked,this,[this]{showActions(m_more->mapToGlobal(QPoint(0,m_more->height())));});
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);connect(m_tree,&QTreeWidget::customContextMenuRequested,this,[this](const QPoint& position){auto* item=m_tree->itemAt(position);if(item&&!item->data(0,FolderRole).toBool())showActions(m_tree->viewport()->mapToGlobal(position),item->data(0,PathRole).toString());});
    connect(m_tree,&QTreeWidget::itemClicked,this,[this](QTreeWidgetItem* item,int){activateFile(item);});
    connect(m_tree,&QTreeWidget::itemActivated,this,[this](QTreeWidgetItem* item,int){activateFile(item);});
    const auto expanded=[this](QTreeWidgetItem* item,bool value){if(m_updating)return;const auto key=item->data(0,KeyRole).toString();if(value)m_collapsed.remove(key);else m_collapsed.insert(key);measure();emit folderExpansionChanged(key,value);};
    connect(m_tree,&QTreeWidget::itemExpanded,this,[expanded](QTreeWidgetItem* item){expanded(item,true);});connect(m_tree,&QTreeWidget::itemCollapsed,this,[expanded](QTreeWidgetItem* item){expanded(item,false);});
    colors();setPending();
}
void TurnFileEdits::setPending(){m_summary={};m_ids.clear();m_tree->hide();m_totals->hide();m_toggle->hide();m_retry->hide();m_more->hide();m_title->setText("Changed files");m_status->setText("Loading saved changes…");m_status->show();}
void TurnFileEdits::setSummary(const QJsonObject& summary,const QString& error){
    if(!error.isEmpty()||!validSummary(summary)){setPending();m_status->setText(error.isEmpty()?QString("Saved changes are unavailable."):error);m_retry->show();return;}
    if(m_summary==summary)return;
    m_summary=summary;m_ids=strings(summary["mutationIds"].toArray());m_updating=true;const QSignalBlocker blocker(m_tree);
    const auto selected=m_tree->currentItem()?m_tree->currentItem()->data(0,PathRole).toString():QString{};m_tree->clear();m_folders.clear();
    const auto files=summary["files"].toArray();QSet<QString> mounts;for(const auto& entry:files)mounts.insert(normalized(entry.toObject()["path"].toString()).section('/',0,0));
    qint64 added=0,removed=0;int unknown=0;
    for(const auto& entry:files){const auto file=entry.toObject();const auto path=normalized(file["path"].toString());auto parts=path.split('/',Qt::SkipEmptyParts);const bool dropRoot=mounts.size()==1&&QStringList{"ws1","w1"}.contains(parts.value(0));if(dropRoot)parts.removeFirst();if(parts.isEmpty())continue;
        QTreeWidgetItem* parent=nullptr;QString key=dropRoot?path.section('/',0,0):QString{};
        for(int i=0;i+1<parts.size();++i){key+=(key.isEmpty()?QString():"/")+parts[i];auto* folder=m_folders.value(key);if(!folder){folder=parent?new QTreeWidgetItem(parent):new QTreeWidgetItem(m_tree);folder->setText(0,parts[i]);folder->setData(0,FolderRole,true);folder->setData(0,KeyRole,key);folder->setToolTip(0,key);m_folders.insert(key,folder);}parent=folder;}
        auto* item=parent?new QTreeWidgetItem(parent):new QTreeWidgetItem(m_tree);item->setText(0,parts.last());item->setData(0,PathRole,path);item->setToolTip(0,path);item->setSizeHint(0,{0,24});
        const bool known=file["statsKnown"].toBool();const int a=file["added"].toInt(),r=file["removed"].toInt();added+=a;removed+=r;
        if(known){item->setText(1,a?"+"+QString::number(a):QString());item->setText(2,r?"-"+QString::number(r):QString());}
        else{++unknown;item->setText(2,file["changeType"]=="created"?QString("new"):file["changeType"].toString());item->setToolTip(2,file["countReason"].toString());}
        for(auto* ancestor=parent;ancestor;ancestor=ancestor->parent()){ancestor->setData(0,AddedRole,ancestor->data(0,AddedRole).toLongLong()+a);ancestor->setData(0,RemovedRole,ancestor->data(0,RemovedRole).toLongLong()+r);}
        if(path==selected)m_tree->setCurrentItem(item);
    }
    std::function<void(QTreeWidgetItem*)> sort=[&](QTreeWidgetItem* parent){auto children=parent?parent->takeChildren():QList<QTreeWidgetItem*>{};if(!parent)while(m_tree->topLevelItemCount())children.append(m_tree->takeTopLevelItem(0));std::sort(children.begin(),children.end(),[](auto* a,auto* b){const bool af=a->data(0,FolderRole).toBool(),bf=b->data(0,FolderRole).toBool();return af!=bf?af:QString::localeAwareCompare(a->text(0),b->text(0))<0;});for(auto* child:children){if(parent)parent->addChild(child);else m_tree->addTopLevelItem(child);sort(child);}};sort(nullptr);
    for(auto it=m_folders.cbegin();it!=m_folders.cend();++it){auto* folder=it.value();const auto a=folder->data(0,AddedRole).toLongLong(),r=folder->data(0,RemovedRole).toLongLong();folder->setText(1,a?"+"+QString::number(a):QString());folder->setText(2,r?"-"+QString::number(r):QString());folder->setExpanded(!m_collapsed.contains(it.key()));folder->setSizeHint(0,{0,24});}
    if(!selected.isEmpty())for(QTreeWidgetItemIterator it(m_tree);*it;++it)if((*it)->data(0,PathRole).toString()==selected)m_tree->setCurrentItem(*it);
    m_title->setText(QString("Changed files %1").arg(files.size()));m_totals->setText(QString("+%1  -%2").arg(added).arg(removed)+(unknown?QString("  · %1 uncounted").arg(unknown):QString()));m_totals->setAccessibleName("Open changed files");m_totals->show();m_more->show();m_toggle->setVisible(!m_folders.isEmpty());m_tree->setVisible(!files.isEmpty());m_retry->hide();
    QString status;if(summary["directoryCount"].toInt())status=QString("%1 directory changes").arg(summary["directoryCount"].toInt());if(files.isEmpty()&&status.isEmpty())status="No net file changes in this turn.";m_status->setText(status);m_status->setVisible(!status.isEmpty());m_updating=false;colors();measure();
}
void TurnFileEdits::setCollapsedFolders(const QSet<QString>& keys){m_collapsed=keys;const QSignalBlocker blocker(m_tree);for(auto it=m_folders.cbegin();it!=m_folders.cend();++it)it.value()->setExpanded(!keys.contains(it.key()));measure();}
void TurnFileEdits::toggleAll(){const bool expand=std::all_of(m_folders.cbegin(),m_folders.cend(),[](auto* item){return !item->isExpanded();});const auto keys=m_folders.keys();const QPointer<TurnFileEdits> self(this);for(const auto& key:keys){auto* folder=m_folders.value(key);if(folder)folder->setExpanded(expand);if(!self)return;}measure();}
void TurnFileEdits::activateFile(QTreeWidgetItem* item){if(!item||item->data(0,FolderRole).toBool())return;const auto path=item->data(0,PathRole).toString();const auto ids=m_ids;if(!path.isEmpty()&&!ids.isEmpty())emit fileRequested(path,ids);}
void TurnFileEdits::showActions(const QPoint& position,const QString& path){
    const auto ids=m_ids;if(ids.isEmpty())return;auto* menu=new QMenu(this);menu->setAttribute(Qt::WA_DeleteOnClose);const QPointer<TurnFileEdits> self(this);
    if(!path.isEmpty()){auto* saved=menu->addAction("Open saved diff");saved->setObjectName("openSavedTurnDiff");connect(saved,&QAction::triggered,this,[self,path,ids]{if(self&&self->m_ids==ids)emit self->savedFileRequested(path,ids);});}
    auto* undo=menu->addAction("Review turn for undo");undo->setObjectName("reviewTurnUndo");connect(undo,&QAction::triggered,this,[self,ids]{if(self&&self->m_ids==ids)emit self->undoReviewRequested(ids);});menu->popup(position);
}
void TurnFileEdits::measure(){int visible=0;std::function<void(QTreeWidgetItem*)> visit=[&](auto* item){++visible;if(item->isExpanded())for(int i=0;i<item->childCount();++i)visit(item->child(i));};for(int i=0;i<m_tree->topLevelItemCount();++i)visit(m_tree->topLevelItem(i));m_tree->setFixedHeight(qBound(24,visible*24+2,288));const bool expand=std::all_of(m_folders.cbegin(),m_folders.cend(),[](auto* item){return !item->isExpanded();});m_toggle->setToolTip(expand?"Expand all":"Collapse all");m_toggle->setAccessibleName(m_toggle->toolTip());m_toggle->setArrowType(expand?Qt::DownArrow:Qt::UpArrow);}
void TurnFileEdits::colors(){
    if(m_coloring)return;
    m_coloring=true;const QPointer<TurnFileEdits> self(this);
    // A transparent stylesheet turns this widget's Base/Window transparent
    // black. Resolve the actual opaque host surface instead of that placeholder.
    QColor surface=palette().color(QPalette::Base);
    for(auto* context=parentWidget();surface.alpha()<240&&context;context=context->parentWidget())surface=context->palette().color(QPalette::Base);
    if(surface.alpha()<240)surface=qApp->palette().color(QPalette::Base);
    const bool dark=surface.lightness()<128;
    const auto style=QString("QWidget#turnFileEdits{border:1px solid %1;border-radius:7px;background:transparent;} QTreeWidget#turnFilesTree{background:transparent;border:0;} QTreeWidget::item{padding:0;} QToolButton{border:0;background:transparent;padding:3px;}").arg(dark?"#3c424c":"#e1e4e8");if(styleSheet()!=style)setStyleSheet(style);if(!self)return;
    const QColor ink=palette().color(QPalette::Text),green=dark?QColor("#77ca96"):QColor("#24733c"),red=dark?QColor("#ed9292"):QColor("#af3131");for(QTreeWidgetItemIterator it(m_tree);*it;++it){(*it)->setIcon(0,fileIcon((*it)->data(0,FolderRole).toBool(),ink));(*it)->setForeground(1,green);(*it)->setForeground(2,(*it)->text(2).startsWith('-')?red:ink);}m_coloring=false;
}
void TurnFileEdits::changeEvent(QEvent* event){QWidget::changeEvent(event);if(event->type()==QEvent::PaletteChange)colors();}
bool TurnFileEdits::eventFilter(QObject* object,QEvent* event){
    if((event->type()==QEvent::PaletteChange||event->type()==QEvent::StyleChange)&&!m_colorPending){m_colorPending=true;QTimer::singleShot(0,this,[this]{m_colorPending=false;colors();});}
    return QWidget::eventFilter(object,event);
}
}
