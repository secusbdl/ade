#pragma once
#include "ModelSelection.h"
namespace ade {
// Fully ordered, task-specific HTTP candidates. Callers must not append a second
// generic chat chain: title and compression have different final-fallback rules.
struct UtilityModelPlan {
    QList<ModelConfig> titleModels;
    QList<ModelConfig> compressionModels;
};
UtilityModelPlan resolveUtilityModelPlan(const ModelSelection& active,
    const QList<ProviderConfig>& providers,const QJsonArray& presets,
    const QJsonObject& globalUtilityLists,const QJsonObject& catalogs={});
QList<ModelSelection> availableUtilityModelSelections(const QList<ProviderConfig>& providers,
    const QJsonObject& catalogs);
// Only for rendering persisted entries; never authorizes a model or endpoint.
QString utilityModelEntryLabel(const QJsonObject& entry,const QList<ProviderConfig>& providers,
    const QJsonObject& catalogs={});
}
