#include "CommandCenter.h"
#include "Icons.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
namespace ade {
namespace {
const QStringList modes{"global","agents","browser","terminals","files","settings"};
const QStringList labels{"All","Chats","Browser","Terminals","Files","Settings"};
const QStringList icons{"search","user","globe","terminal","folder","settings"};
int score(const QJsonObject& item,const QString& query){
    if(query.isEmpty())return item["recency"].toDouble()>0?10:0;
    const auto title=item["title"].toString().toLower();
    if(title==query)return 1000;
    if(title.startsWith(query))return 900;
    if(title.contains(query))return 700;
    const auto keywords=item["keywords"].toArray();
    for(const auto &value:keywords)if(value.toString().toLower()==query)return 650;
    for(const auto &value:keywords)if(value.toString().toLower().startsWith(query))return 600;
    for(const auto &value:keywords)if(value.toString().toLower().contains(query))return 500;
    if(item["subtitle"].toString().toLower().contains(query))return 350;
    if(item["url"].toString().toLower().contains(query))return 300;
    return -1;
}
}
QJsonArray CommandCenter::rank(const QJsonArray& items,const QString& query,int limit){
    struct Ranked{QJsonObject value;int score;double recency;};QList<Ranked> sorted;
    for(const auto& value:items){auto item=value.toObject();const int valueScore=score(item,query.trimmed().toLower());if(valueScore>=0)sorted.append({item,valueScore,item["recency"].toDouble()});}
    std::stable_sort(sorted.begin(),sorted.end(),[](const Ranked& a,const Ranked& b){return a.score!=b.score?a.score>b.score:a.recency>b.recency;});
    QJsonArray result;for(const auto& entry:sorted){if(result.size()>=qBound(0,limit,512))break;auto item=entry.value;item["score"]=entry.score;result.append(item);}return result;
}
CommandCenter::CommandCenter(QWidget* parent):QDialog(parent){
    setObjectName("commandCenter");setWindowTitle("Command center");setWindowFlags(Qt::Dialog|Qt::FramelessWindowHint);setWindowModality(Qt::WindowModal);
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(12,12,12,10);layout->setSpacing(8);
    m_query=new QLineEdit; m_query->setObjectName("commandQuery");m_query->setClearButtonEnabled(true);m_query->setPlaceholderText("Search anything…");m_query->setMaxLength(200);layout->addWidget(m_query);
    auto* modesRow=new QHBoxLayout;modesRow->setSpacing(3);
    for(int index=0;index<modes.size();++index){auto* button=new QToolButton;button->setObjectName("commandMode_"+modes[index]);button->setIcon(icon(icons[index]));button->setToolTip(labels[index]);button->setAccessibleName(labels[index]);button->setCheckable(true);button->setAutoRaise(true);button->setMinimumHeight(28);m_modes.append(button);modesRow->addWidget(button);connect(button,&QToolButton::clicked,this,[this,index]{chooseMode(modes[index]);});}modesRow->addStretch();m_filterMenu=new QToolButton;m_filterMenu->setObjectName("commandFilters");m_filterMenu->setIcon(icon("settings"));m_filterMenu->setToolTip("File search filters");m_filterMenu->setAccessibleName("File search filters");m_filterMenu->setPopupMode(QToolButton::InstantPopup);modesRow->addWidget(m_filterMenu);layout->addLayout(modesRow);
    m_workspace=new QComboBox;m_workspace->setObjectName("commandWorkspace");m_workspace->setMinimumWidth(0);m_workspace->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);m_workspace->addItem("All workspaces","");layout->addWidget(m_workspace);
    auto* filters=new QHBoxLayout;m_content=new QCheckBox("Search contents");m_content->setObjectName("commandContentSearch");m_ignored=new QCheckBox("Include ignored");m_ignored->setObjectName("commandIncludeIgnored");filters->addWidget(m_content);filters->addWidget(m_ignored);filters->addStretch();layout->addLayout(filters);
    auto* filterMenu=new QMenu(m_filterMenu);m_filterMenu->setMenu(filterMenu);connect(filterMenu,&QMenu::aboutToShow,this,[this,filterMenu]{filterMenu->clear();auto* contents=filterMenu->addAction("Search contents (Ctrl+F)");contents->setCheckable(true);contents->setChecked(m_content->isChecked());connect(contents,&QAction::toggled,m_content,&QCheckBox::setChecked);auto* ignored=filterMenu->addAction("Include ignored (Ctrl+G)");ignored->setCheckable(true);ignored->setChecked(m_ignored->isChecked());connect(ignored,&QAction::toggled,m_ignored,&QCheckBox::setChecked);if(m_mode=="files"){auto* workspaces=filterMenu->addMenu("Workspace");for(int index=0;index<m_workspace->count();++index){auto* action=workspaces->addAction(m_workspace->itemText(index));action->setCheckable(true);action->setChecked(index==m_workspace->currentIndex());connect(action,&QAction::triggered,this,[this,index]{m_workspace->setCurrentIndex(index);});}}});
    m_results=new QListWidget;m_results->setObjectName("commandResults");m_results->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);m_results->setTextElideMode(Qt::ElideRight);m_results->setMinimumHeight(65);layout->addWidget(m_results,1);
    m_status=new QLabel;m_status->setObjectName("commandStatus");m_status->setWordWrap(true);layout->addWidget(m_status);
    auto* actions=new QHBoxLayout;
    auto action=[&](const QString& name,const QString& text,const QString& verb){auto* button=new QToolButton;button->setObjectName(name);button->setText(text);button->setAutoRaise(true);actions->addWidget(button);connect(button,&QToolButton::clicked,this,[this,verb]{activate(verb);});return button;};
    m_rename=action("commandRename","Rename","rename");m_pin=action("commandPin","Pin","pin");m_delete=action("commandDelete","Delete","delete");m_copy=action("commandCopy","Copy URL","copy");actions->addStretch();layout->addLayout(actions);
    m_debounce=new QTimer(this);m_debounce->setSingleShot(true);m_debounce->setInterval(80);connect(m_debounce,&QTimer::timeout,this,&CommandCenter::dispatch);
    connect(m_query,&QLineEdit::textChanged,this,[this]{invalidate();m_debounce->start();});
    connect(m_workspace,&QComboBox::currentIndexChanged,this,[this]{refresh();});connect(m_content,&QCheckBox::toggled,this,[this]{refresh();});
    connect(m_ignored,&QCheckBox::toggled,this,[this](bool value){const QPointer<CommandCenter> self(this);refresh();if(self)emit includeIgnoredChanged(value);});
    connect(m_results,&QListWidget::itemActivated,this,[this]{activate();});connect(m_results,&QListWidget::currentRowChanged,this,[this]{updateActions();});
    for(auto* widget:findChildren<QWidget*>())if(!qobject_cast<QMenu*>(widget))widget->installEventFilter(this);
    if(parent)parent->installEventFilter(this);
    chooseMode("global");applyAppearance();
}
void CommandCenter::present(const QString& mode,bool contentSearch){
    const QPointer<CommandCenter> self(this);
    if(!isVisible())m_returnFocus=QApplication::focusWidget();
    {QSignalBlocker blocker(m_query);m_query->clear();}{QSignalBlocker blocker(m_content);m_content->setChecked(contentSearch);}
    // Opening is one atomic query transition; chooseMode must not publish an
    // intermediate request that can delete or reopen this dialog mid-present.
    {QSignalBlocker blocker(this);chooseMode(modes.contains(mode)?mode:QString("global"));}
    if(!self)return;
    const auto epoch=m_request;
    position();if(!self||epoch!=m_request)return;
    show();if(!self||epoch!=m_request)return;
    raise();if(!self||epoch!=m_request)return;
    m_query->setFocus();if(!self||epoch!=m_request)return;
    refresh();
}
void CommandCenter::position(){if(!parentWidget())return;const auto area=parentWidget()->rect();const bool compact=area.height()<400;const bool files=m_mode=="files"||m_mode=="global";m_workspace->setVisible(!compact&&m_mode=="files");m_content->setVisible(!compact&&files);m_ignored->setVisible(!compact&&files);m_filterMenu->setVisible(compact&&files);m_status->setVisible(!compact);m_results->setMinimumHeight(compact?32:65);const int width=qMax(250,qMin(640,area.width()-48));const int height=qMax(190,qMin(520,int(area.height()*.65)));resize(width,qMin(height,qMax(190,area.height()-24)));move(parentWidget()->mapToGlobal(QPoint((area.width()-width)/2,qMax(12,(area.height()-height)/2))));}
void CommandCenter::setWorkspaces(const QJsonArray& values){const auto selected=m_workspace->currentData().toString();QSignalBlocker blocker(m_workspace);m_workspace->clear();m_workspace->addItem("All workspaces","");for(const auto& value:values){auto entry=value.toObject();m_workspace->addItem(entry["title"].toString(),entry["id"].toString());}m_workspace->setCurrentIndex(qMax(0,m_workspace->findData(selected)));}
void CommandCenter::setIncludeIgnored(bool value){QSignalBlocker blocker(m_ignored);m_ignored->setChecked(value);}
void CommandCenter::chooseMode(const QString& mode){m_mode=mode;for(int i=0;i<m_modes.size();++i){m_modes[i]->setChecked(modes[i]==mode);m_modes[i]->setText(modes[i]==mode?labels[i]:QString());m_modes[i]->setToolButtonStyle(modes[i]==mode?Qt::ToolButtonTextBesideIcon:Qt::ToolButtonIconOnly);}m_query->setPlaceholderText(mode=="files"?"Search files…":"Search anything…");position();refresh();}
void CommandCenter::invalidate(){++m_request;m_results->clear();m_status->setText("Searching…");updateActions();}
void CommandCenter::refresh(){invalidate();m_debounce->stop();if(isVisible())dispatch();}
void CommandCenter::dispatch(){if(!isVisible())return;emit queryRequested(m_request,m_mode,m_query->text(),m_mode=="files"?m_workspace->currentData().toString():QString(),m_content->isChecked(),m_ignored->isChecked());}
bool CommandCenter::setResults(quint64 request,const QJsonArray& items,const QString& status){
    if(request!=m_request||!isVisible())return false;
    const auto selected=m_results->currentItem()?m_results->currentItem()->data(Qt::UserRole).toJsonObject()["id"].toString():QString();
    QSignalBlocker blocker(m_results);m_results->clear();int selectedIndex=-1;
    for(const auto& value:items){if(m_results->count()>=256)break;const auto entry=value.toObject();if(entry["id"].toString().isEmpty())continue;
        const auto title=entry["title"].toString().left(512),subtitle=entry["subtitle"].toString().left(1024);auto* item=new QListWidgetItem(title+(subtitle.isEmpty()?QString():"\n"+subtitle),m_results);item->setData(Qt::UserRole,entry);item->setToolTip(title+"\n"+subtitle);item->setSizeHint(QSize(0,subtitle.isEmpty()?34:49));if(entry["disabled"].toBool())item->setFlags(item->flags()&~Qt::ItemIsEnabled);if(entry["id"].toString()==selected)selectedIndex=m_results->count()-1;
    }
    if(selectedIndex<0)for(int i=0;i<m_results->count();++i)if(m_results->item(i)->flags()&Qt::ItemIsEnabled){selectedIndex=i;break;}
    m_results->setCurrentRow(selectedIndex);m_status->setText(status.isEmpty()?(m_results->count()?QString("%1 results · Enter to open · Tab to change mode").arg(m_results->count()):QString("No results found")):status);updateActions();return true;
}
void CommandCenter::updateActions(){auto* current=m_results->currentItem();const auto item=current?current->data(Qt::UserRole).toJsonObject():QJsonObject();const auto kind=item["kind"].toString();m_rename->setVisible(kind=="agent");m_pin->setVisible(kind=="agent");m_pin->setText(item["pinned"].toBool()?"Unpin":"Pin");m_delete->setVisible(kind=="agent"||kind=="tab"||kind=="terminal");m_delete->setText(kind=="agent"?"Delete":"Close");m_delete->setEnabled(kind!="agent"||!item["busy"].toBool());m_copy->setVisible(kind=="tab");}
void CommandCenter::activate(const QString& action){auto* current=m_results->currentItem();if(!current||!(current->flags()&Qt::ItemIsEnabled))return;const auto item=current->data(Qt::UserRole).toJsonObject();const auto kind=item["kind"].toString();if((action=="rename"||action=="pin")&&kind!="agent")return;if(action=="delete"&&(kind=="agent"?item["busy"].toBool():kind!="tab"&&kind!="terminal"))return;if(action=="copy"&&kind!="tab")return;const auto request=m_request;const QPointer<CommandCenter> self(this);if(action!="pin"&&action!="copy")hide();if(self)emit actionRequested(request,item,action);}
bool CommandCenter::eventFilter(QObject* object,QEvent* event){
    if(object==parentWidget()&&(event->type()==QEvent::Resize||event->type()==QEvent::Move)&&isVisible()){position();return false;}
    if(event->type()!=QEvent::KeyPress||!isVisible())return QDialog::eventFilter(object,event);
    auto* key=static_cast<QKeyEvent*>(event);const auto mods=key->modifiers();
    if(key->key()==Qt::Key_Escape){reject();return true;}if(key->key()==Qt::Key_Tab||key->key()==Qt::Key_Backtab){const int step=(key->key()==Qt::Key_Backtab||mods&Qt::ShiftModifier)?-1:1;chooseMode(modes[(modes.indexOf(m_mode)+step+6)%6]);return true;}
    if(key->key()==Qt::Key_Down||key->key()==Qt::Key_Up){const int step=key->key()==Qt::Key_Down?1:-1;int row=m_results->currentRow();for(int i=0;i<m_results->count();++i){row=(row+step+m_results->count())%m_results->count();if(m_results->item(row)->flags()&Qt::ItemIsEnabled){m_results->setCurrentRow(row);m_results->scrollToItem(m_results->item(row));break;}}return true;}
    if((key->key()==Qt::Key_Return||key->key()==Qt::Key_Enter)&&!(mods&Qt::ShiftModifier)){activate();return true;}
    if(mods==Qt::ControlModifier&&key->key()==Qt::Key_E){activate("rename");return true;}if(mods==(Qt::ControlModifier|Qt::ShiftModifier)&&key->key()==Qt::Key_P){activate("pin");return true;}if(mods==(Qt::ControlModifier|Qt::ShiftModifier)&&key->key()==Qt::Key_D){activate("delete");return true;}
    if(mods==Qt::ControlModifier&&key->key()==Qt::Key_C&&!m_query->hasSelectedText()){activate("copy");return true;}
    if(mods==Qt::ControlModifier&&key->key()==Qt::Key_F){m_content->toggle();return true;}if(mods==Qt::ControlModifier&&key->key()==Qt::Key_G){m_ignored->toggle();return true;}return QDialog::eventFilter(object,event);
}
void CommandCenter::resizeEvent(QResizeEvent* event){QDialog::resizeEvent(event);}
void CommandCenter::hideEvent(QHideEvent* event){m_debounce->stop();QDialog::hideEvent(event);auto focus=m_returnFocus;m_returnFocus.clear();if(focus&&focus->isVisible())focus->setFocus();}
void CommandCenter::applyAppearance(){
    const bool dark=palette().color(QPalette::Window).lightness()<128;
    const auto text=dark?QString("#e8e8ed"):QString("#292a2d");const auto selected=dark?QString("#343943"):QString("#eceef1");
    const auto css=QString("QListWidget { background:transparent; border:0; outline:0; } QListWidget::item { padding:2px 6px; border-radius:6px; color:%1; } QListWidget::item:selected { background:%2; color:%1; } QListWidget::item:hover { background:%2; }").arg(text,selected);
    if(m_results->styleSheet()!=css)m_results->setStyleSheet(css);
    const QColor color=dark?QColor("#bec3cb"):QColor("#666a70");for(int i=0;i<m_modes.size();++i)m_modes[i]->setIcon(icon(icons[i],color));m_filterMenu->setIcon(icon("settings",color));
}
void CommandCenter::changeEvent(QEvent* event){QDialog::changeEvent(event);if((event->type()==QEvent::PaletteChange||event->type()==QEvent::ApplicationPaletteChange)&&m_results)applyAppearance();}
}
