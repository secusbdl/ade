#pragma once
#include "ModelSelection.h"
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>

namespace ade {
// Native cycle_model_thinking_effort support. Only efforts discovered from an
// external coding-agent harness are cycled; the upstream API-provider thinking
// capability table is not ported, so HTTP providers have no options.
// Defined in ModelSelection.cpp, which several targets compile standalone.
QStringList thinkingOptions(const ProviderConfig& provider,const QJsonObject& catalogEntry);
// The discovered catalog entry for a provider's model, or an empty object.
QJsonObject thinkingCatalogEntry(const ProviderConfig& provider,const QString& modelId,const QJsonObject& catalogs);
// Next option after current, wrapping. An unknown current value starts from
// fallbackDefault; when that is unknown too, the first option is chosen.
QString nextThinkingOption(const QStringList& options,const QString& current,const QString& fallbackDefault);
// Structural validation of agent.modelThinkingOverrides:
// {instanceId:{modelId:{enabled:bool,provider:string,value:string<=32}}},
// bounded to 256 instances x 1024 models. Malformed entries are dropped.
QJsonObject validThinkingOverrides(const QJsonValue& value);
// The override value that would be applied to a non-preset selection: enabled,
// structurally valid and still one of the model's discovered options.
QString thinkingOverrideValue(const ModelSelection& selection,const QList<ProviderConfig>& providers,
    const QJsonObject& catalogs,const QJsonObject& overrides);
}
