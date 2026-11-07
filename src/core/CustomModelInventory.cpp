#include "CustomModelInventory.h"
namespace ade {
namespace {
bool fail(QString* error,const QString& message){if(error)*error=message;return false;}
bool retire(ProviderConfig& provider,const QString& id,QString* error){
    if(provider.retiredCustomModelIds.contains(id))return true;
    if(provider.retiredCustomModelIds.size()>=256)return fail(error,"Removed custom model history is full. Existing records were preserved; no definitions were removed.");
    provider.retiredCustomModelIds.append(id);return true;
}
}
bool upsertCustomModel(QList<ProviderConfig>& providers,const QString& originalProviderId,
    const QString& originalModelId,const CustomModelConfig& model,QString* error){
    if(error)error->clear();
    if(!model.validate(model.providerInstanceId,error))return false;
    auto next=providers;int target=-1,source=-1,old=-1;
    for(int i=0;i<next.size();++i){if(next[i].id==model.providerInstanceId)target=i;if(next[i].id==originalProviderId)source=i;}
    if(target<0||next[target].kind!="api")return fail(error,"The selected HTTP provider is no longer available.");
    if(!originalModelId.isEmpty()){
        if(source<0)return fail(error,"The original custom model provider is no longer available.");
        for(int i=0;i<next[source].customModels.size();++i)if(next[source].customModels[i].modelId==originalModelId){old=i;break;}
        if(old<0)return fail(error,"The original custom model is no longer available.");
    }else if(!originalProviderId.isEmpty())return fail(error,"Invalid custom model edit identity.");
    int count=0;
    for(int p=0;p<next.size();++p)for(int m=0;m<next[p].customModels.size();++m){
        ++count;if(p==source&&m==old)continue;
        if(next[p].customModels[m].modelId==model.modelId)return fail(error,"A custom model already uses this model ID.");
    }
    if(old<0&&count>=256)return fail(error,"The custom model inventory is full.");
    // Explicitly replacing this same retired identity frees only that marker.
    // Never evict unrelated history to admit another definition.
    next[target].retiredCustomModelIds.removeAll(model.modelId);
    if(old>=0){
        if(source!=target||originalModelId!=model.modelId)if(!retire(next[source],originalModelId,error))return false;
        if(source==target){next[source].customModels[old]=model;providers=std::move(next);return true;}
        next[source].customModels.removeAt(old);
    }
    next[target].customModels.append(model);providers=std::move(next);return true;
}
bool removeCustomModel(QList<ProviderConfig>& providers,const QString& providerId,
    const QString& modelId,QString* error){
    if(error)error->clear();
    auto next=providers;
    for(auto& provider:next)if(provider.id==providerId){
        for(int i=0;i<provider.customModels.size();++i)if(provider.customModels[i].modelId==modelId){
            if(!retire(provider,modelId,error))return false;
            provider.customModels.removeAt(i);providers=std::move(next);return true;
        }
        return fail(error,"The selected custom model is no longer available.");
    }
    return fail(error,"The selected custom model provider is no longer available.");
}
}
