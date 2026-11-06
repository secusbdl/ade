#include "FileMutationStore.h"
#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QSqlQuery>
#include <QSqlError>
#include <QStringDecoder>
#include <optional>
#include <vector>

namespace ade::mutations {
namespace {
constexpr qint64 MaximumSnapshot=4*1024*1024, MaximumCapture=64*1024*1024;
struct Failure {QString text;};
[[noreturn]] void fail(const QString& text){throw Failure{text};}
struct Snapshot {bool exists=false;QString hash;QByteArray bytes;};
struct File {QString path,root,rootId;Snapshot before,after;QStringList ids;};
bool token(const QString& text){return QRegularExpression("^[A-Za-z0-9_-]{1,128}$").match(text).hasMatch();}
bool logicalPath(const QString& text){
    if(text.size()>32768||text.contains(QRegularExpression("[\\x00-\\x1f\\x7f\\\\:]")))return false;
    const auto parts=text.split('/');if(parts.size()<2||parts.size()>34||!QStringList{"ws1","apps","att"}.contains(parts.first()))return false;
    for(const auto& part:parts)if(part.isEmpty()||part=="."||part=="..")return false;
    return true;
}
std::optional<QString> text(const QByteArray& bytes){
    if(bytes.size()>2*1024*1024||bytes.contains('\0'))return {};
    QStringDecoder decoder(QStringDecoder::Utf8,QStringConverter::Flag::Stateless|QStringConverter::Flag::ConvertInitialBom);
    const QString decoded=decoder(bytes);if(decoder.hasError())return {};return decoded;
}
// Myers shortest edit path over complete lines (including final-newline
// identity). Work is bounded; exhaustion is an explicit unknown count.
std::optional<QPair<int,int>> counts(const QString& before,const QString& after){
    auto lines=[](const QString& value){QStringList result;qsizetype from=0;while(from<value.size()){const auto end=value.indexOf('\n',from);if(end<0){result.append(value.mid(from));break;}result.append(value.mid(from,end-from+1));from=end+1;}return result;};
    const auto a=lines(before),b=lines(after);if(a.size()+b.size()>200000)return {};
    int prefix=0;while(prefix<a.size()&&prefix<b.size()&&a[prefix]==b[prefix])++prefix;
    int n=int(a.size())-prefix,m=int(b.size())-prefix;
    while(n>0&&m>0&&a[prefix+n-1]==b[prefix+m-1]){--n;--m;}
    if(!n||!m)return QPair<int,int>{m,n};
    const int maximum=n+m,offset=maximum+1;std::vector<int> frontier(2*maximum+3,-1);frontier[offset+1]=0;
    qint64 work=0;
    for(int d=0;d<=maximum;++d)for(int k=-d;k<=d;k+=2){
        if(++work>8000000)return {};
        int x=k==-d||(k!=d&&frontier[offset+k-1]<frontier[offset+k+1])?frontier[offset+k+1]:frontier[offset+k-1]+1;
        int y=x-k;while(x<n&&y<m&&a[prefix+x]==b[prefix+y]){++x;++y;if(++work>8000000)return {};}
        frontier[offset+k]=x;if(x>=n&&y>=m)return QPair<int,int>{(d+m-n)/2,(d+n-m)/2};
    }
    return {};
}
}

QJsonObject FileMutationStore::turnFiles(const QStringList& requested,const QString& selectedPath,QString* error){
    if(error)error->clear();
    bool transaction=false;
    try{
        if(!m_db.isOpen()||requested.isEmpty()||requested.size()>100)fail("Saved changes require one to 100 mutation IDs and an open journal.");
        QSet<QString> wanted;QStringList placeholders;
        for(const auto& id:requested){if(!token(id)||wanted.contains(id))fail("Saved changes contain invalid or repeated mutation IDs.");wanted.insert(id);placeholders.append("?");}
        if(!selectedPath.isEmpty()&&!logicalPath(selectedPath))fail("Saved diff path is invalid.");
        // A single SQLite read transaction pins the chosen records and blobs
        // against concurrent cleanup. No workspace files are read or opened.
        if(!m_db.transaction())fail("Cannot start a consistent saved-change read.");
        transaction=true;
        QSqlQuery mutations(m_db);
        mutations.prepare("SELECT nonce,state,tool,created_at,cleaned FROM mutations WHERE agent=? AND nonce IN ("+placeholders.join(',')+") ORDER BY rowid");
        mutations.addBindValue(m_agentId);for(const auto& id:requested)mutations.addBindValue(id);
        if(!mutations.exec())fail("Cannot read the selected saved changes.");
        QStringList ordered;QJsonArray records;while(mutations.next()){if(mutations.value(1).toString()!="committed")fail("Selected changes are not committed; recover them before opening a saved diff.");ordered.append(mutations.value(0).toString());records.append(QJsonObject{{"mutationId",mutations.value(0).toString()},{"state","committed"},{"tool",mutations.value(2).toString()},{"createdAt",mutations.value(3).toLongLong()},{"recoveryRequired",!mutations.value(4).toBool()}});}
        if(ordered.size()!=wanted.size())fail("Saved changes are unavailable for this chat. Inherited records do not grant access to another chat's journal.");
        mutations.finish();QList<File> files;QHash<QString,int> indexes;QHash<QString,QPair<bool,bool>> directories;int entryCount=0;
        for(const auto& id:ordered){
            QJsonArray recordEntries;int recordDirectories=0;
            QSqlQuery entries(m_db);entries.prepare("SELECT logical,root,root_id,before_exists,before_hash,after_exists,after_hash FROM entries WHERE nonce=? ORDER BY ordinal");entries.addBindValue(id);
            if(!entries.exec())fail("Cannot read saved file identities.");
            while(entries.next()){
                if(++entryCount>20000)fail("Saved-change selection exceeds the entry limit.");
                const auto path=entries.value(0).toString(),root=entries.value(1).toString(),rootId=entries.value(2).toString();
                recordEntries.append(QJsonObject{{"path",path},{"absolutePath",QDir::cleanPath(root+'/'+path.section('/',1))},{"beforeExists",entries.value(3).toBool()},{"afterExists",entries.value(5).toBool()}});
                if(!logicalPath(path)||root.isEmpty()||rootId.isEmpty())fail("Saved file identity is invalid.");
                Snapshot before{entries.value(3).toBool(),entries.value(4).toString(),{}},after{entries.value(5).toBool(),entries.value(6).toString(),{}};
                const auto valid=[](const Snapshot& s){return s.exists?QRegularExpression("^[0-9a-f]{64}$").match(s.hash).hasMatch():s.hash.isEmpty();};
                if(!valid(before)||!valid(after))fail("Saved snapshot identity is invalid.");
                const auto key=path.toCaseFolded();
                if(indexes.contains(key)){auto& file=files[indexes.value(key)];if(file.root.compare(root,Qt::CaseInsensitive)||file.rootId!=rootId)fail("Selected changes span different workspace instances.");file.path=path;file.after=after;if(!file.ids.contains(id))file.ids.append(id);}
                else{if(files.size()>=1000)fail("Saved-change selection exceeds 1,000 files.");indexes.insert(key,files.size());files.append({path,root,rootId,before,after,{id}});}
            }
            QSqlQuery dirs(m_db);dirs.prepare("SELECT logical,root_id,before_exists,after_exists FROM directories WHERE nonce=? ORDER BY ordinal");dirs.addBindValue(id);
            if(!dirs.exec())fail("Cannot read saved directory identities.");
            while(dirs.next()){++recordDirectories;const auto key=dirs.value(0).toString().toCaseFolded()+':'+dirs.value(1).toString();if(directories.size()>=25600)fail("Saved directory selection exceeds its limit.");if(directories.contains(key))directories[key].second=dirs.value(3).toBool();else directories.insert(key,{dirs.value(2).toBool(),dirs.value(3).toBool()});}
            const auto index=ordered.indexOf(id);auto record=records[index].toObject();record["entries"]=recordEntries;record["directoryCount"]=recordDirectories;records[index]=record;
        }
        qint64 captured=0;auto load=[&](Snapshot& snapshot){
            if(!snapshot.exists)return;
            QSqlQuery size(m_db);size.prepare("SELECT length(bytes) FROM snapshots WHERE hash=?");size.addBindValue(snapshot.hash);
            if(!size.exec()||!size.next()||size.value(0).toLongLong()<0||size.value(0).toLongLong()>MaximumSnapshot||captured+size.value(0).toLongLong()>MaximumCapture)fail("Saved snapshot is missing or exceeds the bounded read limit.");
            QSqlQuery blob(m_db);blob.prepare("SELECT bytes FROM snapshots WHERE hash=?");blob.addBindValue(snapshot.hash);if(!blob.exec()||!blob.next())fail("Saved snapshot is missing.");snapshot.bytes=blob.value(0).toByteArray();captured+=snapshot.bytes.size();
            if(snapshot.bytes.size()>MaximumSnapshot||captured>MaximumCapture||QString::fromLatin1(QCryptographicHash::hash(snapshot.bytes,QCryptographicHash::Sha256).toHex())!=snapshot.hash)fail("Saved snapshot integrity check failed.");
        };
        QJsonArray resultFiles;QJsonObject preview;bool found=selectedPath.isEmpty();int directoryCount=0;for(const auto& state:directories)if(state.first!=state.second)++directoryCount;
        for(auto& file:files){
            // Even equal endpoints must have verifiable originals; imported or
            // damaged records must not masquerade as a verified empty diff.
            load(file.before);load(file.after);
            if(file.before.exists==file.after.exists&&file.before.hash==file.after.hash)continue;
            const auto before=text(file.before.bytes),after=text(file.after.bytes);
            const auto stats=before&&after?counts(*before,*after):std::nullopt;
            const QString kind=!file.before.exists?"created":!file.after.exists?"deleted":"modified";
            QJsonObject row{{"path",file.path},{"mutationIds",QJsonArray::fromStringList(file.ids)},{"added",stats?stats->first:0},{"removed",stats?stats->second:0},{"statsKnown",bool(stats)}};
            if(!stats){row["changeType"]=kind;row["countReason"]=before&&after?"Line count exceeds the bounded comparison limit.":"Binary or large file; line counts are unavailable.";}
            resultFiles.append(row);
            if(file.path==selectedPath){found=true;
                if(before&&after&&before->size()+after->size()<=128*1024&&file.before.bytes.size()+file.after.bytes.size()<=512*1024)
                    preview={{"version",1},{"files",QJsonArray{QJsonObject{{"path",file.path},{"before",file.before.exists?QJsonValue(*before):QJsonValue(QJsonValue::Null)},{"after",file.after.exists?QJsonValue(*after):QJsonValue(QJsonValue::Null)}}}},{"omitted",0}};
                else preview={{"unavailable","This saved change is binary or too large for the bounded text preview. Its exact bytes remain in the journal."}};
            }
        }
        if(!found)fail("The selected path is not a net changed file in this turn.");
        if(!m_db.commit())fail("Cannot finish the saved-change read.");
        transaction=false;
        QJsonObject result{{"version",1},{"agentId",m_agentId},{"mutationIds",QJsonArray::fromStringList(ordered)},{"files",resultFiles},{"directoryCount",directoryCount},{"records",records}};
        if(!selectedPath.isEmpty())result["preview"]=preview;
        return result;
    }catch(const Failure& failure){if(transaction)m_db.rollback();if(error)*error=failure.text;return {};}
}
QJsonArray FileMutationStore::settledInvocations(const QStringList& requested,QString* error){
    if(error)error->clear();
    try{
        if(!m_db.isOpen()||requested.size()>256)fail("Invocation reconciliation requires an open journal and at most 256 IDs.");
        if(requested.isEmpty())return {};
        QSet<QString> seen;QStringList placeholders;for(const auto& id:requested){if(!token(id)||seen.contains(id))fail("Invocation selection is invalid or repeated.");seen.insert(id);placeholders.append("?");}
        QSqlQuery rows(m_db);rows.prepare("SELECT invocation_id,nonce,state,cleaned,length(output),output,length(error),error FROM mutations WHERE agent=? AND invocation_id IN ("+placeholders.join(',')+") ORDER BY rowid");rows.addBindValue(m_agentId);for(const auto& id:requested)rows.addBindValue(id);if(!rows.exec())fail("Cannot reconcile saved mutation invocations.");
        QJsonArray result;qint64 total=0;
        while(rows.next()){
            const auto state=rows.value(2).toString();if(!QStringList{"committed","rolled_back","recovery_required"}.contains(state))continue;
            const auto outputSize=rows.value(4).toLongLong(),errorSize=rows.value(6).toLongLong();if(outputSize<0||errorSize<0||outputSize>128*1024||errorSize>128*1024||(total+=outputSize+errorSize)>16*1024*1024)fail("Saved invocation result exceeds its bounded metadata limit.");
            QJsonParseError parse;const auto output=QJsonDocument::fromJson(rows.value(5).toByteArray(),&parse);if(parse.error!=QJsonParseError::NoError||!output.isObject())fail("Saved invocation result is invalid.");
            result.append(QJsonObject{{"invocationId",rows.value(0).toString()},{"mutationId",rows.value(1).toString()},{"agentId",m_agentId},{"committed",state=="committed"},{"recoveryRequired",state=="recovery_required"||(state=="committed"&&!rows.value(3).toBool())},{"state",state},{"output",output.object()},{"error",rows.value(7).toString()}});
        }
        return result;
    }catch(const Failure& failure){if(error)*error=failure.text;return {};}
}
} // namespace ade::mutations
