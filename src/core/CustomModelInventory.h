#pragma once
#include "ProviderConfig.h"
namespace ade {
// Atomic candidate edits. The owning repository still performs revision CAS.
bool upsertCustomModel(QList<ProviderConfig>& providers,const QString& originalProviderId,
    const QString& originalModelId,const CustomModelConfig& model,QString* error=nullptr);
bool removeCustomModel(QList<ProviderConfig>& providers,const QString& providerId,
    const QString& modelId,QString* error=nullptr);
}
