#include "AttachmentFileBroker.h"
#include "core/ScopedDirectory.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QTimer>

namespace ade {
namespace {
constexpr qsizetype MaxTextCharacters=65536;constexpr qint64 MaxBytes=4*1024*1024;
// Keep this flat namespace identical to AttachmentStore's blob-name contract.
// Validation uses the enumerated name, never a second filesystem lookup.
bool validBlobName(const QString& name){
    return !name.isEmpty()&&name.size()<=180&&name!="."&&name!=".."
        &&QRegularExpression("^[A-Za-z0-9_.-]+$").match(name).hasMatch()
        &&!name.endsWith('.')&&!QRegularExpression("^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$",QRegularExpression::CaseInsensitiveOption).match(name).hasMatch();
}
}
AttachmentFileBroker::AttachmentFileBroker(const QString& agentId,const QString& directory,QObject* parent)
    :QObject(parent),m_store(directory),m_agentId(agentId){m_root=m_store.agentDirectory(agentId,&m_rootError,true);}
FileReadSource AttachmentFileBroker::formattedSource() const {
    if(m_root.isEmpty())return {};
    // Use the accepted physical data directory, not the caller's possibly aliased
    // path. Each observation pins the original flat blob root while reading.
    const auto directory=QFileInfo(QFileInfo(m_root).absolutePath()).absolutePath();
    return [store=AttachmentStore(directory),agent=m_agentId,root=m_root](const QString& tool,const QString& path,
        const FileReadParams& params,FileReadCapture* result,const std::atomic_bool* canceled,QString* error){
        const auto fail=[&](const QString& message){if(error)*error=message;return false;};
        if(error)error->clear();
        if(!result)return fail("A file capture destination is required.");
        *result={};
        if(canceled&&canceled->load())return fail("Attachment read canceled.");
        if(tool!="read"&&tool!="ls")return fail("Formatted attachment access allows read and ls only.");
        if(path!="att"&&path!="att/"&&(!path.startsWith("att/")||!validBlobName(path.mid(4))))
            return fail("Formatted attachment access requires a flat att/ blob path.");
        const bool directoryRead=path=="att"||path=="att/";
        if(tool=="ls"&&!directoryRead)return fail("Search path is not a directory.");
        ScopedDirectorySnapshot snapshot;bool truncated=false;
        if(!readScopedDirectory(root,root,&snapshot,error,directoryRead?4096:0,canceled,false,directoryRead?nullptr:&truncated))return false;
        FileReadCapture capture;capture.tool=tool;capture.params=params;
        capture.descriptor={directoryRead?QStringLiteral("att"):path,directoryRead?QStringLiteral("att"):path.mid(4),-1,{}};
        if(directoryRead){
            capture.directory=true;capture.descriptor.modified=snapshot.modified;
            capture.capturedDepth=params.preview.value_or(false)?0:params.depth.value_or(2);
            if(capture.capturedDepth>32)return fail("Requested directory depth exceeds the native limit.");
            for(const auto& entry:snapshot.entries){
                if(canceled&&canceled->load())return fail("Attachment read canceled.");
                if(entry.isDirectory||entry.isReparsePoint)return fail("Attachment listing refuses unexpected directories or links.");
                if(!validBlobName(entry.name))return fail("Attachment listing contains an invalid blob name.");
                capture.entries.append(FileTreeEntry{entry.name,false,{},{}});
            }
        }else{
            QString readError;
            capture.bytes=store.read(agent,{path,{}},{},&readError,MaxBytes,&capture.descriptor.modified);
            if(!readError.isEmpty())return fail(readError);
            capture.descriptor.size=capture.bytes.size();
        }
        if(canceled&&canceled->load())return fail("Attachment read canceled.");
        *result=std::move(capture);return true;
    };
}
void AttachmentFileBroker::execute(const QString& id,const QString& agentId,const QString& operation,const QJsonObject& arguments){
    if(id.isEmpty()||id.size()>512||agentId!=m_agentId){emit completed(id,"Invalid attachment request identity.",true);return;}
    if(m_pending.contains(id)){cancel(id);return;}
    if(m_pending.size()>=128){emit completed(id,"Too many pending attachment reads.",true);return;}
    if(operation!="readFile"&&operation!="readBytes"&&operation!="readDirectory"){emit completed(id,"The att/ mount allows read and list operations only.",true);return;}
    const auto generation=++m_generation;m_pending.insert(id,generation);
    QTimer::singleShot(0,this,[this,id,generation,operation,arguments]{perform(id,generation,operation,arguments);});
}
void AttachmentFileBroker::perform(const QString& id,quint64 generation,const QString& operation,const QJsonObject& arguments){
    if(m_pending.value(id)!=generation)return;
    if(m_root.isEmpty()){finish(id,m_rootError.isEmpty()?QStringLiteral("Attachment mount is unavailable."):m_rootError,true);return;}
    const auto path=arguments["path"].toString();QString error;
    if(operation=="readDirectory"){
        const auto result=list(path,arguments["recursive"].toBool(),&error);finish(id,error.isEmpty()?result:error,!error.isEmpty());return;
    }
    if(!path.startsWith("att/")){finish(id,"Attachment reads require an att/ blob path.",true);return;}
    const auto bytes=m_store.read(m_agentId,AttachmentRef{path,{}},{},&error,MaxBytes);
    if(!error.isEmpty()){finish(id,error,true);return;}
    if(operation=="readBytes"){finish(id,QString::fromLatin1(bytes.toBase64()),false);return;}
    QStringDecoder decoder(QStringDecoder::Utf8,QStringConverter::Flag::ConvertInitialBom|QStringConverter::Flag::Stateless);
    const QString text=decoder(bytes);
    if(decoder.hasError()){finish(id,"Attachment is not valid UTF-8; request raw Buffer bytes instead.",true);return;}
    if(text.size()>MaxTextCharacters){finish(id,"Attachment text exceeds 64 Ki characters; no truncated result was returned.",true);return;}
    finish(id,text,false);
}
QString AttachmentFileBroker::list(const QString& path,bool recursive,QString* error) const{
    Q_UNUSED(recursive)
    // AttachmentStore's owned blob namespace is flat; snapshots and subfolders
    // are not attachment names. Recursion has the same result for that layout.
    if(path!="att"&&path!="att/"){*error="Only the flat att/ blob directory can be listed.";return {};}
    ScopedDirectorySnapshot snapshot;
    if(!readScopedDirectory(m_root,m_root,&snapshot,error,4096,nullptr,false))return {};
    QJsonArray names;
    for(const auto& entry:snapshot.entries){
        if(entry.isDirectory||entry.isReparsePoint){*error="Attachment listing refuses unexpected directories or links.";return {};}
        if(!validBlobName(entry.name)){*error="Attachment listing contains an invalid blob name.";return {};}
        names.append(entry.name);
    }
    const auto result=QString::fromUtf8(QJsonDocument(names).toJson(QJsonDocument::Compact));
    if(result.size()>MaxTextCharacters){*error="Attachment listing exceeds 64 Ki characters.";return {};}
    // snapshot.pin retains the validated directory handle through serialization.
    // Names come from that handle; no path reopen can mix a retargeted namespace.
    return result;
}
void AttachmentFileBroker::finish(const QString& id,const QString& output,bool error){if(!m_pending.remove(id))return;emit completed(id,output,error);}
void AttachmentFileBroker::cancel(const QString& id){finish(id,"Attachment request canceled.",true);}
void AttachmentFileBroker::cancelAll(){const auto ids=m_pending.keys();const QPointer<AttachmentFileBroker> self(this);for(const auto& id:ids){cancel(id);if(!self)return;}}
}
