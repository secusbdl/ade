#include "FileReadSnapshotStore.h"
#include "AttachmentStore.h"
#include "core/ScopedDirectory.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutex>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace ade {
namespace {
constexpr qsizetype MaxFileBytes=4*1024*1024,MaxManifestBytes=8*1024*1024;
bool fail(QString* error,const QString& message){if(error)*error=message;return false;}
QString digest(const QByteArray& data){return QString::fromLatin1(QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex());}
bool pathValid(const QString& path){
    const auto parts=path.split('/');
    if(path.size()>4096||path.contains('\\')||path.contains(':')||path.contains(QChar::Null)||parts.contains("")||parts.contains(".")||parts.contains(".."))return false;
    if(parts.first()=="att"){
        if(parts.size()==1)return true;
        const auto name=parts.last();
        return parts.size()==2&&name.size()<=180&&QRegularExpression("^[A-Za-z0-9_.-]+$").match(name).hasMatch()
            &&!name.endsWith('.')&&!QRegularExpression("^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$",QRegularExpression::CaseInsensitiveOption).match(name).hasMatch();
    }
    return parts.first()=="ws1"||parts.first()=="apps";
}
bool keys(const QJsonObject& object,const QStringList& accepted){for(auto i=object.begin();i!=object.end();++i)if(!accepted.contains(i.key()))return false;return true;}
bool paramsFromJson(const QString& tool,const QJsonObject& json,FileReadParams* result,QString* error){
    if(!keys(json,{"startLine","endLine","startPage","endPage","preview","depth"}))return fail(error,"Unknown captured read parameter.");
    QJsonObject args;
    for(const auto& pair:QList<QPair<QString,QString>>{{"startLine","start_line"},{"endLine","end_line"},{"startPage","start_page"},{"endPage","end_page"},{"preview","preview"},{"depth","depth"}})
        if(json.contains(pair.first))args[pair.second]=json[pair.first];
    if(tool=="ls"){
        if(json.contains("startLine")||json.contains("endLine")||json.contains("startPage")||json.contains("endPage")||json.contains("preview"))return fail(error,"Invalid listing parameters.");
        return FileTransformer::listParams(args,result,error);
    }
    if(tool!="read"||json.contains("depth"))return fail(error,"Invalid captured read tool or depth.");
    return FileTransformer::readParams(args,result,error);
}
AttachmentRef storageRef(const QString& path,bool directory){
    // Domain-separate manifests from ordinary raw snapshots; no mount lookup occurs.
    return {"reads/"+digest((directory?QByteArray("directory\0",10):QByteArray("file\0",5))+path.toUtf8()),{}};
}
bool encodeEntries(const QList<FileTreeEntry>& entries,double depth,int level,int* count,QJsonArray* out,QString* error){
    auto sorted=entries;std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.name<b.name;});
    QSet<QString> names;
    for(const auto& entry:sorted){
        if(++*count>10000||entry.name.isEmpty()||entry.name.size()>4096||entry.name=="."||entry.name==".."||entry.name.contains('/')||entry.name.contains('\\')||entry.name.contains(QChar::Null)||names.contains(entry.name))return fail(error,"Invalid or oversized directory manifest.");
        names.insert(entry.name);
        if(!entry.metadata.isEmpty())return fail(error,"Unknown captured directory entry metadata.");
        if(!entry.children.isEmpty()&&(!entry.isDirectory||level>=depth))return fail(error,"Directory manifest exceeds its captured depth.");
        QJsonArray children;if(!encodeEntries(entry.children,depth,level+1,count,&children,error))return false;
        out->append(QJsonObject{{"name",entry.name},{"directory",entry.isDirectory},{"children",children}});
    }
    return true;
}
bool decodeEntries(const QJsonArray& entries,double depth,int level,int* count,QList<FileTreeEntry>* out,QString* error){
    QSet<QString> names;
    for(const auto& value:entries){
        if(!value.isObject())return fail(error,"Malformed directory snapshot entry.");
        const auto item=value.toObject();
        if(!keys(item,{"name","directory","children"})||!item["name"].isString()||!item["directory"].isBool()||!item["children"].isArray())return fail(error,"Malformed directory snapshot entry fields.");
        FileTreeEntry entry;entry.name=item["name"].toString();entry.isDirectory=item["directory"].toBool();
        if(++*count>10000||entry.name.isEmpty()||entry.name.size()>4096||entry.name=="."||entry.name==".."||entry.name.contains('/')||entry.name.contains('\\')||entry.name.contains(QChar::Null)||names.contains(entry.name))return fail(error,"Invalid directory snapshot name or bound.");
        names.insert(entry.name);const auto children=item["children"].toArray();
        if(!children.isEmpty()&&(!entry.isDirectory||level>=depth))return fail(error,"Directory snapshot exceeds its captured depth.");
        if(!decodeEntries(children,depth,level+1,count,&entry.children,error))return false;
        out->append(entry);
    }
    return true;
}
bool validate(const FileReadCapture& capture,QByteArray* payload,QString* error){
    if(!capture.inputHash.isEmpty()&&!QRegularExpression("^[0-9a-f]{64}$").match(capture.inputHash).hasMatch())return fail(error,"Invalid captured tool input hash.");
    if(!pathValid(capture.descriptor.mountedPath)||capture.descriptor.originalFileName.size()>1024||capture.descriptor.originalFileName.contains(QChar::Null))return fail(error,"Invalid captured virtual file descriptor.");
    if((capture.descriptor.mountedPath=="att"||capture.descriptor.mountedPath.startsWith("att/"))
        &&((capture.descriptor.mountedPath=="att")!=capture.directory))return fail(error,"Attachment capture kind does not match its flat namespace.");
    FileReadParams normalized;
    if(!paramsFromJson(capture.tool,capture.params.toJson(),&normalized,error)||normalized!=capture.params)return fail(error,"Invalid or unnormalized captured read parameters.");
    if(capture.tool=="ls"&&!capture.directory)return fail(error,"Listing capture must be a directory.");
    if(capture.directory){
        const double expected=capture.params.preview.value_or(false)?0:capture.params.depth.value_or(2);
        if(!std::isfinite(capture.capturedDepth)||capture.capturedDepth<0||capture.capturedDepth>32||capture.capturedDepth!=expected||!capture.bytes.isEmpty())return fail(error,"Invalid captured directory depth or body.");
        QJsonArray entries;int count=0;if(!encodeEntries(capture.entries,capture.capturedDepth,0,&count,&entries,error))return false;
        *payload=QJsonDocument(QJsonObject{{"schema",1},{"kind","directory"},{"depth",capture.capturedDepth},{"entries",entries}}).toJson(QJsonDocument::Compact);
        if(payload->size()>MaxManifestBytes)return fail(error,"Directory snapshot exceeds its encoded byte limit.");
    }else{
        if(capture.bytes.size()>MaxFileBytes||capture.descriptor.size!=capture.bytes.size()||!capture.entries.isEmpty())return fail(error,"Invalid captured file byte count or body.");
        *payload=capture.bytes;
    }
    return true;
}
QJsonObject metadata(const FileReadCapture& capture,const QString& hash){
    const auto path=capture.descriptor.mountedPath;
    QJsonObject details{{"kind",capture.directory?"directory":"file"},{"originalFileName",capture.descriptor.originalFileName}};
    if(capture.directory){details["manifestVersion"]=1;details["capturedDepth"]=capture.capturedDepth;}
    else details["size"]=double(capture.bytes.size());
    if(capture.descriptor.modified.isValid())details["modified"]=capture.descriptor.modified.toUTC().toString(Qt::ISODateWithMs);
    QJsonObject request{{"version",1},{"path",path},{"tool",capture.tool},{"kind",capture.directory?"directory":"file"},{"params",capture.params.toJson()}};
    if(!capture.inputHash.isEmpty())request["inputHash"]=capture.inputHash;
    return {{"fileReads",QJsonArray{request}},
        {"pathReferences",QJsonObject{{path,hash}}},{"pathMetadata",QJsonObject{{path,details}}}};
}
}
struct FileReadSnapshotStore::State {
    State(QString root,QString agent):directory(std::move(root)),agentId(std::move(agent)),store(directory){}
    QString directory,agentId;AttachmentStore store;QMutex mutex;quint64 epoch=1;bool active=true;
    QList<std::shared_ptr<void>> pins;
    bool pinStorage(bool create,QString* error){
        if(!pins.isEmpty())return true;
        const auto base=QFileInfo(directory).absolutePath();
        if(create&&!QDir().mkpath(base))return fail(error,"Cannot create the read snapshot data directory.");
        QList<std::shared_ptr<void>> held;
        QString current=base;
        for(const auto& suffix:QStringList{"","read-context-v1","attachment-snapshots",agentId}){
            if(!suffix.isEmpty()){
                current+='/'+suffix;
                if(create&&!QFileInfo::exists(current)&&!QDir().mkdir(current))return fail(error,"Cannot create an owned read snapshot directory.");
            }
            ScopedDirectorySnapshot captured;bool unused=false;
            // Only acquire/validate the directory handle; no child-name listing is needed.
            if(!readScopedDirectory(current,current,&captured,error,0,nullptr,false,&unused))return false;
            held.append(std::move(captured.pin));
        }
        pins=std::move(held);return true;
    }
};
FileReadSnapshotStore::FileReadSnapshotStore(QString dataDirectory){
    if(!dataDirectory.isEmpty()){
        const QFileInfo info(dataDirectory);const auto canonical=info.canonicalFilePath();
        m_directory=QDir::cleanPath((canonical.isEmpty()?info.absoluteFilePath():canonical)+"/read-context-v1");
    }
}
bool FileReadSnapshotStore::Lease::belongsTo(const QString& agentId)const{
    if(!state)return false;
    QMutexLocker lock(&state->mutex);
#ifdef Q_OS_WIN
    constexpr auto sensitivity=Qt::CaseInsensitive;
#else
    constexpr auto sensitivity=Qt::CaseSensitive;
#endif
    return state->active&&epoch==state->epoch&&state->agentId.compare(agentId,sensitivity)==0;
}
std::shared_ptr<FileReadSnapshotStore::State> FileReadSnapshotStore::stateFor(const QString& agent,QString* error)const{
    if(error)error->clear();
    if(m_directory.isEmpty()||!QRegularExpression("^[A-Za-z0-9_-]{1,128}$").match(agent).hasMatch()){fail(error,"Invalid read snapshot data directory or chat identity.");return {};}
    static QMutex registryMutex;static QHash<QString,std::weak_ptr<State>> registry;
    QMutexLocker lock(&registryMutex);auto key=m_directory+'/'+agent;
#ifdef Q_OS_WIN
    key=key.toCaseFolded();
#endif
    auto state=registry.value(key).lock();
    if(!state){state=std::make_shared<State>(m_directory,agent);registry[key]=state;}
    // Dead weak entries do not retain chats or credentials; bound registry bookkeeping.
    if(registry.size()>4096){for(auto i=registry.begin();i!=registry.end();){if(i->expired())i=registry.erase(i);else ++i;}}
    return state;
}
FileReadSnapshotStore::Lease FileReadSnapshotStore::acquire(const QString& agent,QString* error)const{
    Lease result;result.state=stateFor(agent,error);if(!result.state)return result;
    QMutexLocker lock(&result.state->mutex);
    if(!result.state->active){result.state->active=true;++result.state->epoch;}
    result.epoch=result.state->epoch;return result;
}
QJsonObject FileReadSnapshotStore::commit(const Lease& lease,const FileReadCapture& capture,const std::atomic_bool* cancelled,QString* error){
    if(error)error->clear();
    QByteArray payload;
    if(!lease.state){fail(error,"Read snapshot lease is unavailable.");return {};}
    if(cancelled&&cancelled->load()){fail(error,"Read snapshot capture canceled.");return {};}
    if(!validate(capture,&payload,error))return {};
    QMutexLocker lock(&lease.state->mutex);
    if(!lease.state->active||lease.epoch!=lease.state->epoch||(cancelled&&cancelled->load())){fail(error,"Read snapshot lease was revoked or canceled.");return {};}
    if(!lease.state->pinStorage(true,error))return {};
    if(!lease.state->store.saveSnapshot(lease.state->agentId,storageRef(capture.descriptor.mountedPath,capture.directory),payload,error))return {};
    if(cancelled&&cancelled->load()){fail(error,"Read snapshot capture canceled.");return {};}
    return metadata(capture,digest(payload));
}
bool FileReadSnapshotStore::read(const Lease& lease,const QJsonObject& meta,FileReadCapture* capture,QString* error){
    if(error)error->clear();
    if(!capture)return fail(error,"Missing read snapshot result.");
    *capture={};
    if(!lease.state||!meta["fileReads"].isArray()||meta["fileReads"].toArray().size()!=1||!meta["pathReferences"].isObject()||!meta["pathMetadata"].isObject())return fail(error,"Invalid read snapshot metadata.");
    const auto value=meta["fileReads"].toArray().first();if(!value.isObject())return fail(error,"Invalid read snapshot request.");const auto request=value.toObject();
    if(!keys(request,{"version","path","tool","kind","params","inputHash"})||request["version"]!=1||!request["path"].isString()||!request["tool"].isString()||!request["kind"].isString()||!request["params"].isObject())return fail(error,"Invalid read snapshot request fields.");
    if(request.contains("inputHash")&&!request["inputHash"].isString())return fail(error,"Invalid captured tool input hash.");
    capture->inputHash=request["inputHash"].toString();
    capture->descriptor.mountedPath=request["path"].toString();capture->tool=request["tool"].toString();
    const auto kind=request["kind"].toString();capture->directory=kind=="directory";
    if(!pathValid(capture->descriptor.mountedPath)||(kind!="directory"&&kind!="file")||!paramsFromJson(capture->tool,request["params"].toObject(),&capture->params,error))return fail(error,"Invalid read snapshot path, kind or parameters.");
    const auto hashes=meta["pathReferences"].toObject(),descriptors=meta["pathMetadata"].toObject();const auto path=capture->descriptor.mountedPath;
    if(hashes.size()!=1||descriptors.size()!=1||!hashes[path].isString()||!descriptors[path].isObject())return fail(error,"Read snapshot path metadata does not match its request.");
    const auto details=descriptors[path].toObject();
    if(!keys(details,{"kind","originalFileName","size","modified","manifestVersion","capturedDepth"})||details["kind"]!=kind||!details["originalFileName"].isString())return fail(error,"Invalid read snapshot descriptor.");
    capture->descriptor.originalFileName=details["originalFileName"].toString();
    if(details.contains("modified")){
        if(!details["modified"].isString()||details["modified"].toString().size()>40)return fail(error,"Invalid snapshot modification time.");
        capture->descriptor.modified=QDateTime::fromString(details["modified"].toString(),Qt::ISODateWithMs);
        if(!capture->descriptor.modified.isValid()||capture->descriptor.modified.toUTC().toString(Qt::ISODateWithMs)!=details["modified"].toString())return fail(error,"Invalid snapshot modification time.");
    }
    QByteArray payload;
    {QMutexLocker lock(&lease.state->mutex);
        if(!lease.state->active||lease.epoch!=lease.state->epoch)return fail(error,"Read snapshot lease was revoked.");
        if(!lease.state->pinStorage(false,error))return false;
        payload=lease.state->store.readSnapshot(lease.state->agentId,storageRef(path,capture->directory),hashes[path].toString(),error);
        if(error&&!error->isEmpty())return false;
        if(payload.isNull())return fail(error,"The original read snapshot is unavailable.");
    }
    if(capture->directory){
        if(details.contains("size")||details["manifestVersion"]!=1||!details["capturedDepth"].isDouble()||payload.size()>MaxManifestBytes)return fail(error,"Invalid directory snapshot descriptor or size.");
        capture->capturedDepth=details["capturedDepth"].toDouble(-1);
        if(!std::isfinite(capture->capturedDepth)||capture->capturedDepth<0||capture->capturedDepth>32)return fail(error,"Invalid directory snapshot depth.");
        QJsonParseError parse;const auto document=QJsonDocument::fromJson(payload,&parse);const auto body=document.object();
        if(parse.error!=QJsonParseError::NoError||!document.isObject()||!keys(body,{"schema","kind","depth","entries"})||body["schema"]!=1||body["kind"]!="directory"||!body["depth"].isDouble()||body["depth"].toDouble()!=capture->capturedDepth||!body["entries"].isArray())return fail(error,"Invalid typed directory snapshot.");
        int count=0;if(!decodeEntries(body["entries"].toArray(),capture->capturedDepth,0,&count,&capture->entries,error))return false;
    }else{
        if(details.contains("manifestVersion")||details.contains("capturedDepth")||!details["size"].isDouble()||details["size"].toDouble(-1)!=double(payload.size()))return fail(error,"Invalid file snapshot size.");
        capture->bytes=payload;capture->descriptor.size=payload.size();
    }
    QByteArray verified;if(!validate(*capture,&verified,error))return false;
    if(verified!=payload)return fail(error,"Read snapshot encoding is not canonical.");
    return true;
}
void FileReadSnapshotStore::revoke(const Lease& lease){
    if(lease.state){
        QMutexLocker lock(&lease.state->mutex);
        if(lease.epoch==lease.state->epoch){lease.state->active=false;++lease.state->epoch;}
    }
}
bool FileReadSnapshotStore::removeAgent(const QString& agent,QString* error)const{
    const auto state=stateFor(agent,error);if(!state)return false;QMutexLocker lock(&state->mutex);state->active=false;++state->epoch;
    state->pins.clear();
    if(!QFileInfo::exists(state->directory))return true;
    return state->store.removeAgent(agent,error);
}
bool FileReadSnapshotStore::cloneAgent(const QString& source,const QString& target,QString* error)const{
    if(source==target)return fail(error,"Read snapshots require distinct clone identities.");
    const auto from=stateFor(source,error);if(!from)return false;
    const auto to=stateFor(target,error);if(!to)return false;
    if(from==to)return fail(error,"Read snapshots require physically distinct clone identities.");
    const bool ordered=std::less<State*>{}(from.get(),to.get());
    QMutexLocker first(ordered?&from->mutex:&to->mutex),second(ordered?&to->mutex:&from->mutex);
    if(!from->active)return fail(error,"Source read snapshot lease was revoked.");
    if(to.use_count()>1)return fail(error,"Read snapshot clone destination already has an owner.");
    // A failed clone must not invalidate an existing destination owner's lease.
    // Pin and preflight even for an empty source, before changing either realm.
    if(!to->pinStorage(true,error))return false;
    for(const auto& category:QStringList{"attachments","attachment-snapshots"}){
        const auto destination=to->directory+'/'+category+'/'+to->agentId;
        if(!QFileInfo::exists(destination))continue;
        ScopedDirectorySnapshot snapshot;bool nonempty=false;
        if(!readScopedDirectory(destination,destination,&snapshot,error,0,nullptr,false,&nonempty))return false;
        if(nonempty)return fail(error,"Read snapshot clone destination must be empty.");
    }
    if(QFileInfo::exists(from->directory+"/attachment-snapshots/"+from->agentId)){
        if(!from->pinStorage(false,error))return false;
        if(!from->store.cloneAgent(from->agentId,to->agentId,error))return false;
    }
    to->active=false;++to->epoch;
    return true;
}
}
