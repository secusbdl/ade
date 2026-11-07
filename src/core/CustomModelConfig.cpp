#include "CustomModelConfig.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <cmath>
namespace ade {
namespace {
#include "../attachments/ModelMediaEntries.inc"
bool fail(QString* error,const QString& message){if(error)*error=message;return false;}
bool identifier(const QString& value,int limit){
    if(value.isEmpty()||value.size()>limit||value!=value.trimmed())return false;
    for(const auto c:value)if(c.unicode()<32||c.unicode()==127)return false;
    return true;
}
bool bounded(const QJsonValue& value,int depth,int& nodes,qint64& characters){
    if(depth>12||++nodes>8192)return false;
    if(value.isString()){characters+=value.toString().size();return characters<=65536;}
    if(value.isDouble())return std::isfinite(value.toDouble());
    if(value.isArray())for(const auto& item:value.toArray())if(!bounded(item,depth+1,nodes,characters))return false;
    if(value.isObject()){const auto object=value.toObject();for(auto it=object.begin();it!=object.end();++it){characters+=it.key().size();if(it.key().size()>512||characters>65536||!bounded(it.value(),depth+1,nodes,characters))return false;}}
    return !value.isUndefined();
}
}
QJsonObject defaultCustomModelCapabilities(){
    const QJsonObject text{{"text",true},{"image",false},{"audio",false},{"video",false},{"file",false}};
    return {{"inputModalities",text},{"outputModalities",text},{"toolCalling",true}};
}
bool validCustomModelCapabilities(const QJsonObject& object,QString* error){
    if(error)error->clear();
    if(object.size()!=3||!object.value("toolCalling").isBool())return fail(error,"Custom model capabilities have an invalid shape.");
    for(const auto& section:{"inputModalities","outputModalities"}){
        if(!object.value(section).isObject())return fail(error,"Custom model modality capabilities must be objects.");
        const auto modalities=object.value(section).toObject();
        if(modalities.size()!=5)return fail(error,"Custom model modality capabilities have an invalid shape.");
        for(const auto& key:{"text","image","audio","video","file"})if(!modalities.value(key).isBool())return fail(error,"Custom model modality values must be boolean.");
    }
    return true;
}
bool isReservedCustomModelId(const QString& id){
    if(id=="default"||id=="quick"||id=="smart")return true;
    for(const auto& entry:Entries)if(id==QLatin1String(entry.id))return true;
    return false;
}
CustomModelConfig::CustomModelConfig():capabilities(defaultCustomModelCapabilities()){}
QJsonObject CustomModelConfig::publicJson()const{
    return {{"modelId",modelId},{"displayName",displayName},{"description",description},
        {"providerInstanceId",providerInstanceId},{"contextWindowSize",contextWindowSize},
        {"thinkingEnabled",thinkingEnabled},{"capabilities",capabilities}};
}
QJsonObject CustomModelConfig::privateJson()const{return {{"providerOptions",providerOptions},{"headers",headers}};}
CustomModelConfig CustomModelConfig::fromPublicJson(const QJsonObject& object,QString* error){
    if(error)error->clear();
    CustomModelConfig model;
    for(const auto& key:{"modelId","displayName","providerInstanceId"})if(!object.value(key).isString()){fail(error,"Custom model identity fields must be strings.");return {};}
    if(object.contains("description")&&!object.value("description").isString()){fail(error,"Custom model description must be text.");return {};}
    if(object.contains("thinkingEnabled")&&!object.value("thinkingEnabled").isBool()){fail(error,"Custom model thinking capability must be boolean.");return {};}
    if(object.contains("contextWindowSize")){const auto context=object.value("contextWindowSize");if(!context.isDouble()||context.toDouble()!=context.toInt()||context.toInt()<=0||context.toInt()>100000000){fail(error,"Custom model context window is outside its supported positive integer range.");return {};}model.contextWindowSize=context.toInt();}
    if(object.contains("capabilities")){if(!object.value("capabilities").isObject()){fail(error,"Custom model capabilities must be an object.");return {};}if(!validCustomModelCapabilities(object.value("capabilities").toObject(),error))return {};model.capabilities=object.value("capabilities").toObject();}
    model.modelId=object.value("modelId").toString();model.displayName=object.value("displayName").toString();model.providerInstanceId=object.value("providerInstanceId").toString();model.description=object.value("description").toString();model.thinkingEnabled=object.value("thinkingEnabled").toBool();
    if(!model.validate(model.providerInstanceId,error))return {};
    return model;
}
bool CustomModelConfig::setPrivateJson(const QJsonObject& object,QString* error){
    if(error)error->clear();
    if(object.size()!=2||!object.value("providerOptions").isObject()||!object.value("headers").isObject())return fail(error,"Custom model private configuration is invalid.");
    auto candidate=*this;candidate.providerOptions=object.value("providerOptions").toObject();candidate.headers=object.value("headers").toObject();
    if(!candidate.validate(candidate.providerInstanceId,error))return false;
    providerOptions=candidate.providerOptions;headers=candidate.headers;return true;
}
bool CustomModelConfig::validate(const QString& expectedProviderId,QString* error)const{
    if(error)error->clear();
    if(!identifier(modelId,512)||!identifier(displayName,512)||!identifier(providerInstanceId,512)||providerInstanceId!=expectedProviderId)return fail(error,"Custom model identity is invalid or belongs to another provider instance.");
    if(isReservedCustomModelId(modelId))return fail(error,"This model ID belongs to a built-in model.");
    if(description.size()>16384||description.contains(QChar::Null))return fail(error,"Custom model description exceeds its supported bound.");
    if(contextWindowSize<=0||contextWindowSize>100000000)return fail(error,"Custom model context window is outside its supported positive integer range.");
    if(!validCustomModelCapabilities(capabilities,error))return false;
    int nodes=0;qint64 characters=0;if(!bounded(providerOptions,0,nodes,characters))return fail(error,"Custom model provider options exceed supported size or nesting limits.");
    if(headers.size()>64)return fail(error,"Custom model headers exceed the supported count.");
    qint64 headerCharacters=0;
    for(auto it=headers.begin();it!=headers.end();++it){if(it.key().isEmpty()||it.key().size()>256||!it.value().isString()||it.value().toString().size()>8192)return fail(error,"Custom model headers must contain bounded string values.");headerCharacters+=it.key().size()+it.value().toString().size();}
    if(headerCharacters>32768)return fail(error,"Custom model headers exceed the supported aggregate size.");
    return true;
}
}
