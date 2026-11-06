#include "ProviderModelsDialog.h"
#include "ModelSelection.h"
#include <QComboBox>
#include <QEvent>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPointer>
#include <QPainter>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QSet>
#include <QVBoxLayout>
namespace ade {
namespace {
constexpr int IdRole=Qt::UserRole+1;constexpr int SearchRole=Qt::UserRole+2;
class VisibilityCombo final:public QComboBox {
protected:
 void paintEvent(QPaintEvent* event)override{
    QComboBox::paintEvent(event);QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);painter.setPen(QPen(palette().color(QPalette::Text),1.2));
    const int x=width()-10,y=height()/2;painter.drawLine(QPointF(x-3,y-1),QPointF(x,y+2));painter.drawLine(QPointF(x,y+2),QPointF(x+3,y-1));
 }
};
}
class ProviderModelsFilter final:public QSortFilterProxyModel {
public:
 using QSortFilterProxyModel::QSortFilterProxyModel;
 void setQuery(QString query){beginFilterChange();m_query=query.trimmed();endFilterChange(Direction::Rows);}
 void setEnabledOnly(bool enabled){beginFilterChange();m_enabledOnly=enabled;endFilterChange(Direction::Rows);}
protected:
 bool filterAcceptsRow(int row,const QModelIndex& parent)const override{
    const auto index=sourceModel()->index(row,0,parent);
    return (!m_enabledOnly||index.data(Qt::CheckStateRole).toInt()==Qt::Checked)
      &&(m_query.isEmpty()||index.data(SearchRole).toString().contains(m_query,Qt::CaseInsensitive));
 }
private:QString m_query;bool m_enabledOnly=false;
};
ProviderModelsDialog::ProviderModelsDialog(QString dataDirectory,QString providerId,QJsonObject catalog,QWidget* parent)
 :QDialog(parent),m_repository(dataDirectory+"/providers.json"),m_id(std::move(providerId)){
 if(parent){setPalette(parent->palette());parent->installEventFilter(this);}
 setObjectName("providerModelsDialog");setWindowTitle("Provider models");resize(620,500);setMinimumSize(320,300);
 auto* layout=new QVBoxLayout(this);layout->setContentsMargins(20,18,20,18);layout->setSpacing(10);
 QString error;m_providers=m_repository.load(&error);for(int i=0;i<m_providers.size();++i)if(m_providers[i].id==m_id){m_provider=i;break;}
 auto* title=new QLabel(m_provider>=0?m_providers[m_provider].name+" models":QStringLiteral("Provider models"));title->setTextFormat(Qt::PlainText);title->setWordWrap(true);layout->addWidget(title);
 auto* help=new QLabel("Choose which models appear when selecting a model. Existing conversations and saved presets keep their selections.");help->setWordWrap(true);layout->addWidget(help);
 auto* tools=new QHBoxLayout;m_search=new QLineEdit;m_search->setObjectName("providerModelSearch");m_search->setPlaceholderText("Search models…");m_search->setMaxLength(256);m_search->setMinimumWidth(0);tools->addWidget(m_search,1);
 m_visibility=new VisibilityCombo;m_visibility->setObjectName("providerModelVisibility");m_visibility->addItem("All models",false);m_visibility->addItem("Enabled only",true);m_visibility->setMinimumWidth(m_visibility->fontMetrics().horizontalAdvance("Enabled only")+30);tools->addWidget(m_visibility);layout->addLayout(tools);
 m_count=new QLabel;m_count->setObjectName("providerModelCount");layout->addWidget(m_count);
 m_model=new QStandardItemModel(this);m_filter=new ProviderModelsFilter(this);m_filter->setSourceModel(m_model);m_filter->setDynamicSortFilter(true);m_filter->setFilterRole(Qt::CheckStateRole);
 m_list=new QListView;m_list->setObjectName("providerModelList");m_list->setModel(m_filter);m_list->setUniformItemSizes(true);m_list->setTextElideMode(Qt::ElideRight);m_list->setMinimumSize(0,70);layout->addWidget(m_list,1);
 m_empty=new QLabel;m_empty->setObjectName("providerModelsEmpty");m_empty->setWordWrap(true);layout->addWidget(m_empty);
 auto* bulk=new QHBoxLayout;m_enableAll=new QPushButton("Enable all");m_enableAll->setObjectName("enableAllProviderModels");m_disableAll=new QPushButton("Disable all");m_disableAll->setObjectName("disableAllProviderModels");bulk->addWidget(m_enableAll);bulk->addWidget(m_disableAll);bulk->addStretch();layout->addLayout(bulk);
 m_status=new QLabel;m_status->setObjectName("providerModelsStatus");m_status->setTextFormat(Qt::PlainText);m_status->setWordWrap(true);layout->addWidget(m_status);
 auto* close=new QDialogButtonBox(QDialogButtonBox::Close);layout->addWidget(close);connect(close,&QDialogButtonBox::rejected,this,&QDialog::reject);
 for(auto* button:findChildren<QPushButton*>()){button->setMinimumHeight(32);button->setMinimumWidth(button->fontMetrics().horizontalAdvance(button->text())+30);}
 if(m_provider>=0){
    const auto& provider=m_providers[m_provider];const auto entries=availableModelSelections({provider},{{provider.id,catalog}},{},true);
    QSet<QString> disabled;for(const auto& id:provider.disabledModelIds)disabled.insert(normalizedModelVisibilityId(provider,id));
    for(const auto& entry:entries){
        const auto label=entry.label.mid(provider.name.size()+3);auto* item=new QStandardItem(label);item->setData(entry.modelId,IdRole);item->setData(entry.modelId+" "+entry.label,SearchRole);item->setToolTip(label==entry.modelId?entry.modelId:label+"\n"+entry.modelId);item->setEditable(false);item->setCheckable(true);item->setCheckState(disabled.contains(normalizedModelVisibilityId(provider,entry.modelId))?Qt::Unchecked:Qt::Checked);item->setSizeHint(QSize(0,34));m_model->appendRow(item);
    }
 }
 connect(m_model,&QStandardItemModel::itemChanged,this,&ProviderModelsDialog::changed);
 connect(m_search,&QLineEdit::textChanged,m_filter,&ProviderModelsFilter::setQuery);
 connect(m_visibility,&QComboBox::currentIndexChanged,this,[this]{m_filter->setEnabledOnly(m_visibility->currentData().toBool());});
 connect(m_enableAll,&QPushButton::clicked,this,[this]{commit({});});
 connect(m_disableAll,&QPushButton::clicked,this,[this]{QStringList ids;for(int row=0;row<m_model->rowCount();++row)ids.append(m_model->item(row)->data(IdRole).toString());commit(ids);});
 connect(m_filter,&QAbstractItemModel::rowsInserted,this,[this]{m_empty->setVisible(m_filter->rowCount()==0);});
 connect(m_filter,&QAbstractItemModel::rowsRemoved,this,[this]{m_empty->setVisible(m_filter->rowCount()==0);});
 if(error.isEmpty()&&m_provider<0)error="This provider is no longer configured. Reopen provider settings.";
 m_status->setText(error);publish();updateColors();
}
QStringList ProviderModelsDialog::disabledModelIds()const{return m_provider>=0?m_providers[m_provider].disabledModelIds:QStringList{};}
QStringList ProviderModelsDialog::visibleModelIds()const{QStringList ids;for(int i=0;i<m_filter->rowCount();++i)ids.append(m_filter->index(i,0).data(IdRole).toString());return ids;}
void ProviderModelsDialog::done(int result){m_closed=true;++m_epoch;QDialog::done(result);}
void ProviderModelsDialog::changed(QStandardItem* item){
 if(m_updating||m_committing||m_closed||m_provider<0||!item)return;
 const auto provider=m_providers[m_provider];const auto id=item->data(IdRole).toString();const bool enabled=item->checkState()==Qt::Checked;
 if(enabled==isModelEnabled(provider,id))return;
 const QPointer<ProviderModelsDialog> self(this);const auto epoch=m_epoch;
 m_updating=true;item->setCheckState(isModelEnabled(provider,id)?Qt::Checked:Qt::Unchecked);
 if(!self)return;
 m_updating=false;if(m_closed||m_epoch!=epoch)return;
 commit(setModelEnabled(provider,id,enabled));
}
void ProviderModelsDialog::commit(QStringList disabled){
 if(m_committing||m_closed||m_provider<0||disabled==m_providers[m_provider].disabledModelIds)return;
 const QPointer<ProviderModelsDialog> self(this);const auto epoch=m_epoch;m_committing=true;
 auto next=m_providers;next[m_provider].disabledModelIds=std::move(disabled);QString error;
 const bool saved=m_repository.save(next,&error);
 if(!self)return;
 if(!saved){m_committing=false;m_status->setText(error);return;}
 m_providers=std::move(next);m_status->clear();
 // Notify the owner of a durable change even if a later view callback closes this dialog.
 emit modelsChanged(m_id);
 if(!self)return;
 if(m_closed||m_epoch!=epoch){m_committing=false;return;}
 publish();if(self)m_committing=false;
}
void ProviderModelsDialog::publish(){
 const QPointer<ProviderModelsDialog> self(this);const auto epoch=m_epoch;m_updating=true;int enabled=0;
 QSet<QString> disabled;
 if(m_provider>=0)for(const auto& id:m_providers[m_provider].disabledModelIds)disabled.insert(normalizedModelVisibilityId(m_providers[m_provider],id));
 const bool blocked=m_model->blockSignals(true);
 for(int row=0;row<m_model->rowCount();++row){
    auto* item=m_model->item(row);const bool active=m_provider>=0&&!disabled.contains(normalizedModelVisibilityId(m_providers[m_provider],item->data(IdRole).toString()));
    if(active)++enabled;
    item->setCheckState(active?Qt::Checked:Qt::Unchecked);
 }
 m_model->blockSignals(blocked);
 if(!blocked&&m_model->rowCount()>0)emit m_model->dataChanged(m_model->index(0,0),m_model->index(m_model->rowCount()-1,0),{Qt::CheckStateRole});
 if(!self)return;
 if(m_closed||m_epoch!=epoch){m_updating=false;return;}
 m_updating=false;const bool valid=m_provider>=0;
 m_count->setText(QString("%1 of %2 models enabled").arg(enabled).arg(m_model->rowCount()));
 m_enableAll->setEnabled(valid&&m_model->rowCount()>0&&enabled<m_model->rowCount());m_disableAll->setEnabled(valid&&enabled>0);m_list->setEnabled(valid);m_search->setEnabled(valid);m_visibility->setEnabled(valid);
 m_empty->setText(m_model->rowCount()?"No models match the current filter.":"Fetch models in provider settings to show available models.");m_empty->setVisible(m_filter->rowCount()==0);
}
void ProviderModelsDialog::updateColors(){
 if(m_coloring)return;
 m_coloring=true;
 const QPointer<ProviderModelsDialog> self(this);
 const bool dark=palette().color(QPalette::Base).lightness()<128;
 const auto base=dark?QStringLiteral("#21242b"):QStringLiteral("#fdfdfd"),foreground=dark?QStringLiteral("#e8e9ec"):QStringLiteral("#292a2d"),border=dark?QStringLiteral("#3b4049"):QStringLiteral("#e4e6e8");
 const auto button=dark?QStringLiteral("#2c3039"):QStringLiteral("#f2f2f2"),hover=dark?QStringLiteral("#373c47"):QStringLiteral("#e8ebef");
 setStyleSheet(QString("QDialog#providerModelsDialog,QDialog#providerModelsDialog QWidget {color:%1;} QDialog#providerModelsDialog {background:%2;} QDialog#providerModelsDialog QLineEdit,QDialog#providerModelsDialog QListView {background:%2;border:1px solid %3;border-radius:5px;padding:5px;} QDialog#providerModelsDialog QListView::indicator:unchecked {border:1px solid #80858d;border-radius:1px;background:transparent;} QDialog#providerModelsDialog QPushButton {background:%4;border:1px solid %3;border-radius:5px;padding:4px 10px;} QDialog#providerModelsDialog QPushButton:hover {background:%5;} QDialog#providerModelsDialog QPushButton:disabled {color:#80858d;} QDialog#providerModelsDialog QComboBox {padding-right:20px;} QDialog#providerModelsDialog QComboBox QAbstractItemView {background:%2;border:1px solid %3;} QLabel#providerModelsStatus {color:%1;}").arg(foreground,base,border,button,hover));
 if(self)m_coloring=false;
}
void ProviderModelsDialog::changeEvent(QEvent* event){const QPointer<ProviderModelsDialog> self(this);QDialog::changeEvent(event);if(self&&event->type()==QEvent::PaletteChange&&!m_coloring)updateColors();}
bool ProviderModelsDialog::eventFilter(QObject* watched,QEvent* event){
 const QPointer<ProviderModelsDialog> self(this);const QPointer<QObject> receiver(watched);
 if(watched==parentWidget()&&event->type()==QEvent::PaletteChange){setPalette(parentWidget()->palette());if(!self)return !receiver;updateColors();}
 return self?QDialog::eventFilter(watched,event):!receiver;
}
}
