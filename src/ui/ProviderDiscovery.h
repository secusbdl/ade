#pragma once
#include "core/ProviderConfig.h"
namespace ade {
// Stages only a complete successful result; no store writes or provider requests.
bool stageModelDiscovery(ProviderConfig& provider,const QJsonArray& models,
    const QJsonObject& legacyCatalog={},QString* error=nullptr);
}
