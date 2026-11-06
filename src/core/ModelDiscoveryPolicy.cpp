#include "ModelDiscoveryPolicy.h"
#include "ModelDiscoveryPolicyData.h"
#include <QJsonDocument>
#include <QSet>
#include <QUrl>

namespace ade {
namespace {
bool text(const QJsonValue& value, int maximum, bool empty=true) {
    if (!value.isString() || value.toString().size()>maximum || (!empty&&value.toString().isEmpty())) return false;
    const auto string=value.toString();
    for(qsizetype i=0;i<string.size();++i){
        const auto c=string[i];
        if(c.unicode()<32||c.unicode()==127||c.isLowSurrogate())return false;
        if(c.isHighSurrogate()&&(++i>=string.size()||!string[i].isLowSurrogate()))return false;
    }
    return true;
}
bool tokenLimit(const QJsonValue& value) {
    return value.isDouble()&&value.toDouble()==value.toInt()&&value.toInt()>=0&&value.toInt()<=100000000;
}
QString matchId(const QString& type,QString id) {
    if(type=="deepseek-api"&&id.compare("deepseek-flash",Qt::CaseInsensitive)==0) id="deepseek-v4.1-flash";
    return id.toLower();
}
QSet<QString> strings(const QJsonArray& array) {
    QSet<QString> result;for(const auto& value:array)result.insert(value.toString());return result;
}
const QJsonObject& policy() {
    static const auto data=QJsonDocument::fromJson(QByteArray(discovery_data::json)).object();return data;
}
}
QJsonObject modelDiscoveryIdentity(const ProviderConfig& provider) {
    return {{"providerId",provider.id},{"baseUrl",QUrl(provider.baseUrl).toString()},
        {"protocol",provider.protocol},{"kind",provider.kind},{"harnessExecutable",provider.harnessExecutable},
        {"templateId",provider.templateId}};
}
bool modelDiscoveryMatches(const ProviderConfig& provider,const QJsonObject& snapshot) {
    return snapshot.value("identity").toObject()==modelDiscoveryIdentity(provider);
}
bool validModelDiscoverySnapshot(const ProviderConfig& provider,const QJsonObject& snapshot,QString* error) {
    if(error)error->clear();
    if(snapshot.isEmpty())return true; // Absent in legacy records.
    const auto fail=[&]{if(error)*error="Invalid provider discovery snapshot. Original data was preserved.";return false;};
    if(snapshot.size()!=4||snapshot.value("version").toDouble()!=1||!snapshot.value("complete").isBool()
       ||!snapshot.value("identity").isObject()||!snapshot.value("models").isArray()
       ||!modelDiscoveryMatches(provider,snapshot))return fail();
    const auto models=snapshot.value("models").toArray();
    if(models.size()>10000||(!snapshot.value("complete").toBool()&&!models.isEmpty()))return fail();
    QSet<QString> ids;
    for(const auto& value:models){
        if(!value.isObject())return fail();
        const auto model=value.toObject();
        if(!text(model.value("id"),512,false)||!text(model.value("name"),512)
           ||!tokenLimit(model.value("inputTokenLimit"))||!tokenLimit(model.value("outputTokenLimit")))return fail();
        const auto id=model.value("id").toString();if(ids.contains(id))return fail();ids.insert(id);
        for(auto it=model.begin();it!=model.end();++it){
            if(it.key()=="id"||it.key()=="name"||it.key()=="inputTokenLimit"||it.key()=="outputTokenLimit")continue;
            if(it.key()=="isDefault"){if(!it.value().isBool())return fail();continue;}
            if(it.key()=="defaultReasoningEffort"){if(!text(it.value(),32))return fail();continue;}
            if(it.key()=="reasoningEfforts"||it.key()=="inputModalities"){
                if(!it.value().isArray()||it.value().toArray().size()>(it.key()=="reasoningEfforts"?20:10))return fail();
                for(const auto& item:it.value().toArray())if(!text(item,32))return fail();
                continue;
            }
            return fail();
        }
    }
    return true;
}
QJsonObject makeModelDiscoverySnapshot(const ProviderConfig& provider,const QJsonArray& models,bool complete,QString* error) {
    QJsonObject snapshot{{"version",1},{"identity",modelDiscoveryIdentity(provider)},{"complete",complete},{"models",models}};
    if(!validModelDiscoverySnapshot(provider,snapshot,error))return {};
    return snapshot;
}
void invalidateModelDiscovery(ProviderConfig& provider) {
    if(!provider.discoverySnapshot.isEmpty()&&!modelDiscoveryMatches(provider,provider.discoverySnapshot))
        provider.discoverySnapshot=makeModelDiscoverySnapshot(provider,{},false);
}
bool flagshipFilteringEnabled(const QString& type) {
    return type=="openrouter"||(type.endsWith("-api")&&type!="anthropic-api");
}
QStringList disabledModelsAfterDiscovery(const QString& type,const QJsonArray& models,
    const QStringList& disabled,const QJsonArray& previousModels) {
    if(!flagshipFilteringEnabled(type))return disabled;
    const auto vendor=type.endsWith("-api")?type.left(type.size()-4):QString();
    const auto catalog=type=="openrouter"?QSet<QString>{}:strings(policy().value("catalog").toObject().value(vendor).toArray());
    const auto flagships=strings(type=="openrouter"?policy().value("openrouter").toArray():policy().value("vendorFlagships").toObject().value(vendor).toArray());
    QSet<QString> current,previous,stored;
    for(const auto& value:models)current.insert(matchId(type,value.toObject().value("id").toString()));
    for(const auto& value:previousModels)previous.insert(matchId(type,value.toObject().value("id").toString()));
    QStringList result;
    for(const auto& id:disabled)if(!stored.contains(id)){stored.insert(id);result.append(id);}
    for(const auto& value:models){
        const auto id=value.toObject().value("id").toString(),key=matchId(type,id);
        if(!catalog.contains(key)&&!flagships.contains(key)&&!previous.contains(key)&&!stored.contains(id)){
            stored.insert(id);result.append(id);
        }
    }
    result.removeIf([&](const QString& id){const auto key=matchId(type,id);return !catalog.contains(key)&&!current.contains(key);});
    return result;
}
}
