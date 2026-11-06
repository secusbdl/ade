#include "FileMutationStore.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringDecoder>
#include <QUuid>
#include <windows.h>
#include <vector>

namespace ade::mutation_stage {
namespace {
constexpr qint64 MaxBytes=4*1024*1024;
using Handle=std::shared_ptr<void>;
struct Failure {QString text;};
[[noreturn]] void fail(const QString& message){throw Failure{message};}
QString sha(const QByteArray& value){return QString::fromLatin1(QCryptographicHash::hash(value,QCryptographicHash::Sha256).toHex());}
QString winError(const QString& text){return text+QString(" (Windows %1)").arg(GetLastError());}
HANDLE raw(const Handle& value){return static_cast<HANDLE>(value.get());}
Handle own(HANDLE value){if(value==INVALID_HANDLE_VALUE)fail(winError("Cannot open scoped filesystem object"));return {value,[](void* h){CloseHandle(static_cast<HANDLE>(h));}};}
QString finalPath(HANDLE handle){
    const DWORD size=GetFinalPathNameByHandleW(handle,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!size||size>32768)fail("Cannot validate opened path");
    std::vector<wchar_t> buffer(size+1);const DWORD written=GetFinalPathNameByHandleW(handle,buffer.data(),DWORD(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!written||written>=buffer.size())fail("Cannot validate opened path");
    QString path=QString::fromWCharArray(buffer.data(),written);
    if(path.startsWith("\\\\?\\UNC\\"))path="\\\\"+path.mid(8);else if(path.startsWith("\\\\?\\"))path.remove(0,4);
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}
BY_HANDLE_FILE_INFORMATION info(HANDLE handle){BY_HANDLE_FILE_INFORMATION result{};if(!GetFileInformationByHandle(handle,&result))fail("Cannot inspect opened object");return result;}
QString identity(HANDLE handle){const auto v=info(handle);return QString("%1:%2:%3").arg(v.dwVolumeSerialNumber,8,16,QChar('0')).arg(v.nFileIndexHigh,8,16,QChar('0')).arg(v.nFileIndexLow,8,16,QChar('0'));}
Handle directory(const QString& path){
    auto handle=own(CreateFileW(QDir::toNativeSeparators(path).toStdWString().c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|SYNCHRONIZE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    const auto v=info(raw(handle));if(!(v.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(v.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||finalPath(raw(handle)).compare(path,Qt::CaseInsensitive))fail("Directory identity or link changed");return handle;
}
QList<Handle> pinAbsolute(const QString& path){
    if(!QRegularExpression("^[A-Za-z]:/").match(path).hasMatch())fail("Private mutation stage requires a local drive path");
    QString current=path.left(3);QList<Handle> result{directory(current)};
    for(const auto& part:path.mid(3).split('/',Qt::SkipEmptyParts)){current=QDir::cleanPath(current+'/'+part);result.append(directory(current));}
    return result;
}
bool validPart(const QString& name){
    static const QRegularExpression device("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$",QRegularExpression::CaseInsensitiveOption);
    return !name.isEmpty()&&name!="."&&name!=".."&&name.size()<=255&&!name.endsWith('.')&&!name.endsWith(' ')&&!name.contains(QRegularExpression("[<>:\"/\\\\|?*\\x00-\\x1f]"))&&!device.match(name).hasMatch()&&!name.startsWith(".ade-mutation-");
}
struct State {bool exists=false;QByteArray bytes;QString id;QString hash()const{return exists?sha(bytes):QString();}};
struct ScopedPath {QString logical,root,rootId,parentPath,leaf;QList<Handle> parents;Handle file;State state;};
State state(HANDLE handle){
    const auto v=info(handle);if(v.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))fail("Single-file mutation rejects directories and links");
    if(v.nNumberOfLinks!=1)fail("Single-file mutation rejects hard-linked files");
    LARGE_INTEGER length{};if(!GetFileSizeEx(handle,&length)||length.QuadPart<0||length.QuadPart>MaxBytes)fail("Single-file mutation exceeds 4 MiB capture bound");
    LARGE_INTEGER zero{};if(!SetFilePointerEx(handle,zero,nullptr,FILE_BEGIN))fail("Cannot seek file snapshot");
    State result;result.exists=true;result.id=identity(handle);QByteArray chunk(65536,Qt::Uninitialized);
    while(true){DWORD count=0;if(!ReadFile(handle,chunk.data(),DWORD(chunk.size()),&count,nullptr))fail("Cannot capture file bytes");if(!count)break;if(result.bytes.size()+count>MaxBytes)fail("File grew beyond mutation capture bound");result.bytes.append(chunk.constData(),count);}
    return result;
}
Handle openFile(const QString& path,bool change,bool writable=false){
    HANDLE value=CreateFileW(QDir::toNativeSeparators(path).toStdWString().c_str(),GENERIC_READ|(change?DELETE:0)|(writable?GENERIC_WRITE:0),FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(value==INVALID_HANDLE_VALUE){const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND)return {};fail(winError("Cannot lock scoped file"));}
    auto result=own(value);if(finalPath(raw(result)).compare(path,Qt::CaseInsensitive))fail("Opened file path changed");return result;
}
ScopedPath scoped(const QString& logical,const QString& root,const QString& rootId,bool change){
    const auto parts=logical.split('/');if(parts.size()<2||parts.size()>34)fail("A file below a mount with at most 32 parent directories is required");
    ScopedPath result;result.logical=logical;result.root=root;result.rootId=rootId;result.parentPath=root;result.leaf=parts.last();
    result.parents=pinAbsolute(root);if(identity(raw(result.parents.last()))!=rootId)fail("Mount identity changed");
    for(int i=1;i<parts.size();++i){if(!validPart(parts[i]))fail("Invalid or reserved mutation path component");if(i+1<parts.size()){result.parentPath+='/'+parts[i];result.parents.append(directory(result.parentPath));}}
    result.file=openFile(result.parentPath+'/'+result.leaf,change);if(result.file)result.state=state(raw(result.file));return result;
}
void renameTo(const Handle& file,const ScopedPath& target,const QString& name){
    const auto wide=QDir::toNativeSeparators(target.parentPath+'/'+name).toStdWString();std::vector<char> buffer(offsetof(FILE_RENAME_INFO,FileName)+(wide.size()+1)*sizeof(wchar_t),0);
    auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());rename->ReplaceIfExists=FALSE;rename->RootDirectory=nullptr;rename->FileNameLength=DWORD(wide.size()*sizeof(wchar_t));std::copy(wide.begin(),wide.end(),rename->FileName);
    if(!SetFileInformationByHandle(raw(file),FileRenameInfo,rename,DWORD(buffer.size())))fail(winError("Scoped rename failed"));
}
void removeOpened(Handle& file){if(!file)return;FILE_DISPOSITION_INFO disposition{TRUE};if(!SetFileInformationByHandle(raw(file),FileDispositionInfo,&disposition,sizeof(disposition)))fail(winError("Scoped delete failed"));file.reset();}
Handle createEmpty(const ScopedPath& target,const QString& name){
    const auto path=target.parentPath+'/'+name;
    auto file=own(CreateFileW(QDir::toNativeSeparators(path).toStdWString().c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(finalPath(raw(file)).compare(path,Qt::CaseInsensitive))fail("Created staging file escaped its parent");
    return file;
}
void writeOpened(const Handle& file,const QByteArray& bytes,const std::function<void()>& onChunk={}){
    LARGE_INTEGER zero{};if(!SetFilePointerEx(raw(file),zero,nullptr,FILE_BEGIN)||!SetEndOfFile(raw(file)))fail("Cannot reset owned staging file");
    qsizetype offset=0;while(offset<bytes.size()){DWORD count=0;if(!WriteFile(raw(file),bytes.constData()+offset,DWORD(qMin<qsizetype>(65536,bytes.size()-offset)),&count,nullptr)||!count)fail("Cannot write staged bytes");offset+=count;if(onChunk)onChunk();}
    if(!FlushFileBuffers(raw(file)))fail("Cannot flush staged bytes");
}
std::unique_ptr<QLockFile> ownerLock(const QString& directory,const QString& agent){
    auto lock=std::make_unique<QLockFile>(directory+"/owner-"+sha(agent.toCaseFolded().toUtf8())+".lock");
    lock->setStaleLockTime(0);if(!lock->tryLock(0))fail("Mutation journal owner is busy; recovery cannot overlap live work");return lock;
}
void query(QSqlDatabase& db,const QString& sql,const QVariantList& values={}){QSqlQuery q(db);if(!q.prepare(sql))fail(q.lastError().text());for(const auto& v:values){if(v.metaType().id()==QMetaType::QString&&v.toString().isNull())q.addBindValue(QString(""));else if(v.metaType().id()==QMetaType::QByteArray&&v.toByteArray().isNull())q.addBindValue(QByteArray(""));else q.addBindValue(v);}if(!q.exec())fail(q.lastError().text());}
QString stripped(QString content){if(content.startsWith("```")){auto lines=content.split('\n');if(lines.first().trimmed().startsWith("```"))lines.removeFirst();content=lines.join('\n');}if(content.endsWith("```")){auto lines=content.split('\n');if(lines.last().trimmed()=="```")lines.removeLast();content=lines.join('\n');}return content;}
bool same(const State& a,const State& b,bool checkId=true){return a.exists==b.exists&&(!a.exists||(a.bytes==b.bytes&&(!checkId||b.id.isEmpty()||a.id==b.id)));}
QString token(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
bool validToken(const QString& value){return QRegularExpression("^[A-Za-z0-9_-]{1,128}$").match(value).hasMatch();}
}
struct MutationEntry {ScopedPath path;State before,after;QString backupName,stageName,restoreId;Handle backup,stage,restore;};
FileMutationStore::FileMutationStore(QString dataDirectory,QString agentId):m_directory(QDir::cleanPath(QFileInfo(dataDirectory).absoluteFilePath()+"/mutation-journal-v1")),m_connection("mutation-"+token()),m_agentId(std::move(agentId)){}
FileMutationStore::~FileMutationStore(){m_db.close();m_db={};QSqlDatabase::removeDatabase(m_connection);}
bool FileMutationStore::open(QString* error){
    if(error)error->clear();
    try{
        if(!validToken(m_agentId))fail("Invalid journal agent identity");
        if(m_db.isOpen())return true;
        auto held=pinAbsolute(QFileInfo(m_directory).absolutePath());
        if(!QFileInfo::exists(m_directory)&&!QDir().mkdir(m_directory))fail("Cannot create mutation journal directory");
        held.append(directory(m_directory));m_storagePin=std::make_shared<QList<Handle>>(std::move(held));
        const auto path=m_directory+"/journal.sqlite";if(QFileInfo(path).isSymLink()||QFileInfo(path).isJunction())fail("Journal database cannot be a link");
        m_db=QSqlDatabase::addDatabase("QSQLITE",m_connection);m_db.setDatabaseName(path);if(!m_db.open())fail(m_db.lastError().text());
        query(m_db,"PRAGMA journal_mode=WAL");query(m_db,"PRAGMA synchronous=FULL");query(m_db,"PRAGMA foreign_keys=ON");
        query(m_db,"CREATE TABLE IF NOT EXISTS snapshots(hash TEXT PRIMARY KEY, bytes BLOB NOT NULL)");
        query(m_db,"CREATE TABLE IF NOT EXISTS mutations(nonce TEXT PRIMARY KEY, agent TEXT NOT NULL, tool_call TEXT NOT NULL, tool TEXT NOT NULL, undo_of TEXT NOT NULL, state TEXT NOT NULL, error TEXT NOT NULL, output TEXT NOT NULL,cleaned INTEGER NOT NULL DEFAULT 0)");
        query(m_db,"CREATE TABLE IF NOT EXISTS entries(nonce TEXT NOT NULL REFERENCES mutations(nonce), ordinal INTEGER NOT NULL, logical TEXT NOT NULL, root TEXT NOT NULL, root_id TEXT NOT NULL, before_exists INTEGER NOT NULL,before_hash TEXT NOT NULL,before_id TEXT NOT NULL,after_exists INTEGER NOT NULL,after_hash TEXT NOT NULL,after_id TEXT NOT NULL,backup TEXT NOT NULL,stage TEXT NOT NULL,restore_id TEXT NOT NULL DEFAULT '',PRIMARY KEY(nonce,ordinal))");
        return true;
    }catch(const Failure& e){if(error)*error=e.text;return false;}
}
bool FileMutationStore::bindMount(const QString& prefix,const QString& root,bool writable,QString* error){
    if(error)error->clear();
    try{if(prefix!="ws1"&&prefix!="apps"&&prefix!="att")fail("Unknown private-stage mount");if(prefix=="att"&&writable)fail("Attachment mount must remain read-only");
        const auto canonical=QDir::fromNativeSeparators(QFileInfo(root).canonicalFilePath());if(canonical.isEmpty())fail("Mount directory unavailable");auto handle=directory(canonical);m_mounts[prefix]={canonical,identity(raw(handle)),writable};return true;
    }catch(const Failure& e){if(error)*error=e.text;return false;}
}
bool FileMutationStore::status(const QString& nonce,const QString& value,const QString& message,QString* error){try{query(m_db,"UPDATE mutations SET state=?,error=? WHERE nonce=?",{value,message,nonce});return true;}catch(const Failure& e){if(error)*error=e.text;return false;}}
Result FileMutationStore::execute(const Request& request,const std::atomic_bool* cancelled){
    Result result;result.mutationId=request.nonce;try{
        if(!m_db.isOpen()||request.agentId!=m_agentId||!validToken(request.agentId)||!validToken(request.nonce)||request.toolCallId.isEmpty()||request.toolCallId.size()>512)fail("Invalid mutation identity or unopened store");
        auto owner=ownerLock(m_directory,m_agentId);
        if(cancelled&&cancelled->load())fail("Mutation canceled before preparation");
        const auto resolve=[&](const QString& logical,bool change){const auto prefix=logical.section('/',0,0);if(!m_mounts.contains(prefix))fail("Mutation mount unavailable");const auto mount=m_mounts[prefix];if(change&&!mount.writable)fail("Mutation mount is read-only");return scoped(logical,mount.root,mount.identity,change);};
        QList<MutationEntry> entries;QJsonObject output;
        if(request.tool=="copy"){
            if(request.path==request.outputPath)fail("Source and destination must differ");
            auto source=resolve(request.path,request.move);if(!source.state.exists)fail("Source file does not exist");
            auto destination=resolve(request.outputPath,true);if(destination.state.exists&&destination.state.id==source.state.id)fail("Source and destination identify the same file");
            MutationEntry dest;dest.path=std::move(destination);dest.before=dest.path.state;dest.after={true,source.state.bytes,{}};entries.append(std::move(dest));
            if(request.move){MutationEntry from;from.path=std::move(source);from.before=from.path.state;entries.append(std::move(from));}
            output={{"message",QString(request.move?"Moved file: %1 → %2":"Copied file: %1 → %2").arg(request.path,request.outputPath)}};
        }else{
            MutationEntry entry;entry.path=resolve(request.path,true);entry.before=entry.path.state;
            if(request.tool=="delete"){if(!entry.before.exists)fail("File not found");}
            else if(request.tool=="write"){
                const auto text=stripped(request.content);entry.after={true,text.toUtf8(),{}};
                auto message=QString("Successfully %1 file: %2").arg(entry.before.exists?"updated":"created",request.path.section('/',1));
                if(text.size()>4000)message+=QString("\n\n⚠️ Large file write (%1 chars). Prefer incremental edits rather than making large changes like this again.").arg(text.size());
                output={{"message",message}};
            }else if(request.tool=="multiEdit"){
                if(!entry.before.exists||request.edits.isEmpty())fail("multiEdit requires an existing file and edits");
                QStringDecoder decoder(QStringDecoder::Utf8,QStringConverter::Flag::ConvertInitialBom|QStringConverter::Flag::Stateless);QString text=decoder(entry.before.bytes);if(decoder.hasError()||entry.before.bytes.contains('\0'))fail("multiEdit requires valid UTF-8 text");
                int applied=0;for(const auto& edit:request.edits){if(edit.before.isEmpty())fail("Empty multiEdit search strings are unsupported");int count=0;qsizetype found=0;while((found=text.indexOf(edit.before,found))>=0){++count;found+=edit.before.size();}if(!count)continue;const qint64 replacements=edit.all?count:1;
                    if(edit.after.size()>MaxBytes)fail("multiEdit expansion exceeds 4 MiB bound");
                    const qint64 predicted=text.toUtf8().size()+replacements*(qint64(edit.after.toUtf8().size())-edit.before.toUtf8().size());
                    if(predicted<0||predicted>MaxBytes)fail("multiEdit expansion exceeds 4 MiB bound");
                    if(edit.all){text.replace(edit.before,edit.after);applied+=count;}else{text.replace(text.indexOf(edit.before),edit.before.size(),edit.after);++applied;}}
                entry.after={true,text.toUtf8(),{}};output={{"message",applied?QString("Successfully applied %1 edits").arg(applied):QString("Applied 0 edits to %1.").arg(request.path.section('/',1))},{"result",QJsonObject{{"editsApplied",applied}}}};
            }else fail("Unsupported private-stage mutation tool");
            if(entry.after.bytes.size()>MaxBytes)fail("Mutation output exceeds 4 MiB bound");
            entries.append(std::move(entry));
        }
        return apply(request,std::move(entries),output,{},cancelled);
    }catch(const Failure& e){result.error=e.text;return result;}
}
Result FileMutationStore::apply(const Request& request,QList<MutationEntry> entries,QJsonObject output,const QString& undoOf,const std::atomic_bool* cancelled){
    Result result;result.mutationId=request.nonce;bool prepared=false,committed=false;
    const auto boundary=[&](const QString& name,int i){if(cancelled&&cancelled->load())fail("Mutation canceled");const auto action=m_fault?m_fault(name,i):Fault::None;if(action==Fault::Fail)fail("Injected failure at "+name);if(action==Fault::Interrupt)throw QString("Interrupted at "+name);};
    try{
        boundary("before-prepare",-1);
        for(int i=0;i<entries.size();++i){auto& entry=entries[i];entry.backupName=QString(".ade-mutation-%1-%2.backup").arg(request.nonce).arg(i);entry.stageName=QString(".ade-mutation-%1-%2.stage").arg(request.nonce).arg(i);
            if(QFileInfo::exists(entry.path.parentPath+'/'+entry.backupName)||QFileInfo::exists(entry.path.parentPath+'/'+entry.stageName))fail("Mutation staging name already exists");}
        if(!m_db.transaction())fail(m_db.lastError().text());
        try{
            query(m_db,"INSERT INTO mutations(nonce,agent,tool_call,tool,undo_of,state,error,output) VALUES(?,?,?,?,?,?,?,?)",{request.nonce,request.agentId,request.toolCallId,request.tool,undoOf,"prepared","",QString::fromUtf8(QJsonDocument(output).toJson(QJsonDocument::Compact))});
            for(int i=0;i<entries.size();++i){const auto& e=entries[i];for(const auto* snapshot:{&e.before,&e.after})if(snapshot->exists){
                    query(m_db,"INSERT OR IGNORE INTO snapshots VALUES(?,?)",{snapshot->hash(),snapshot->bytes});
                    QSqlQuery verified(m_db);verified.prepare("SELECT bytes FROM snapshots WHERE hash=?");verified.addBindValue(snapshot->hash());if(!verified.exec()||!verified.next()||verified.value(0).toByteArray()!=snapshot->bytes)fail("Existing durable snapshot is corrupt");
                }
                query(m_db,"INSERT INTO entries(nonce,ordinal,logical,root,root_id,before_exists,before_hash,before_id,after_exists,after_hash,after_id,backup,stage) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",{request.nonce,i,e.path.logical,e.path.root,e.path.rootId,e.before.exists,e.before.hash(),e.before.id,e.after.exists,e.after.hash(),"",e.backupName,e.stageName});}
            if(!m_db.commit())fail(m_db.lastError().text());
            prepared=true;
        }catch(...){m_db.rollback();throw;}
        boundary("after-prepare",-1);QString failure;if(!status(request.nonce,"applying",{},&failure))fail(failure);
        for(int i=0;i<entries.size();++i){auto& e=entries[i];boundary("before-entry",i);
            if(same(e.before,e.after,false)){e.after.id=e.before.id;query(m_db,"UPDATE entries SET after_id=? WHERE nonce=? AND ordinal=?",{e.after.id,request.nonce,i});continue;}
            if(e.after.exists){
                e.stage=createEmpty(e.path,e.stageName);e.after.id=identity(raw(e.stage));
                query(m_db,"UPDATE entries SET after_id=? WHERE nonce=? AND ordinal=?",{e.after.id,request.nonce,i});boundary("after-stage-created",i);
                writeOpened(e.stage,e.after.bytes,[&]{boundary("after-stage-chunk",i);});boundary("after-stage",i);
            }
            if(e.before.exists){renameTo(e.path.file,e.path,e.backupName);e.backup=std::move(e.path.file);boundary("after-backup",i);}
            if(e.after.exists){renameTo(e.stage,e.path,e.path.leaf);e.path.file=std::move(e.stage);boundary("after-publish",i);}
        }
        boundary("before-commit",-1);if(!status(request.nonce,"committed",{},&failure))fail(failure);committed=true;boundary("after-commit",-1);
        if(!cleanup(entries,&failure)){result.success=true;result.output=output;result.error="Committed; staging cleanup needs recovery: "+failure;return result;}
        query(m_db,"UPDATE mutations SET cleaned=1 WHERE nonce=?",{request.nonce});
        result.success=true;result.output=output;return result;
    }catch(const QString& interrupted){result.recoveryRequired=prepared;result.error=interrupted;return result;}
    catch(const Failure& error){result.error=error.text;
        if(committed){result.success=true;result.output=output;return result;}
        if(prepared){QString failure;for(auto& e:entries){e.path.file.reset();e.backup.reset();e.stage.reset();}
            if(!rollback(request.nonce,entries,&failure)){result.recoveryRequired=true;result.error+="; "+failure;status(request.nonce,"recovery_required",result.error,nullptr);}}
        return result;
    }
}
bool FileMutationStore::cleanup(QList<MutationEntry>& entries,QString* error){
    try{for(auto& e:entries){e.path.file.reset();e.backup.reset();e.stage.reset();e.restore.reset();
        auto backup=openFile(e.path.parentPath+'/'+e.backupName,true);
        if(backup){if(!same(state(raw(backup)),e.before))fail("Backup cleanup detected changed content/identity");removeOpened(backup);}
        for(const auto& pair:QList<QPair<QString,QString>>{{e.stageName,e.after.id},{e.stageName+"-restore",e.restoreId}}){
            auto file=openFile(e.path.parentPath+'/'+pair.first,true);if(!file)continue;
            const auto actual=state(raw(file));if(pair.second.isEmpty()||actual.id!=pair.second)fail("Staging cleanup detected an unowned identity");
            removeOpened(file); // An identity-journaled reserved stage may be partial.
        }}return true;
    }catch(const Failure& failure){if(error)*error=failure.text;return false;}
}
bool FileMutationStore::rollback(const QString& nonce,QList<MutationEntry>& entries,QString* error){
    try{
        const auto beforeMatches=[](const State& now,const MutationEntry& e){return same(now,e.before)||(now.exists&&e.before.exists&&!e.restoreId.isEmpty()&&now.id==e.restoreId&&now.bytes==e.before.bytes);};
        const auto checkpoint=[&](const QString& at,int i){if(m_fault&&m_fault(at,i)!=Fault::None)fail("Rollback interrupted at "+at);};
        QList<ScopedPath> current;
        for(auto& e:entries){auto path=scoped(e.path.logical,e.path.root,e.path.rootId,true);
            auto backup=openFile(path.parentPath+'/'+e.backupName,true);if(backup&&!same(state(raw(backup)),e.before))fail("Rollback backup changed");
            if(!beforeMatches(path.state,e)&&!same(path.state,e.after)&&path.state.exists)fail("Rollback conflict: current file was changed externally");
            e.restore=openFile(path.parentPath+'/'+e.stageName+"-restore",true,true);
            if(e.restore&&(e.restoreId.isEmpty()||state(raw(e.restore)).id!=e.restoreId))fail("Rollback restore staging identity changed");
            e.backup=std::move(backup);current.append(std::move(path));
        }
        QString failure;if(!status(nonce,"rolling_back",{},&failure))fail(failure);
        for(int i=entries.size()-1;i>=0;--i){auto& e=entries[i];auto& now=current[i];
            if(!beforeMatches(now.state,e)){
                if(now.file)removeOpened(now.file);
                if(e.before.exists){
                    if(e.backup){renameTo(e.backup,now,now.leaf);e.backup.reset();}
                    else{
                        if(!e.restore){e.restore=createEmpty(now,e.stageName+"-restore");e.restoreId=identity(raw(e.restore));query(m_db,"UPDATE entries SET restore_id=? WHERE nonce=? AND ordinal=?",{e.restoreId,nonce,i});}
                        checkpoint("rollback-after-restore-created",i);
                        writeOpened(e.restore,e.before.bytes,[&]{checkpoint("rollback-after-restore-chunk",i);});
                        renameTo(e.restore,now,now.leaf);e.restore.reset();checkpoint("rollback-after-restore",i);
                    }
                }
            }
            now.file.reset();e.path.file.reset();
        }
        if(!cleanup(entries,&failure))fail(failure);
        if(!status(nonce,"rolled_back",{},&failure))fail(failure);
        return true;
    }catch(const Failure& e){if(error)*error=e.text;return false;}
}
bool FileMutationStore::load(const QString& nonce,QString* agent,QString* phase,QList<MutationEntry>* entries,QString* error){
    try{if(!m_db.isOpen())fail("Mutation journal is not open");if(!validToken(nonce))fail("Invalid stored mutation identity");QSqlQuery mutation(m_db);mutation.prepare("SELECT agent,state FROM mutations WHERE nonce=? AND agent=?");mutation.addBindValue(nonce);mutation.addBindValue(m_agentId);if(!mutation.exec()||!mutation.next())fail("Mutation not found");*agent=mutation.value(0).toString();*phase=mutation.value(1).toString();
        QSqlQuery rows(m_db);rows.prepare("SELECT logical,root,root_id,before_exists,before_hash,before_id,after_exists,after_hash,after_id,backup,stage,restore_id FROM entries WHERE nonce=? ORDER BY ordinal");rows.addBindValue(nonce);if(!rows.exec())fail(rows.lastError().text());
        while(rows.next()){if(entries->size()>=2)fail("Single-file mutation has too many entries");MutationEntry e;e.path.logical=rows.value(0).toString();e.path.root=rows.value(1).toString();e.path.rootId=rows.value(2).toString();const auto mount=m_mounts.value(e.path.logical.section('/',0,0));if(!mount.writable||mount.root.compare(e.path.root,Qt::CaseInsensitive)!=0||mount.identity!=e.path.rootId)fail("Original mutation mount is unavailable or changed");
            e.path=scoped(e.path.logical,e.path.root,e.path.rootId,true);e.path.file.reset();
            const auto snapshot=[&](bool exists,const QString& hash,const QString& id){State result;result.exists=exists;result.id=id;if(!exists){if(!hash.isEmpty())fail("Missing snapshot has content hash");return result;}QSqlQuery q(m_db);q.prepare("SELECT bytes FROM snapshots WHERE hash=?");q.addBindValue(hash);if(!q.exec()||!q.next())fail("Snapshot missing");result.bytes=q.value(0).toByteArray();if(result.bytes.size()>MaxBytes||sha(result.bytes)!=hash)fail("Snapshot integrity check failed");return result;};
            e.before=snapshot(rows.value(3).toBool(),rows.value(4).toString(),rows.value(5).toString());e.after=snapshot(rows.value(6).toBool(),rows.value(7).toString(),rows.value(8).toString());
            e.backupName=rows.value(9).toString();e.stageName=rows.value(10).toString();e.restoreId=rows.value(11).toString();const int i=entries->size();if(e.backupName!=QString(".ade-mutation-%1-%2.backup").arg(nonce).arg(i)||e.stageName!=QString(".ade-mutation-%1-%2.stage").arg(nonce).arg(i))fail("Invalid mutation staging metadata");entries->append(std::move(e));
        }if(entries->isEmpty()||entries->size()>2)fail("Invalid single-file mutation entry count");return true;
    }catch(const Failure& e){if(error)*error=e.text;return false;}
}
Result FileMutationStore::undo(const QString& agentId,const QString& original,const QString& nonce,const std::atomic_bool* cancelled){
    Result result;result.mutationId=nonce;std::unique_ptr<QLockFile> owner;try{if(!m_db.isOpen())fail("Mutation journal is not open");owner=ownerLock(m_directory,m_agentId);}catch(const Failure& e){result.error=e.text;return result;}if(agentId!=m_agentId){result.error="Undo agent does not match journal owner";return result;}QString agent,phase,error;QList<MutationEntry> entries;if(!validToken(nonce)||!load(original,&agent,&phase,&entries,&error)){result.error=error.isEmpty()?"Invalid undo identity":error;return result;}
    if(agent!=agentId||phase!="committed"){result.error="Undo requires a committed mutation owned by this agent";return result;}
    try{for(auto& e:entries){e.path=scoped(e.path.logical,e.path.root,e.path.rootId,true);if(!same(e.path.state,e.after))fail("Undo conflict: file changed after the selected mutation");std::swap(e.before,e.after);e.after.id.clear();}
        Request request;request.agentId=agentId;request.nonce=nonce;request.toolCallId="undo-"+original;request.tool="undo";return apply(request,std::move(entries),{{"message","Restored state before mutation "+original}},original,cancelled);
    }catch(const Failure& e){result.error=e.text;return result;}
}
QList<Result> FileMutationStore::recover(){
    QList<Result> results;std::unique_ptr<QLockFile> owner;try{if(!m_db.isOpen())fail("Mutation journal is not open");owner=ownerLock(m_directory,m_agentId);}catch(const Failure& e){return {{false,true,{},e.text,{}}};}QSqlQuery rows(m_db);rows.prepare("SELECT nonce FROM mutations WHERE agent=? AND (state IN ('prepared','applying','rolling_back','recovery_required') OR (state='committed' AND cleaned=0)) ORDER BY rowid");rows.addBindValue(m_agentId);if(!rows.exec())return {{false,true,{},rows.lastError().text(),{}}};QStringList ids;while(rows.next())ids.append(rows.value(0).toString());
    for(const auto& nonce:ids){Result result;result.mutationId=nonce;QString agent,phase,error;QList<MutationEntry> entries;
        if(!load(nonce,&agent,&phase,&entries,&error)){result.recoveryRequired=true;result.error=error;results.append(result);continue;}
        const bool okay=phase=="committed"?cleanup(entries,&error):rollback(nonce,entries,&error);result.success=okay;if(okay&&phase=="committed"){try{query(m_db,"UPDATE mutations SET cleaned=1 WHERE nonce=?",{nonce});}catch(const Failure& failure){error=failure.text;result.success=false;}}result.recoveryRequired=!result.success;result.error=error;if(!okay&&phase!="committed")status(nonce,"recovery_required",error,nullptr);results.append(result);
    }return results;
}
QJsonObject FileMutationStore::describe(const QString& nonce,QString* error){
    if(!m_db.isOpen()){if(error)*error="Mutation journal is not open";return {};}
    QSqlQuery query(m_db);query.prepare("SELECT agent,tool_call,tool,undo_of,state,error FROM mutations WHERE nonce=? AND agent=?");query.addBindValue(nonce);query.addBindValue(m_agentId);if(!query.exec()||!query.next()){if(error)*error="Mutation not found";return {};}
    return {{"agent",query.value(0).toString()},{"toolCallId",query.value(1).toString()},{"tool",query.value(2).toString()},{"undoOf",query.value(3).toString()},{"state",query.value(4).toString()},{"error",query.value(5).toString()}};
}
}
