#include "UtilityModelSelection.h"
#include <QJsonDocument>
#include <QSet>
namespace ade {
namespace {
bool eligible(const QJsonObject& entry,const QList<ProviderConfig>& providers) {
    const auto id=entry.value("providerInstanceId").toString();
    for(const auto& provider:providers)if(provider.id==id)return !provider.isExternalHarness();
    // Source selection considers a missing instance eligible. Resolution below
    // skips it; its presence must not switch an intentional preset to globals.
    return true;
}
QJsonArray filtered(const QJsonArray& entries,const QList<ProviderConfig>& providers) {
    QJsonArray result;
    for(const auto& value:entries)if(value.isObject()&&eligible(value.toObject(),providers))result.append(value);
    return result;
}
QList<ModelConfig> resolved(const QJsonArray& entries,const QList<ProviderConfig>& providers,const QJsonObject& catalogs) {
    QJsonArray unique;QSet<QString> seen;
    for(const auto& value:entries){
        auto entry=value.toObject();
        if(!eligible(entry,providers))continue;
        const auto key=QString::fromUtf8(QJsonDocument(QJsonArray{entry.value("providerInstanceId"),entry.value("modelId")}).toJson(QJsonDocument::Compact));
        if(seen.contains(key))continue;
        seen.insert(key);entry.remove("presetId");unique.append(entry);
    }
    return resolveModelEntries(providers,unique,catalogs);
}
}
UtilityModelPlan resolveUtilityModelPlan(const ModelSelection& active,
    const QList<ProviderConfig>& providers,const QJsonArray& presets,
    const QJsonObject& globals,const QJsonObject& catalogs) {
    QJsonObject preset;
    if(!active.presetId.isEmpty())for(const auto& value:presets)
        if(value.toObject().value("id").toString()==active.presetId){preset=value.toObject();break;}
    // Older native presets stored one primary entry at the top level.
    const auto presetModels=preset.isEmpty()?QJsonArray{}:
        (preset.value("models").isArray()?preset.value("models").toArray():QJsonArray{preset});
    const auto eligiblePreset=filtered(presetModels,providers);
    auto chain=[&](const QString& task,bool compression){
        QJsonArray configured=filtered(globals.value(task).toArray(),providers);
        if(!preset.isEmpty()){
            const auto override=filtered(preset.value(task).toArray(),providers);
            if(!override.isEmpty())configured=override;
            else if(!eligiblePreset.isEmpty())configured={};
        }
        QJsonArray entries=configured;
        // Source [] uses the currently active model before preset models, even
        // when failover selected a later entry. Utility overrides are not inferred.
        const QJsonObject current{{"providerInstanceId",active.providerId},{"modelId",active.modelId}};
        if(configured.isEmpty()&&!active.modelId.isEmpty())entries.append(current);
        for(const auto& entry:presetModels)entries.append(entry);
        // Compression has an explicit last-resort active-model attempt. Titles
        // with a nonempty configured list and no preset do not append it.
        if(compression&&!active.modelId.isEmpty())entries.append(current);
        return resolved(entries,providers,catalogs);
    };
    return {chain("titleGeneration",false),chain("contextCompression",true)};
}
QList<ModelSelection> availableUtilityModelSelections(const QList<ProviderConfig>& providers,const QJsonObject& catalogs) {
    QList<ProviderConfig> eligibleProviders;
    for(const auto& provider:providers)if(!provider.isExternalHarness())eligibleProviders.append(provider);
    return availableModelSelections(eligibleProviders,catalogs,{});
}
QString utilityModelEntryLabel(const QJsonObject& entry,const QList<ProviderConfig>& providers,const QJsonObject& catalogs) {
    const auto selection=ModelSelection::fromJson(entry);ProviderConfig provider;
    if(!selection.resolve(providers,&provider))return selection.providerId+" / "+selection.modelId+" (unavailable)";
    QString label=provider.name+" / "+selection.modelId;
    for(const auto& choice:availableModelSelections(providers,catalogs,{},true))
        if(choice.providerId==selection.providerId&&choice.modelId==selection.modelId){label=choice.label;break;}
    if(provider.isExternalHarness())return label+" (not supported for utility requests)";
    if(!isModelEnabled(provider,selection.modelId))return label+" (disabled in model picker)";
    return label;
}
}
