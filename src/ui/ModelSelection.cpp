#include "ModelSelection.h"
#include "core/ModelDiscoveryPolicy.h"
#include <QJsonDocument>
#include <QSet>
#include <QUrl>

namespace ade {
namespace {
bool catalogMatches(const ProviderConfig& provider,const QJsonObject& catalog) {
    return QUrl(catalog["baseUrl"].toString())==QUrl(provider.baseUrl)
        && catalog["protocol"].toString()==provider.protocol
        && catalog["kind"].toString("api")==provider.kind
        && catalog["harnessExecutable"].toString()==provider.harnessExecutable;
}
}
QString normalizedModelVisibilityId(const ProviderConfig& provider,const QString& id){
    return provider.templateId=="deepseek-api"&&id.compare("deepseek-flash",Qt::CaseInsensitive)==0?QStringLiteral("deepseek-v4.1-flash"):id;
}
bool isModelEnabled(const ProviderConfig& provider,const QString& modelId){
    const auto key=normalizedModelVisibilityId(provider,modelId);
    for(const auto& id:provider.disabledModelIds)if(normalizedModelVisibilityId(provider,id)==key)return false;
    return true;
}
QStringList setModelEnabled(const ProviderConfig& provider,const QString& modelId,bool enabled){
    const auto key=normalizedModelVisibilityId(provider,modelId);QStringList result;
    for(const auto& id:provider.disabledModelIds)if(normalizedModelVisibilityId(provider,id)!=key)result.append(id);
    if(!enabled&&!modelId.isEmpty())result.append(modelId);
    return result;
}
QJsonArray enabledDiscoveredModels(const ProviderConfig& provider,const QJsonArray& models){
    QSet<QString> disabled;for(const auto& id:provider.disabledModelIds)disabled.insert(normalizedModelVisibilityId(provider,id));
    QJsonArray result;for(const auto& value:models){const auto id=value.toObject().value("id").toString();if(!id.isEmpty()&&!disabled.contains(normalizedModelVisibilityId(provider,id)))result.append(value);}return result;
}
QString discoveredDefaultModel(const ProviderConfig& provider,const QJsonArray& models,const QString& current){
    // Discovery must not silently replace an existing choice, even if it is hidden.
    for(const auto& value:models)if(value.toObject().value("id").toString()==current)return current;
    QString selected;for(const auto& value:enabledDiscoveredModels(provider,models)){const auto model=value.toObject();if(selected.isEmpty()||model.value("isDefault").toBool())selected=model.value("id").toString();}
    return selected.isEmpty()?current:selected;
}
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
        const CustomModelConfig* custom=nullptr;
        for(const auto& definition:provider.customModels)if(definition.modelId==modelId&&definition.providerInstanceId==providerId){custom=&definition;break;}
        if(!custom&&provider.retiredCustomModelIds.contains(modelId)){
            bool discovered=false;const auto catalog=effectiveModelCatalog(provider);
            for(const auto& value:catalog.value("models").toArray())if(value.toObject().value("id").toString()==modelId){discovered=true;break;}
            if(!discovered)return false;
        }
        if(custom){QString error;if(provider.kind!="api"||!custom->validate(provider.id,&error))return false;}
        *result=provider;result->model=modelId;result->customRequest={};result->requestOverrides={};
        // A custom definition supplies independent model settings, not the
        // previously selected provider-default model's reasoning or limit.
        if(custom){
            result->contextWindowTokens=custom->contextWindowSize;result->reasoningEffort.clear();result->thinkingMode.clear();result->thinkingBudget=0;result->maxOutputTokens=0;
            result->customRequest={{"providerInstanceId",provider.id},{"modelId",modelId},{"providerOptions",custom->providerOptions},{"headers",custom->headers},{"capabilities",custom->capabilities}};
        }else if(modelId!=provider.model)result->contextWindowTokens=0;
        for(const auto& key:{"reasoningEffort","thinkingMode","thinkingBudget","maxOutputTokens"})if(overrides.contains(key))result->requestOverrides[key]=overrides.value(key);
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
        QJsonObject entry{{"id",id},{"name",m["name"].toString(id).left(512)},
            {"inputTokenLimit",limit(m["inputTokenLimit"])},
            {"outputTokenLimit",limit(m["outputTokenLimit"])}};
        if(provider.isExternalHarness()) {
            QJsonArray efforts,modalities;
            for(const auto& value:m["reasoningEfforts"].toArray())if(value.isString()&&value.toString().size()<=32&&efforts.size()<20)efforts.append(value);
            for(const auto& value:m["inputModalities"].toArray())if(value.isString()&&value.toString().size()<=32&&modalities.size()<10)modalities.append(value);
            entry["reasoningEfforts"]=efforts;entry["inputModalities"]=modalities;
            entry["defaultReasoningEffort"]=m["defaultReasoningEffort"].toString().left(32);
            entry["isDefault"]=m["isDefault"].toBool();
        }
        entries.append(entry);
        if(entries.size()>=10000)break;
    }
    return {{"baseUrl",provider.baseUrl},{"protocol",provider.protocol},{"kind",provider.kind},
            {"harnessExecutable",provider.harnessExecutable},{"models",entries}};
}
QJsonObject effectiveModelCatalog(const ProviderConfig& provider,const QJsonObject& legacyCatalog) {
    if(provider.discoverySnapshot.isEmpty())return catalogMatches(provider,legacyCatalog)?legacyCatalog:QJsonObject{};
    if(!modelDiscoveryMatches(provider,provider.discoverySnapshot)||!provider.discoverySnapshot.value("complete").toBool()
       ||!validModelDiscoverySnapshot(provider,provider.discoverySnapshot))return {};
    auto catalog=provider.discoverySnapshot.value("identity").toObject();
    catalog["models"]=provider.discoverySnapshot.value("models");return catalog;
}
void prepareModelDiscoveryForSave(ProviderConfig& provider,const QJsonObject& legacyCatalog) {
    invalidateModelDiscovery(provider);
    if(!provider.discoverySnapshot.isEmpty()||!catalogMatches(provider,legacyCatalog)||!legacyCatalog.value("models").isArray())return;
    const auto models=legacyCatalog.value("models").toArray();
    if(models.size()>10000)return;
    const auto cleaned=cleanModelCatalog(provider,models).value("models").toArray();
    if(cleaned.size()!=models.size())return;
    provider.discoverySnapshot=makeModelDiscoverySnapshot(provider,cleaned,true);
}
QList<ModelSelection> availableModelSelections(const QList<ProviderConfig> &providers,
    const QJsonObject &catalogs,const QJsonArray &presets,bool includeDisabled) {
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
        QSet<QString> disabled;
        if(!includeDisabled)for(const auto& id:p.disabledModelIds)disabled.insert(normalizedModelVisibilityId(p,id));
        const auto enabled=[&](const QString& id){return !disabled.contains(normalizedModelVisibilityId(p,id));};
        QSet<QString> customIds;for(const auto& custom:p.customModels)customIds.insert(custom.modelId);
        ModelSelection defaultSelection{p.id,p.model,{},p.name+" / "+p.model,{}};ProviderConfig resolvedDefault;
        if(!customIds.contains(p.model)&&(includeDisabled||enabled(p.model))&&defaultSelection.resolve({p},&resolvedDefault))append(defaultSelection);
        const auto catalog=effectiveModelCatalog(p,catalogs[p.id].toObject());
        // A catalog belongs to one endpoint/protocol. Editing a provider invalidates old discovery.
        if(catalogMatches(p,catalog))for(const auto &value:catalog["models"].toArray()) {
            const auto m=value.toObject();const auto id=m["id"].toString();
            if(!customIds.contains(id)&&(includeDisabled||enabled(id)))append({p.id,id,{},p.name+" / "+m["name"].toString(id),{}});
        }
        for(const auto& custom:p.customModels)if(custom.providerInstanceId==p.id&&(includeDisabled||enabled(custom.modelId)))append({p.id,custom.modelId,{},p.name+" / "+custom.displayName,{}});
    }
    return result;
}
QList<ModelConfig> resolveModelEntries(const QList<ProviderConfig>& providers,const QJsonArray& entries,const QJsonObject& catalogs){
    QList<ModelConfig> result;QSet<QString> seen;
    for(const auto& value:entries){const auto item=value.toObject();auto selection=ModelSelection::fromJson(item);ProviderConfig provider;
        if(!selection.resolve(providers,&provider)||seen.contains(selection.key()))continue;
        const auto catalog=effectiveModelCatalog(provider,catalogs[provider.id].toObject());
        if(provider.contextWindowTokens==0&&catalogMatches(provider,catalog)){
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
