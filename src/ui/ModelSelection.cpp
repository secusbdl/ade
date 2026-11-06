#include "ModelSelection.h"
#include <QJsonDocument>
#include <QSet>
#include <QUrl>

namespace ade {
QJsonObject ModelSelection::toJson() const {
    return {{"providerInstanceId",providerId},{"modelId",modelId},
            {"presetId",presetId},{"label",label},{"overrides",overrides}};
}
ModelSelection ModelSelection::fromJson(const QJsonObject &o) {
    return {o["providerInstanceId"].toString(),o["modelId"].toString(),
        o["presetId"].toString(),o["label"].toString(),o["overrides"].toObject()};
}
QString ModelSelection::key() const {
    return QString::fromUtf8(QJsonDocument(QJsonArray{providerId,modelId,presetId}).toJson(QJsonDocument::Compact));
}
bool ModelSelection::resolve(const QList<ProviderConfig> &providers, ProviderConfig *result) const {
    if (modelId.isEmpty() || !result) return false;
    for (const auto &provider : providers) {
        if (provider.id != providerId) continue;
        *result=provider;result->model=modelId;
        // An explicit provider limit belongs to its default model only.
        if(modelId!=provider.model)result->contextWindowTokens=0;
        if(overrides.contains("reasoningEffort"))result->reasoningEffort=overrides["reasoningEffort"].toString();
        if(overrides.contains("thinkingMode"))result->thinkingMode=overrides["thinkingMode"].toString();
        if(overrides.contains("thinkingBudget"))result->thinkingBudget=overrides["thinkingBudget"].toInt();
        if(overrides.contains("maxOutputTokens"))result->maxOutputTokens=overrides["maxOutputTokens"].toInt();
        return true;
    }
    return false;
}
QJsonObject cleanModelCatalog(const ProviderConfig &provider,const QJsonArray &models) {
    QJsonArray entries;QSet<QString> ids;
    for(const auto &value:models) {
        const auto m=value.toObject();const auto id=m["id"].toString();
        if(id.isEmpty() || id.size()>512 || ids.contains(id))continue;
        ids.insert(id);
        auto limit=[](const QJsonValue& value){return value.isDouble()&&value.toDouble()==value.toInt()&&value.toInt()>0&&value.toInt()<=100000000?value.toInt():0;};
        entries.append(QJsonObject{{"id",id},{"name",m["name"].toString(id).left(512)},
            {"inputTokenLimit",limit(m["inputTokenLimit"])},
            {"outputTokenLimit",limit(m["outputTokenLimit"])}});
        if(entries.size()>=10000)break;
    }
    return {{"baseUrl",provider.baseUrl},{"protocol",provider.protocol},{"models",entries}};
}
QList<ModelSelection> availableModelSelections(const QList<ProviderConfig> &providers,
    const QJsonObject &catalogs,const QJsonArray &presets) {
    QList<ModelSelection> result;QSet<QString> keys;
    auto append=[&](ModelSelection item){if(!item.modelId.isEmpty()&&!keys.contains(item.key())){keys.insert(item.key());result.append(std::move(item));}};
    for(const auto &value:presets) {
        const auto preset=value.toObject();
        const auto entry=preset["models"].isArray()&&!preset["models"].toArray().isEmpty()?preset["models"].toArray().first().toObject():preset;
        ModelSelection selection{entry["providerInstanceId"].toString(),entry["modelId"].toString(),
            preset["id"].toString(),preset["name"].toString(),preset["overrides"].toObject()};
        if(entry.contains("overrides"))selection.overrides=entry["overrides"].toObject();
        ProviderConfig resolved;
        if(!selection.presetId.isEmpty() && selection.resolve(providers,&resolved))append(selection);
    }
    for(const auto &p:providers) {
        append({p.id,p.model,{},p.name+" / "+p.model,{}});
        const auto catalog=catalogs[p.id].toObject();
        // A catalog belongs to one endpoint/protocol. Editing a provider invalidates old discovery.
        if(QUrl(catalog["baseUrl"].toString())!=QUrl(p.baseUrl)||catalog["protocol"].toString()!=p.protocol)continue;
        for(const auto &value:catalog["models"].toArray()) {
            const auto m=value.toObject();const auto id=m["id"].toString();
            append({p.id,id,{},p.name+" / "+m["name"].toString(id),{}});
        }
    }
    return result;
}
QList<ModelConfig> resolveModelEntries(const QList<ProviderConfig>& providers,const QJsonArray& entries,const QJsonObject& catalogs){
    QList<ModelConfig> result;QSet<QString> seen;
    for(const auto& value:entries){const auto item=value.toObject();auto selection=ModelSelection::fromJson(item);ProviderConfig provider;
        if(!selection.resolve(providers,&provider)||seen.contains(selection.key()))continue;
        const auto catalog=catalogs[provider.id].toObject();
        if(provider.contextWindowTokens==0&&QUrl(catalog["baseUrl"].toString())==QUrl(provider.baseUrl)&&catalog["protocol"]==provider.protocol){
            for(const auto& value:catalog["models"].toArray()){
                const auto model=value.toObject();const auto limit=model["inputTokenLimit"];
                if(model["id"]==provider.model&&limit.isDouble()&&limit.toDouble()==limit.toInt()&&limit.toInt()>0&&limit.toInt()<=100000000){provider.contextWindowTokens=limit.toInt();break;}
            }
        }
        seen.insert(selection.key());const auto options=selection.overrides;
        result.append({provider,options.contains("reasoningEffort")||options.contains("thinkingMode")||options.contains("thinkingBudget")});
    }
    return result;
}
QList<ModelConfig> selectedModelChain(const ModelSelection& selection,const QList<ProviderConfig>& providers,const QJsonArray& presets,const QJsonObject& catalogs){
    if(!selection.presetId.isEmpty())for(const auto& value:presets){const auto preset=value.toObject();if(preset["id"].toString()!=selection.presetId)continue;
        const auto entries=preset["models"].isArray()?preset["models"].toArray():QJsonArray{preset};
        const auto resolved=resolveModelEntries(providers,entries,catalogs);if(!resolved.isEmpty())return resolved;
    }
    return resolveModelEntries(providers,{selection.toJson()},catalogs);
}
}
