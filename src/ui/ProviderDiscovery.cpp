#include "ProviderDiscovery.h"
#include "ModelSelection.h"
#include "ProviderCatalog.h"
#include "core/ModelDiscoveryPolicy.h"
namespace ade {
bool stageModelDiscovery(ProviderConfig& provider,const QJsonArray& models,const QJsonObject& legacyCatalog,QString* error) {
    if(error)error->clear();
    if(models.size()>10000){if(error)*error="Model discovery exceeded its entry limit; previous choices were preserved.";return false;}
    auto candidate=provider;ProviderCatalog::clearChangedProvenance(candidate);
    prepareModelDiscoveryForSave(candidate,legacyCatalog);
    const auto cleaned=cleanModelCatalog(candidate,models).value("models").toArray();
    if(cleaned.size()!=models.size()){if(error)*error="Model discovery returned invalid or duplicate identifiers; previous choices were preserved.";return false;}
    const auto snapshot=makeModelDiscoverySnapshot(candidate,cleaned,true,error);
    if(snapshot.isEmpty())return false;
    const auto* source=ProviderCatalog::find(candidate.templateId);
    if(source&&ProviderCatalog::matches(candidate,*source))candidate.disabledModelIds=disabledModelsAfterDiscovery(candidate.templateId,cleaned,candidate.disabledModelIds,effectiveModelCatalog(candidate,legacyCatalog).value("models").toArray());
    candidate.discoverySnapshot=snapshot;provider=std::move(candidate);return true;
}
}
