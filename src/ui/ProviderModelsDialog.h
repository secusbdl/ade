#pragma once
#include "core/ProviderConfig.h"
#include "ProviderRepository.h"
#include <QDialog>
#include <QJsonObject>
class QLineEdit;class QComboBox;class QLabel;class QListView;class QStandardItemModel;class QPushButton;class QStandardItem;
namespace ade {
class ProviderModelsFilter;
// Each edit is committed through the same encrypted repository CAS as provider setup.
// Hidden models are omitted from new choices; saved selections remain resolvable.
class ProviderModelsDialog final:public QDialog {
 Q_OBJECT
public:
 ProviderModelsDialog(QString dataDirectory,QString providerId,QJsonObject catalog,QWidget* parent=nullptr);
 QString providerId()const{return m_id;}
 QStringList disabledModelIds()const;
 QStringList visibleModelIds()const;
signals:
 void modelsChanged(QString providerId);
protected:
 void done(int result)override;
 void changeEvent(QEvent* event)override;
 bool eventFilter(QObject* watched,QEvent* event)override;
private:
 void changed(QStandardItem* item);
 void commit(QStringList disabled);
 void publish();
 void updateColors();
 bool rebuildRows();
 void updateCustomActions();
 void editCustom(const QString& id={});
 void deleteCustom();
 bool commitCustom(const QString& originalProvider,const QString& originalId,const CustomModelConfig&,QString* error);
 ProviderRepository m_repository;
 QList<ProviderConfig> m_providers;
 QString m_id;
 int m_provider=-1;
 bool m_updating=false,m_committing=false,m_closed=false,m_coloring=false,m_editing=false;
 QJsonObject m_catalog;
 quint64 m_epoch=0;
 QStandardItemModel* m_model=nullptr;
 ProviderModelsFilter* m_filter=nullptr;
 QListView* m_list=nullptr;
 QLineEdit* m_search=nullptr;
 QComboBox* m_visibility=nullptr;
 QLabel *m_count=nullptr,*m_status=nullptr,*m_empty=nullptr;
 QPushButton *m_enableAll=nullptr,*m_disableAll=nullptr;
 QPushButton *m_addCustom=nullptr,*m_editCustom=nullptr,*m_deleteCustom=nullptr;
};
}
