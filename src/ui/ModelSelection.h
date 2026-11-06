#pragma once
#include "core/ProviderConfig.h"
#include "core/ModelConfig.h"
#include <QJsonArray>
#include <QList>

namespace ade {
// A selection refers to a provider instance, never just a model's display name.
struct ModelSelection {
    QString providerId, modelId, presetId, label;
    QJsonObject overrides;
    QJsonObject toJson() const;
    static ModelSelection fromJson(const QJsonObject &object);
    QString key() const;
    bool resolve(const QList<ProviderConfig> &providers, ProviderConfig *result) const;
};
QList<ModelSelection> availableModelSelections(const QList<ProviderConfig> &providers,
    const QJsonObject &catalogs, const QJsonArray &presets, bool includeDisabled=false);
QString normalizedModelVisibilityId(const ProviderConfig& provider,const QString& modelId);
bool isModelEnabled(const ProviderConfig& provider,const QString& modelId);
QStringList setModelEnabled(const ProviderConfig& provider,const QString& modelId,bool enabled);
QJsonArray enabledDiscoveredModels(const ProviderConfig& provider,const QJsonArray& models);
QString discoveredDefaultModel(const ProviderConfig& provider,const QJsonArray& models,const QString& current);
QJsonObject cleanModelCatalog(const ProviderConfig &provider, const QJsonArray &models);
QList<ModelConfig> resolveModelEntries(const QList<ProviderConfig>& providers,const QJsonArray& entries,const QJsonObject& catalogs={});
QList<ModelConfig> selectedModelChain(const ModelSelection& selection,const QList<ProviderConfig>& providers,const QJsonArray& presets,const QJsonObject& catalogs={});
}
