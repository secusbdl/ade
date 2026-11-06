#pragma once
#include "ProviderConfig.h"

namespace ade {
// UI inventory only. Never authorizes a request or changes saved selections.
QJsonObject modelDiscoveryIdentity(const ProviderConfig& provider);
bool modelDiscoveryMatches(const ProviderConfig& provider, const QJsonObject& snapshot);
bool validModelDiscoverySnapshot(const ProviderConfig& provider, const QJsonObject& snapshot,
                                QString* error=nullptr);
QJsonObject makeModelDiscoverySnapshot(const ProviderConfig& provider, const QJsonArray& models,
                                      bool complete, QString* error=nullptr);
void invalidateModelDiscovery(ProviderConfig& provider);
bool flagshipFilteringEnabled(const QString& sourceType);
QStringList disabledModelsAfterDiscovery(const QString& sourceType, const QJsonArray& models,
    const QStringList& disabled, const QJsonArray& previousModels);
}
