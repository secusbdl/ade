#include "ToolExecutor.h"
#include "ScopedDirectory.h"
#include "SearchRules.h"
#include "attachments/FileTransformer.h"

#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QTimeZone>
#include <QUuid>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <vector>
#include <functional>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace ade {
namespace {
constexpr qint64 MaxFileBytes = 4 * 1024 * 1024;
constexpr qsizetype MaxOutputChars = 64 * 1024;
QString capped(const QString &value) {
    return value.size() <= MaxOutputChars ? value
        : value.left(MaxOutputChars) + QStringLiteral("\n[Output truncated at 64 Ki characters]");
}
bool within(const QString &root, const QString &path) {
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return path.compare(root, sensitivity) == 0
        || path.startsWith(root.endsWith('/') ? root : root + '/', sensitivity);
}
#ifdef Q_OS_WIN
QString pathFromHandle(HANDLE handle) {
    const DWORD size=GetFinalPathNameByHandleW(handle,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!size)return {};
    std::vector<wchar_t> buffer(size+1);
    const DWORD written=GetFinalPathNameByHandleW(handle,buffer.data(),static_cast<DWORD>(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!written||written>=buffer.size())return {};
    QString result=QString::fromWCharArray(buffer.data(),written);
    if(result.startsWith(QStringLiteral("\\\\?\\UNC\\")))result=QStringLiteral("\\\\")+result.mid(8);
    else if(result.startsWith(QStringLiteral("\\\\?\\")))result.remove(0,4);
    return QDir::cleanPath(QDir::fromNativeSeparators(result));
}
#endif
QString resolvedExistingPath(const QString &path) {
#ifdef Q_OS_WIN
    const auto native=QDir::toNativeSeparators(path).toStdWString();
    HANDLE handle=CreateFileW(native.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return {};
    const auto result=pathFromHandle(handle);CloseHandle(handle);return result;
#else
    return QFileInfo(path).canonicalFilePath();
#endif
}
bool readBoundedFile(const QString& path,const QString& root,QByteArray* bytes,QString* error,qint64 maximum=MaxFileBytes,QDateTime* modified=nullptr,const std::atomic_bool* cancelled=nullptr,qint64* remainingReadBytes=nullptr) {
    bytes->clear();
    if(cancelled&&cancelled->load()){*error="File read cancelled.";return false;}
    if(modified)*modified={};
#ifdef Q_OS_WIN
    const auto native=QDir::toNativeSeparators(path).toStdWString();
    HANDLE handle=CreateFileW(native.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(handle==INVALID_HANDLE_VALUE){*error="Cannot open file for reading.";return false;}
    const auto close=qScopeGuard([&]{CloseHandle(handle);});
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
    if(!GetFileInformationByHandle(handle,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||!GetFileSizeEx(handle,&size)){*error="Not a readable regular file.";return false;}
    const auto resolved=pathFromHandle(handle);
    if(resolved.isEmpty()||!within(root,resolved)){*error="Opened file is outside the selected mount.";return false;}
    if(modified){ULARGE_INTEGER ticks{};ticks.LowPart=info.ftLastWriteTime.dwLowDateTime;ticks.HighPart=info.ftLastWriteTime.dwHighDateTime;*modified=QDateTime::fromMSecsSinceEpoch(qint64(ticks.QuadPart/10000)-11644473600000LL,QTimeZone::UTC);}
    if(size.QuadPart<0||size.QuadPart>maximum){*error=maximum==MaxFileBytes?QStringLiteral("File exceeds the 4 MiB read limit."):QStringLiteral("File exceeds the bounded read limit.");return false;}
    QByteArray chunk(65536,Qt::Uninitialized);
    while(true){
        if(cancelled&&cancelled->load()){*error="File read cancelled.";return false;}
        const auto request = remainingReadBytes ? qMin<qint64>(chunk.size(), qMax<qint64>(0, *remainingReadBytes)) : chunk.size();
        if (!request) {
            LARGE_INTEGER currentSize{};
            if (GetFileSizeEx(handle, &currentSize) && currentSize.QuadPart == bytes->size()) return true;
            *error = "Search read budget exhausted.";
            return false;
        }
        DWORD count=0;
        const bool read = ReadFile(handle,chunk.data(),static_cast<DWORD>(request),&count,nullptr);
        if (remainingReadBytes) *remainingReadBytes -= count;
        if(!read){*error="Cannot read file bytes.";return false;}
        if(!count)break;
        if(bytes->size()+count>maximum){*error="File grew beyond the bounded read limit.";return false;}
        bytes->append(chunk.constData(),count);
    }
#else
    const auto resolved=QFileInfo(path).canonicalFilePath();QFile file(path);
    if(resolved.isEmpty()||!within(root,resolved)||!file.open(QIODevice::ReadOnly)){*error="Cannot open scoped file.";return false;}
    if(modified)*modified=QFileInfo(file).lastModified().toUTC();
    *bytes=file.read(remainingReadBytes ? qMin(maximum+1,qMax<qint64>(0,*remainingReadBytes)) : maximum+1);
    if(remainingReadBytes)*remainingReadBytes-=bytes->size();
    if(bytes->size()>maximum||!file.atEnd()){*error="File exceeds the bounded read limit.";return false;}
#endif
    return true;
}
// Retain every ancestor handle until its descendants have been captured.
// Only the explicitly selected root may follow an in-mount directory alias.
bool captureDirectory(const QString& path,const QString& root,int level,double depth,
                      int& count,QList<FileTreeEntry>* entries,QString* error,QDateTime* modified=nullptr,const std::atomic_bool* cancelled=nullptr) {
    ScopedDirectorySnapshot snapshot;
    if(!readScopedDirectory(path,root,&snapshot,error,10000-count,cancelled,level==0))return false;
    count+=snapshot.entries.size();
    if(modified)*modified=snapshot.modified;
    for(const auto& child:snapshot.entries){
        if(cancelled&&cancelled->load()){*error="Directory read cancelled.";return false;}
        FileTreeEntry entry;entry.name=child.name;entry.isDirectory=child.isDirectory&&!child.isReparsePoint;
        if(entry.isDirectory&&level<depth&&!captureDirectory(child.absolutePath,root,level+1,depth,count,&entry.children,error,nullptr,cancelled))return false;
        entries->append(std::move(entry));
    }
    return true;
}
enum class DirectoryVisit { Continue,SkipChildren,Stop,Error };
// Iterative traversal keeps stack use bounded even for very deep search trees.
// Captured names (including unvisited siblings) count against the total budget.
bool walkScopedDirectory(const QString& path,const QString& root,int limit,
                         const std::function<DirectoryVisit(const ScopedDirectoryEntry&,const QString&)>& visit,
                         QString* error,bool* truncated=nullptr,int maximumDepth=-1){
    struct Frame {ScopedDirectorySnapshot snapshot;QString prefix;qsizetype next=0;};
    QList<Frame> frames;int captured=0;
    const auto open=[&](const QString& directory,const QString& prefix,bool rootAlias){
        Frame frame;frame.prefix=prefix;bool partial=false;
        if(!readScopedDirectory(directory,root,&frame.snapshot,error,limit-captured,nullptr,rootAlias,truncated?&partial:nullptr))return false;
        captured+=frame.snapshot.entries.size();if(partial)*truncated=true;
        frames.append(std::move(frame));return true;
    };
    if(!open(path,{},true))return false;
    while(!frames.isEmpty()){
        auto& frame=frames.last();
        if(frame.next>=frame.snapshot.entries.size()){frames.removeLast();continue;}
        const auto child=frame.snapshot.entries.at(frame.next++);
        const auto relative=frame.prefix+child.name;
        const auto action=visit(child,relative);
        if(action==DirectoryVisit::Error)return false;
        if(action==DirectoryVisit::Stop)return true;
        if(action==DirectoryVisit::Continue&&child.isDirectory&&!child.isReparsePoint){
            if(truncated&&(captured>=limit||(maximumDepth>=0&&relative.count('/')>=maximumDepth))){
                *truncated=true;continue;
            }
            if(!open(child.absolutePath,relative+'/',false))return false;
        }
    }
    return true;
}
// Ignore files are read only inside the selected mount, never through links,
// with their own bounds. A rejected file contributes no rules (fail-open) and
// is disclosed, because hiding whole trees silently would be worse.
struct IgnoreLoadState {
    int files=0,rules=0;
    qint64 bytes=0;
    bool capped=false;
    QStringList diagnostics;
    int omittedDiagnostics=0;
    void note(const QString& text){if(diagnostics.size()<5)diagnostics.append(text);else ++omittedDiagnostics;}
};
constexpr qint64 MaxIgnoreFileBytes=256*1024,MaxIgnoreTotalBytes=4*1024*1024;
constexpr int MaxIgnoreFiles=2000,MaxIgnoreRules=100000;
QString shownSearchPath(const QString& path){return path.size()<=160?path:QStringLiteral("...")+path.right(157);}
bool loadIgnoreFile(const QString& directory,const QString& relativeDirectory,search::IgnoreKind kind,const QString& root,
                    search::RuleFile* out,IgnoreLoadState* state){
    if(state->capped)return false;
    const auto name=search::ignoreFileName(kind);
    const auto path=directory+'/'+name;
    const auto shown=shownSearchPath(relativeDirectory.isEmpty()?name:relativeDirectory+'/'+name);
    const auto reject=[&](const QString& reason){state->note(QStringLiteral("[Ignore file %1 not applied: %2.]").arg(shown,reason));return false;};
    QByteArray bytes;
#ifdef Q_OS_WIN
    const auto native=QDir::toNativeSeparators(path).toStdWString();
    HANDLE handle=CreateFileW(native.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(handle==INVALID_HANDLE_VALUE){
        const DWORD code=GetLastError();
        if(code==ERROR_FILE_NOT_FOUND||code==ERROR_PATH_NOT_FOUND)return false;
        const DWORD attributes=GetFileAttributesW(native.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY))return reject(QStringLiteral("not a regular file"));
        return reject(QStringLiteral("unreadable, Windows error %1").arg(code));
    }
    const auto close=qScopeGuard([&]{CloseHandle(handle);});
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
    if(!GetFileInformationByHandle(handle,&info)||!GetFileSizeEx(handle,&size))return reject(QStringLiteral("unreadable"));
    if(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)return reject(QStringLiteral("links are not followed"));
    if(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)return reject(QStringLiteral("not a regular file"));
    const auto resolved=pathFromHandle(handle);
    if(resolved.isEmpty()||!within(root,resolved))return reject(QStringLiteral("outside the selected mount"));
    if(size.QuadPart>MaxIgnoreFileBytes)return reject(QStringLiteral("exceeds 256 KiB"));
    if(state->bytes+size.QuadPart>MaxIgnoreTotalBytes){
        state->capped=true;state->note(QStringLiteral("[Further ignore files not applied: 4 MiB total ignore-file limit reached.]"));return false;
    }
    QByteArray chunk(65536,Qt::Uninitialized);
    while(true){
        DWORD count=0;
        if(!ReadFile(handle,chunk.data(),static_cast<DWORD>(chunk.size()),&count,nullptr))return reject(QStringLiteral("unreadable"));
        if(!count)break;
        if(bytes.size()+count>MaxIgnoreFileBytes)return reject(QStringLiteral("grew beyond 256 KiB"));
        bytes.append(chunk.constData(),count);
    }
#else
    const QFileInfo info(path);
    if(!info.exists()&&!info.isSymLink())return false;
    if(info.isSymLink()||info.isJunction())return reject(QStringLiteral("links are not followed"));
    if(!info.isFile())return reject(QStringLiteral("not a regular file"));
    if(!within(root,info.canonicalFilePath()))return reject(QStringLiteral("outside the selected mount"));
    if(info.size()>MaxIgnoreFileBytes)return reject(QStringLiteral("exceeds 256 KiB"));
    if(state->bytes+info.size()>MaxIgnoreTotalBytes){
        state->capped=true;state->note(QStringLiteral("[Further ignore files not applied: 4 MiB total ignore-file limit reached.]"));return false;
    }
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly))return reject(QStringLiteral("unreadable"));
    bytes=file.read(MaxIgnoreFileBytes+1);
    if(bytes.size()>MaxIgnoreFileBytes)return reject(QStringLiteral("grew beyond 256 KiB"));
#endif
    state->bytes+=bytes.size();
    if(++state->files>MaxIgnoreFiles){
        state->capped=true;state->note(QStringLiteral("[Further ignore files not applied: %1-file limit reached.]").arg(MaxIgnoreFiles));return false;
    }
    out->rules.clear();
    // Like rg: lines end at LF (one preceding CR is dropped), a leading UTF-8
    // BOM is ignored, a malformed glob skips only its line, and invalid UTF-8
    // stops the file while earlier lines stay applied.
    const auto lines=bytes.split('\n');
    for(qsizetype i=0;i<lines.size();++i){
        auto line=lines[i];
        if(line.endsWith('\r'))line.chop(1);
        QStringDecoder decoder(QStringDecoder::Utf8,QStringConverter::Flag::ConvertInitialBom|QStringConverter::Flag::Stateless);
        QString text=decoder(line);
        if(decoder.hasError()){
            state->note(QStringLiteral("[Ignore file %1: line %2 is not valid UTF-8; it and later lines were not applied.]").arg(shown,QString::number(i+1)));
            break;
        }
        if(i==0)while(text.startsWith(QChar(0xfeff)))text.remove(0,1);
        search::Rule rule;QString problem;
        if(search::compileRule(text,search::Syntax::IgnoreFile,int(i+1),&rule,&problem)){
            if(++state->rules>MaxIgnoreRules){
                state->capped=true;state->note(QStringLiteral("[Further ignore rules not applied: %1-rule limit reached.]").arg(MaxIgnoreRules));break;
            }
            out->rules.append(std::move(rule));
        }else if(!problem.isEmpty())state->note(QStringLiteral("[Ignore file %1 line %2 skipped: %3.]").arg(shown,QString::number(i+1),problem));
    }
    return true;
}
// Any .git entry marks a repository boundary, as in rg. The entry itself is
// inspected, never a link target, and it is never listed or entered.
bool hasGitEntry(const QString& directory){
#ifdef Q_OS_WIN
    const auto native=QDir::toNativeSeparators(directory+QStringLiteral("/.git")).toStdWString();
    return GetFileAttributesW(native.c_str())!=INVALID_FILE_ATTRIBUTES;
#else
    const QFileInfo info(directory+QStringLiteral("/.git"));
    return info.exists()||info.isSymLink();
#endif
}
search::IgnoreLevel loadIgnoreLevel(const QString& directory,const QString& relativeDirectory,const QString& root,IgnoreLoadState* state){
    search::IgnoreLevel level;
    level.gitBoundary=hasGitEntry(directory);
    for(int kind=0;kind<search::IgnoreKindCount;++kind){
        level.files[kind].base=relativeDirectory;
        level.present[kind]=loadIgnoreFile(directory,relativeDirectory,search::IgnoreKind(kind),root,&level.files[kind],state);
    }
    return level;
}
struct SearchScope {
    search::Policy policy;
    search::OverrideSet overrides;
};
constexpr int SearchEntryLimit=10000,SearchDepthLimit=64;
constexpr qint64 SearchScanLimit=200000;
// Like walkScopedDirectory, but ignored, excluded and hidden names are dropped
// before they consume the 10000-entry budget. Every enumerated name still
// counts toward a 200000-name scan bound. Pruned directories are never opened,
// so their contents and ignore files are never read. The start directory is
// never pruned; ignore files between the mount root and it still apply.
// Cancellation fails the walk; an expired deadline ends it early as a
// truncated (disclosed) result.
bool walkSearchTree(const QString& path,const QString& root,const SearchScope& scope,
                    const std::function<DirectoryVisit(const ScopedDirectoryEntry&,const QString&)>& visit,
                    QString* error,bool* truncated,IgnoreLoadState* ignoreState,
                    const std::atomic_bool* cancelled,const QDeadlineTimer& deadline){
    const bool respectIgnores=scope.policy.respectIgnores;
    std::atomic_bool halt{false};bool expired=false;
    const auto halted=[&]{
        if(halt.load(std::memory_order_relaxed))return true;
        if(cancelled&&cancelled->load(std::memory_order_relaxed)){halt.store(true,std::memory_order_relaxed);return true;}
        if(deadline.hasExpired()){expired=true;halt.store(true,std::memory_order_relaxed);return true;}
        return false;
    };
    const auto interrupted=[&]{
        if(expired){if(error)error->clear();*truncated=true;return true;}
        if(error)*error=QStringLiteral("Search cancelled.");
        return false;
    };
    if(halted())return interrupted();
    QString start=QDir(root).relativeFilePath(path);
    if(start=="."||start==".."||start.startsWith("../"))start.clear();
    search::IgnoreStack ignores;
    if(respectIgnores&&!start.isEmpty()){
        ignores.push(loadIgnoreLevel(root,{},root,ignoreState));
        const auto parts=start.split('/');QString ancestor;
        for(qsizetype i=0;i+1<parts.size();++i){
            ancestor+=(ancestor.isEmpty()?QString():QStringLiteral("/"))+parts[i];
            ignores.push(loadIgnoreLevel(root+'/'+ancestor,ancestor,root,ignoreState));
        }
    }
    struct Frame {ScopedDirectorySnapshot snapshot;QString prefix,mountDirectory;qsizetype next=0;};
    QList<Frame> frames;int captured=0;qint64 scanned=0;
    const auto open=[&](const QString& directory,const QString& prefix,const QString& mountDirectory,bool rootAlias){
        if(respectIgnores)ignores.push(loadIgnoreLevel(directory,mountDirectory,root,ignoreState));
        Frame frame;frame.prefix=prefix;frame.mountDirectory=mountDirectory;bool partial=false;
        ScopedDirectoryAdmission admission;admission.scanned=&scanned;admission.scanLimit=SearchScanLimit;
        admission.admit=[&](const ScopedDirectoryEntry& entry){
            // The halt flag is the enumeration's cancel flag, so an expired
            // deadline also ends a long single-directory read.
            if(halted())return false;
            const auto mountPath=mountDirectory.isEmpty()?entry.name:mountDirectory+'/'+entry.name;
            return search::admit(scope.policy,scope.overrides,ignores,entry.name,prefix+entry.name,mountPath,
                                 entry.isDirectory,entry.isReparsePoint,entry.isHidden);
        };
        if(!readScopedDirectory(directory,root,&frame.snapshot,error,SearchEntryLimit-captured,&halt,rootAlias,&partial,&admission)){
            if(respectIgnores)ignores.pop();
            return false;
        }
        captured+=frame.snapshot.entries.size();if(partial)*truncated=true;
        frames.append(std::move(frame));return true;
    };
    if(!open(path,{},start,true))return halt.load(std::memory_order_relaxed)?interrupted():false;
    while(!frames.isEmpty()){
        auto& frame=frames.last();
        if(frame.next>=frame.snapshot.entries.size()){
            frames.removeLast();
            if(respectIgnores)ignores.pop();
            continue;
        }
        if(halted())return interrupted();
        const auto child=frame.snapshot.entries.at(frame.next++);
        const auto relative=frame.prefix+child.name;
        const auto action=visit(child,relative);
        if(action==DirectoryVisit::Error)return false;
        if(action==DirectoryVisit::Stop)return true;
        if(action==DirectoryVisit::Continue&&child.isDirectory&&!child.isReparsePoint){
            if(captured>=SearchEntryLimit||scanned>=SearchScanLimit||relative.count('/')>=SearchDepthLimit){*truncated=true;continue;}
            const auto mountDirectory=frame.mountDirectory.isEmpty()?child.name:frame.mountDirectory+'/'+child.name;
            if(!open(child.absolutePath,relative+'/',mountDirectory,false))return halt.load(std::memory_order_relaxed)?interrupted():false;
        }
    }
    return true;
}
struct FormattedRead {QString output;bool error=false;QJsonObject metadata={};};
FormattedRead formattedMountedRead(const FileReadSource& source,const QString& name,const QString& path,
    const FileReadParams& params,const std::atomic_bool* cancelled,const FileReadCommitter& committer,const QString& inputHash){
    try{
        FileReadCapture capture;QString error;
        if(!source(name,path,params,&capture,cancelled,&error))return {error.isEmpty()?QStringLiteral("Mounted file capture failed."):error,true};
        if(cancelled&&cancelled->load())return {"Mounted file read canceled.",true};
        capture.tool=name;capture.params=params;capture.inputHash=inputHash;
        if(committer){
            const auto metadata=committer(capture,cancelled,&error);
            if(!error.isEmpty()||metadata.isEmpty())return {error.isEmpty()?QStringLiteral("The file capture could not be persisted."):error,true,{}};
            return {name=="ls"?QStringLiteral("Directory opened and loaded into context."):QStringLiteral("File opened and loaded into context."),false,metadata};
        }
        const auto transformed=capture.directory?FileTransformer::fromDirectory(capture.entries,capture.descriptor,params,{},cancelled)
            :FileTransformer::fromBytes(capture.bytes,capture.descriptor,params,{},cancelled);
        if(!transformed.error.isEmpty())return {transformed.error,true};
        QString output;for(const auto& part:FileTransformer::wrappedParts(capture.descriptor,transformed)){
            if(part.toObject()["type"]!="text")return {"This native tool transport cannot return non-text file parts yet.",true};
            output+=part.toObject()["text"].toString();
        }
        return {output,false};
    }catch(...){return {"The mounted file capture could not be completed.",true,{}};}
}
FormattedRead rawMountedRead(const FileReadSource& source,const QString& name,const QString& path,const std::atomic_bool* cancelled) {
    try {
        FileReadCapture capture;QString error;
        if(!source(name=="readDirectory"?"ls":"read",path,{},&capture,cancelled,&error))return {error.isEmpty()?QStringLiteral("Mounted file capture failed."):error,true};
        if(cancelled&&cancelled->load())return {"Mounted file read canceled.",true};
        if(name=="readDirectory"){
            if(!capture.directory)return {"The requested path is not a directory.",true};
            QJsonArray names;for(const auto& entry:capture.entries){if(entry.isDirectory||!entry.children.isEmpty())return {"The mounted log namespace must remain flat.",true};names.append(entry.name);}
            const auto output=QString::fromUtf8(QJsonDocument(names).toJson(QJsonDocument::Compact));
            return output.size()>65536?FormattedRead{"Directory listing exceeds64Ki characters; no partial result was returned.",true}:FormattedRead{output,false};
        }
        if(capture.directory)return {"The requested path is not a file.",true};
        if(capture.bytes.size()>4*1024*1024)return {"Raw file read exceeds4MiB; use formatted read with line ranges.",true};
        if(name=="readBytes")return {QString::fromLatin1(capture.bytes.toBase64()),false};
        QStringDecoder decoder(QStringDecoder::Utf8,QStringConverter::Flag::ConvertInitialBom|QStringConverter::Flag::Stateless);
        const QString output=decoder(capture.bytes);
        if(decoder.hasError())return {"File is not valid UTF-8; request Buffer bytes instead.",true};
        if(output.size()>65536)return {"Raw text exceeds64Ki characters; no partial result was returned.",true};
        return {output,false};
    }catch(...){return {"The raw mounted read could not be completed.",true};}
}
FormattedRead formattedRead(const QString& name,const QString& path,const QString& root,const QString& requested,
                            const FileReadParams& params,const std::atomic_bool* cancelled,const FileReadCommitter& committer,const QString& inputHash) {
    if(cancelled&&cancelled->load())return {"File read cancelled.",true};
    QString error;const QFileInfo info(path);FileDescriptor descriptor{requested,info.fileName(),-1,{}};FileTransformResult converted;
    FileReadCapture capture;capture.tool=name;capture.params=params;capture.inputHash=inputHash;
    if(info.isDir()){
        const double depth=params.preview.value_or(false)?0:params.depth.value_or(2);
        if(depth>32)return {"Requested directory depth exceeds the native limit.",true};
        QList<FileTreeEntry> entries;int count=0;
        if(!captureDirectory(path,root,0,depth,count,&entries,&error,&descriptor.modified,cancelled))return {error,true};
        if(committer){capture.directory=true;capture.entries=std::move(entries);capture.capturedDepth=depth;}
        else converted=FileTransformer::fromDirectory(entries,descriptor,params,{},cancelled);
    }else if(name=="ls")return {"Search path is not a directory.",true};
    else{
        QByteArray bytes;if(!readBoundedFile(path,root,&bytes,&error,MaxFileBytes,&descriptor.modified,cancelled))return {error,true};
        descriptor.size=bytes.size();if(committer)capture.bytes=std::move(bytes);
        else converted=FileTransformer::fromBytes(bytes,descriptor,params,{},cancelled);
    }
    if(committer){
        capture.descriptor=descriptor;
        try{
            const auto metadata=committer(capture,cancelled,&error);
            if(!error.isEmpty()||metadata.isEmpty())return {error.isEmpty()?QStringLiteral("The file capture could not be persisted."):error,true,{}};
            return {name=="ls"?QStringLiteral("Directory opened and loaded into context."):QStringLiteral("File opened and loaded into context."),false,metadata};
        }catch(...){return {"The file capture could not be persisted.",true,{}};}
    }
    if(!converted.error.isEmpty())return {converted.error,true};
    QString output;for(const auto& part:FileTransformer::wrappedParts(descriptor,converted)){
        if(part.toObject()["type"]!="text")return {"This native tool transport cannot return non-text file parts yet.",true};
        output+=part.toObject()["text"].toString();
    }
    return {output,false};
}
QJsonObject prop(const char *type, const char *description) {
    return {{"type", QString::fromLatin1(type)}, {"description", QString::fromUtf8(description)}};
}
QJsonObject definition(const char *name, const char *description, QJsonObject properties,
                       QJsonArray required) {
    return {{"type", "function"}, {"function", QJsonObject{
        {"name", QString::fromLatin1(name)}, {"description", QString::fromUtf8(description)},
        {"parameters", QJsonObject{{"type", "object"}, {"properties", properties},
                                   {"required", required}, {"additionalProperties", false}}}}}};
}
struct SearchRequest {
    bool grep=false;
    QString path,root;
    SearchScope scope;
    QRegularExpression regex;
    int maxMatches=15,timeLimitMs=0;
};
// Runs off the owner thread. The wall-clock limit starts with the walk and
// ends it with the ordinary limit note; cancellation discards the result.
FormattedRead runSearch(const SearchRequest& request,const std::atomic_bool* cancelled){
    const QDeadlineTimer deadline(request.timeLimitMs);
    const auto stopped=[&]{return (cancelled&&cancelled->load(std::memory_order_relaxed))||deadline.hasExpired();};
    constexpr int globResultLimit = 50;
    // Reserve room for truthful completion diagnostics; capped() must not
    // cut those off after a long path or matching line fills the payload.
    constexpr qsizetype payloadLimit = MaxOutputChars - 2048;
    constexpr qint64 readLimit = 8 * 1024 * 1024;
    QString output,error;
    int matches = 0, skippedFiles = 0, globMatches = 0;
    IgnoreLoadState ignoreState;
    qint64 remainingReadBytes = readLimit;
    bool truncated = false;
    const auto append = [&](const QString& line) {
        if (line.size() > payloadLimit - output.size()) {
            truncated = true;
            return false;
        }
        output += line;
        return true;
    };
    if(!walkSearchTree(request.path,request.root,request.scope,[&](const ScopedDirectoryEntry& child,const QString& relative){
        if(child.isDirectory)return DirectoryVisit::Continue;
        if(!request.grep){
            if(globMatches>=globResultLimit){truncated=true;return DirectoryVisit::Stop;}
            if(!append(relative+'\n'))return DirectoryVisit::Stop;
            ++globMatches;return DirectoryVisit::SkipChildren;
        }
        const auto remaining = remainingReadBytes;
        if (remaining <= 0) { truncated = true; return DirectoryVisit::Stop; }
        QByteArray bytes;
        QString readError;
        const bool read = readBoundedFile(child.absolutePath, request.root, &bytes, &readError,
                                         qMin<qint64>(512 * 1024, remaining), nullptr, cancelled, &remainingReadBytes);
        if (!read) { ++skippedFiles; return DirectoryVisit::SkipChildren; }
        QString text;
        if (bytes.startsWith("\xff\xfe") || bytes.startsWith("\xfe\xff")) {
            if (bytes.size() % 2 != 0) { ++skippedFiles; return DirectoryVisit::SkipChildren; }
            QStringDecoder decoder(bytes.startsWith("\xff\xfe") ? QStringConverter::Utf16LE : QStringConverter::Utf16BE,
                                   QStringConverter::Flag::Stateless);
            text = decoder(bytes);
            if (decoder.hasError() || !text.isValidUtf16()) { ++skippedFiles; return DirectoryVisit::SkipChildren; }
            if (text.contains(QChar(0))) return DirectoryVisit::SkipChildren;
        } else {
            if (bytes.contains('\0')) return DirectoryVisit::SkipChildren;
            text = QString::fromUtf8(bytes);
        }
        const auto lines = QStringView(text).split('\n');
        for(qsizetype i=0;i<lines.size();++i){
            if(!(i&1023)&&stopped()){truncated=true;return DirectoryVisit::Stop;}
            if(!request.regex.matchView(lines[i]).hasMatch())continue;
            QString preview;
            if (lines[i].size() > 500) {
                qsizetype length = 497;
                if (lines[i].at(length - 1).isHighSurrogate() && lines[i].at(length).isLowSurrogate()) --length;
                preview = lines[i].left(length).toString() + "...";
            } else preview = lines[i].toString();
            if (!append(relative + ':' + QString::number(i + 1) + ": " + preview + '\n')) return DirectoryVisit::Stop;
            if (++matches >= request.maxMatches) { truncated = true; return DirectoryVisit::Stop; }
        }
        return DirectoryVisit::SkipChildren;
    },&error,&truncated,&ignoreState,cancelled,deadline))return {error,true};
    if (truncated) output += "\n[Search limit reached; narrow the query.]";
    if (skippedFiles) output += QString("\n[Search incomplete: skipped %1 file(s) that exceeded the read limit, could not be read safely, or had malformed text encoding.]").arg(skippedFiles);
    for (const auto& note : ignoreState.diagnostics) output += '\n' + note;
    if (ignoreState.omittedDiagnostics) output += QStringLiteral("\n[... and %1 more ignore-file diagnostics.]").arg(ignoreState.omittedDiagnostics);
    return {output.isEmpty() ? QStringLiteral("No matches.") : output, false};
}
}

ToolExecutor::ToolExecutor(QObject *parent) : QObject(parent) {
    m_processTimeout.setSingleShot(true);
    connect(&m_processTimeout, &QTimer::timeout, this, [this] {
        m_commandTimedOut = true;
        closeJob();
        if (m_process) m_process->kill();
    });
}

ToolExecutor::~ToolExecutor() {
    ++m_generation;
    if(m_fileReadCancelled)m_fileReadCancelled->store(true);
    closeJob();
    for (auto process : findChildren<QProcess *>(QString(), Qt::FindDirectChildrenOnly)) {
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
}

bool ToolExecutor::setWorkspace(const QString &path) {
    if (!m_id.isEmpty()) return false;
    const QFileInfo info(path);
    if (!info.isDir()) { m_workspace.clear(); return false; }
    m_workspace = resolvedExistingPath(info.absoluteFilePath());
    return !m_workspace.isEmpty();
}

bool ToolExecutor::setAppsDirectory(const QString &path) {
    if(!m_id.isEmpty())return false;
    if(path.isEmpty()){m_appsDirectory.clear();return true;}
    const QFileInfo info(path);if(!info.isDir())return false;
    const auto canonical=resolvedExistingPath(info.absoluteFilePath());if(canonical.isEmpty())return false;
    m_appsDirectory=canonical;return true;
}

QString ToolExecutor::rootForPath(const QString &requested,QString *relative) const {
    QString path=QDir::fromNativeSeparators(requested),root=m_workspace;
    if(path=="apps"||path.startsWith("apps/")){root=m_appsDirectory;path=path=="apps"?QStringLiteral("."):path.mid(5);}
    else if(path=="ws1"||path=="w1"||path=="workspace")path=".";
    else if(path.startsWith("ws1/"))path.remove(0,4);
    else if(path.startsWith("w1/"))path.remove(0,3);
    else if(path.startsWith("workspace/"))path.remove(0,10);
    else if(QRegularExpression(QStringLiteral("^(att|plugins|logs|shells|memory|plans|w[0-9a-f]{4}|w[0-9]+|ws[0-9]+)(/|$)")).match(path).hasMatch())root.clear();
    else if(QFileInfo(path).isAbsolute()&&!m_appsDirectory.isEmpty()&&within(m_appsDirectory,QDir::cleanPath(path)))root=m_appsDirectory;
    if(relative)*relative=path.isEmpty()?QStringLiteral("."):path;
    return root;
}

QString ToolExecutor::resolvePath(const QString &requested, QString *error) const {
    auto reject = [error](const QString &reason) { if (error) *error = reason; return QString(); };
    if (error) error->clear();
    QString path;const auto root=rootForPath(requested,&path);
    if(root.isEmpty())return reject(QStringLiteral("The requested mount is unavailable. Connect a workspace or use the configured apps/ mount."));
    if(path.contains(QChar::Null))return reject(QStringLiteral("Path contains a null character."));
#ifdef Q_OS_WIN
    // Disallow alternate data streams and device paths, including drive-relative C:foo.
    if (path.startsWith("//") || path.startsWith("\\\\")
        || (path.contains(':') && !(path.size() > 2 && path.at(1) == ':' && path.at(2) == '/'
                                   && !path.mid(2).contains(':'))))
        return reject(QStringLiteral("Unsupported Windows device or stream path."));
    for (const auto &part : path.split('/')) {
        if (part == "." || part == ".." || part.isEmpty()) continue;
        if (part.endsWith('.') || part.endsWith(' '))
            return reject(QStringLiteral("Ambiguous Windows path component."));
        static const QRegularExpression reserved(QStringLiteral("^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$"),
                                                  QRegularExpression::CaseInsensitiveOption);
        if (reserved.match(part).hasMatch()) return reject(QStringLiteral("Reserved Windows device name."));
    }
#endif
    const QString absolute=QDir::cleanPath(QFileInfo(path).isAbsolute()?path:root+'/'+path);
    if(!within(root,absolute))return reject(QStringLiteral("Path is outside the selected mount."));
    QFileInfo ancestor(absolute);
    while(!ancestor.exists()&&!ancestor.isSymLink()&&!ancestor.isJunction()){
        const QString parent=ancestor.absolutePath();if(parent==ancestor.absoluteFilePath())break;ancestor.setFile(parent);
    }
    const QString canonical=resolvedExistingPath(ancestor.absoluteFilePath());
    if(canonical.isEmpty()||!within(root,canonical))return reject(QStringLiteral("Path resolves outside the selected mount or through a broken link."));
    return absolute;
}

bool ToolExecutor::requiresApproval(const QString &name) {
    return name != "read" && name != "ls" && name != "glob" && name != "grepSearch";
}

QJsonArray ToolExecutor::definitions() {
    const auto path = prop("string", "Path under the connected workspace (ws1/ or w1/) or the host-owned apps/ mount. Unprefixed paths use the workspace.");
    const auto readPath = prop("string", "Path under the workspace (ws1/ or w1/), chat-owned apps/, or read-only att/ attachment and shells/ session-log mounts. These are flat directories. Unprefixed paths use the workspace.");
    const auto edits = QJsonObject{{"type", "array"}, {"minItems", 1}, {"items", QJsonObject{
        {"type", "object"}, {"properties", QJsonObject{{"old_string", prop("string", "Exact text to find.")},
        {"new_string", prop("string", "Replacement text.")}, {"replace_all", prop("boolean", "Replace all matches; default false.")}}},
        {"required", QJsonArray{"old_string", "new_string"}}, {"additionalProperties", false}}}};
    return {
        definition("read", "Read file metadata and contents. Text uses 1-based line numbers and a bounded character budget. Preview returns a Markdown heading outline or a short text preview; small text files are promoted to full content. Unsupported formats return an explicit placeholder.",
                   {{"path", readPath}, {"start_line", prop("integer", "First line, inclusive; default 1.")},
                    {"end_line", prop("integer", "Last line, inclusive.")},
                    {"start_page", prop("integer", "First page, inclusive, for paginated formats. Ignored for text; PDF transformation is not yet implemented.")},
                    {"end_page", prop("integer", "Last page, inclusive.")},
                    {"preview", prop("boolean", "Request a structural preview; default false.")}}, {"path"}),
        definition("ls", "List a directory as a bounded tree with metadata. Includes hidden entries; does not follow directory links.",
                   {{"path", readPath}, {"depth", prop("number", "Nonnegative recursion depth, default 0; native maximum 32.")}}, {"path"}),
        definition("write", "Write a UTF-8 file with durable recovery and undo history. Creates parents; strips surrounding code fences for model writes.",
                   {{"path", path}, {"content", prop("string", "Complete new file content.")}}, {"path", "content"}),
        definition("multiEdit", "Apply exact replacements sequentially with durable undo history. Unmatched edits are skipped; reports editsApplied. Empty search strings are unsupported.",
                   {{"path", path}, {"edits", edits}}, {"path", "edits"}),
        definition("copy", "Copy a scoped file or directory; move=true removes the source after capturing durable recovery history. Both sides are checked independently.",
                   {{"input_path",readPath},{"output_path",path},{"move",prop("boolean","Required: true moves, false copies.")}}, {"input_path","output_path","move"}),
        definition("delete", "Delete a scoped file or directory with durable recovery and conflict-aware undo history.", {{"path",path}}, {"path"}),
        definition("mkdir", "Create a directory and missing parent directories.", {{"path", path}}, {"path"}),
        definition("glob", "Find files BY THEIR PATH/NAME using glob patterns (like 'find'). Returns at most 50 paths; a truncated list is reported. Respects .gitignore, .ignore and .rgignore files inside the mount (and skips node_modules) unless include_gitignored is true; .git and links are never listed. Traversal is bounded to 10000 entries, depth 64 and 20 seconds; limits are reported.",
                   {{"pattern", prop("string", "Glob pattern supporting standard syntax (*, **, ?, [abc], {a,b}). A pattern without '/' matches names at any depth. Examples: '**/*.test.ts' for test files, 'src/**/config.json' for configs.")},
                    {"include_gitignored", prop("boolean", "If true, includes files from gitignored paths (e.g. node_modules, dist). Use with specific patterns to avoid noise. Defaults to false.")},
                    {"mount_prefix", prop("string", "Workspace mount; ws1.")}}, {"pattern"}),
        definition("grepSearch", "Regex search INSIDE file contents. Hidden files and directories are skipped unless a pattern or ignore rule names them, and ignore files are respected unless include_gitignored is true. Output, byte reads, traversal and time (20 seconds) are bounded; search depth is at most 64.",
                   {{"query", prop("string", "Regular expression to search.")}, {"case_sensitive", prop("boolean", "Whether search is case sensitive. Defaults to false (case insensitive).")},
                    {"include_file_pattern", prop("string", "Glob pattern for files to include. Examples: \"*.ts\", \"**/*.tsx\", \"src/**/*.js\".")},
                    {"exclude_file_pattern", prop("string", "Glob pattern for files to exclude. Examples: \"**/test-*.js\", \"metadata/**\".")},
                    {"max_matches", prop("integer", "Maximum matches to return. Defaults to 15, maximum allowed is 50.")},
                    {"include_gitignored", prop("boolean", "If true, includes files from gitignored paths (e.g. node_modules, dist). Use with specific patterns to avoid noise. Defaults to false.")},
                    {"mount_prefix", prop("string", "Workspace mount; ws1.")}}, {"query"}),
        definition("runCommand", "Execute a shell command in the workspace (PowerShell on Windows). Requires approval unless always-allow is enabled. Commands run with the user's OS permissions; they are not filesystem sandboxed.",
                   {{"command", prop("string", "Shell command to execute.")}, {"cwd", path},
                    {"timeout_seconds", prop("integer", "Timeout in seconds, 1-300; default 60.")}}, {"command"})
    };
}

void ToolExecutor::executeWithProvenance(const QString &id, const QString &name, const QJsonObject &arguments,const QString& mutationProvenanceId) {
    if (!m_id.isEmpty()) {
        emit completed(id, QStringLiteral("A tool is already running."), true);
        return;
    }
    m_id = id;
    m_mutationProvenanceId=mutationProvenanceId.isEmpty()?id:mutationProvenanceId;
    const auto generation = ++m_generation;
    QTimer::singleShot(0, this, [this, name, arguments, generation] {
        if (generation != m_generation || m_id.isEmpty()) return;
        if (name == "runCommand") executeCommand(arguments);
        else executeFileTool(name, arguments);
    });
}

void ToolExecutor::finish(const QString &output, bool error, bool truncate,const QJsonObject& metadata) {
    if (m_id.isEmpty()) return;
    const QString id = m_id;
    m_id.clear();
    emit completed(id, truncate?capped(output):output, error,metadata);
}

void ToolExecutor::executeFileTool(const QString &name, const QJsonObject &args) {
    QString error;
    if(m_fileMutationExecutor&&QStringList{"write","multiEdit","writeBytes","copy","delete"}.contains(name)){
        const auto generation=m_generation;const auto id=m_id;const auto executor=m_fileMutationExecutor;
        const auto invocationId=QUuid::createUuid().toString(QUuid::WithoutBraces);
        const FileMutationInvocation invocation{m_mutationProvenanceId,name,m_workspace,m_appsDirectory,args,invocationId};
        const auto canceled=std::make_shared<std::atomic_bool>(false);m_fileReadCancelled=canceled;
        auto* watcher=new QFutureWatcher<FileMutationOutcome>(this);
        connect(watcher,&QFutureWatcher<FileMutationOutcome>::finished,this,[this,watcher,generation,id,invocationId]{
            const auto result=watcher->result();watcher->deleteLater();
            const QPointer<ToolExecutor> self(this);
            emit mutationSettled(invocationId,id,result.output,result.error,result.metadata);
            if(!self)return;
            // A committed filesystem change remains observable after its request
            // is canceled or replaced. Only completion belongs to that dispatch.
            for(const auto& change:result.changes){emit fileChanged(change.path,change.before,change.after);if(!self)return;}
            if(generation!=m_generation||id!=m_id)return;
            m_fileReadCancelled.reset();
            finish(result.output,result.error,false,result.metadata);
        });
        const QPointer<ToolExecutor> self(this);
        emit mutationStarted(invocationId,id);
        if(!self)return;
        if(generation!=m_generation||id!=m_id){
            watcher->deleteLater();
            emit mutationSettled(invocationId,id,"Mutation cancelled before dispatch.",true,{});return;
        }
        watcher->setFuture(QtConcurrent::run([executor,invocation,canceled]{return executor(invocation,canceled.get());}));return;
    }
    if(name=="copy"||name=="delete"){finish("A durable mutation host must be connected for this operation.",true);return;}
    if((name=="read"||name=="ls")&&(!args["path"].isString()||args["path"].toString().isEmpty())){finish("A nonempty string path is required.",true);return;}
    QString requested=args["path"].toString(".");
    const bool attachment=requested=="att"||requested.startsWith("att/")||requested.startsWith("att\\");
    const bool shell=requested=="shells"||requested.startsWith("shells/")||requested.startsWith("shells\\");
    if(attachment||shell){
        const bool raw=shell&&(name=="readBytes"||name=="readDirectory"||(name=="read"&&args["raw"].toBool()));
        if(!raw&&((name!="read"&&name!="ls")||args["raw"].toBool())){finish("This mount allows read and list operations only.",true);return;}
        const auto source=attachment?m_attachmentReadSource:m_shellReadSource;
        if(!source){finish("The requested read-only mount is unavailable.",true);return;}
        FileReadParams params;
        if(!raw&&!(name=="ls"?FileTransformer::listParams(args,&params,&error):FileTransformer::readParams(args,&params,&error))){finish(error,true);return;}
        const auto generation=m_generation;const auto id=m_id;
        const auto committer=m_fileReadCommitter;const auto inputHash=fileReadInputHash(name,args);
        const auto cancelled=std::make_shared<std::atomic_bool>(false);m_fileReadCancelled=cancelled;
        auto* watcher=new QFutureWatcher<FormattedRead>(this);
        connect(watcher,&QFutureWatcher<FormattedRead>::finished,this,[this,watcher,generation,id]{
            const auto result=watcher->result();watcher->deleteLater();
            if(generation!=m_generation||id!=m_id)return;
            m_fileReadCancelled.reset();finish(result.output,result.error,false,result.metadata);
        });
        watcher->setFuture(QtConcurrent::run([source,name,requested,params,cancelled,committer,inputHash,raw]{return raw?rawMountedRead(source,name,requested,cancelled.get()):formattedMountedRead(source,name,requested,params,cancelled.get(),committer,inputHash);}));
        return;
    }
    if((name=="glob"||name=="grepSearch")&&args.contains("mount_prefix")){
        const auto prefix=args["mount_prefix"].toString();
        if(!QStringList{"ws1","w1","workspace","apps"}.contains(prefix)){finish("Unknown file mount.",true);return;}
        requested=prefix+"/"+requested;
    }
    const auto path=resolvePath(requested,&error);const auto root=rootForPath(requested);
    if(path.isEmpty()){finish(error,true);return;}
    QFileInfo info(path);
    if(name=="ls"||(name=="read"&&!args["raw"].toBool())){
        FileReadParams params;
        const bool valid=name=="ls"?FileTransformer::listParams(args,&params,&error):FileTransformer::readParams(args,&params,&error);
        if(!valid){finish(error,true);return;}
        const auto generation=m_generation;const auto id=m_id;
        const auto committer=m_fileReadCommitter;
        const auto inputHash=fileReadInputHash(name,args);
        QString capturedPath=requested;
        if(committer){
            const auto relative=QDir(root).relativeFilePath(path);
            const auto prefix=(requested=="apps"||requested.startsWith("apps/"))?QStringLiteral("apps"):QStringLiteral("ws1");
            capturedPath=relative=="."?prefix:prefix+'/'+relative;
        }
        const auto cancelled=std::make_shared<std::atomic_bool>(false);m_fileReadCancelled=cancelled;
        auto* watcher=new QFutureWatcher<FormattedRead>(this);
        connect(watcher,&QFutureWatcher<FormattedRead>::finished,this,[this,watcher,generation,id]{
            const auto result=watcher->result();watcher->deleteLater();
            if(generation!=m_generation||id!=m_id)return;
            m_fileReadCancelled.reset();finish(result.output,result.error,false,result.metadata);
        });
        watcher->setFuture(QtConcurrent::run([name,path,root,capturedPath,params,cancelled,committer,inputHash]{return formattedRead(name,path,root,capturedPath,params,cancelled.get(),committer,inputHash);}));
        return;
    }
    if(name=="read"||name=="readBytes") {
        QByteArray bytes;if(!readBoundedFile(path,root,&bytes,&error)){finish(error,true);return;}
        if(name=="readBytes"){finish(QString::fromLatin1(bytes.toBase64()),false,false);return;}
        if (bytes.contains('\0')) { finish("Text read rejects binary content. Use fs.readFile(path) in the sandbox for raw bytes.", true); return; }
        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::ConvertInitialBom | QStringConverter::Flag::Stateless);
        const QString decoded = decoder(bytes);
        if (decoder.hasError()) { finish("File is not valid UTF-8; decoding would change its contents.", true); return; }
        const auto encoding = args["encoding"].toString("utf8").toLower();
        if (encoding != "utf8" && encoding != "utf-8") { finish("Raw text reads currently support UTF-8 encoding only.", true); return; }
        if (decoded.size() > MaxOutputChars) { finish("Raw read exceeds the 64 Ki-character result limit; no truncated content was returned.", true); return; }
        finish(decoded);
    } else if (name == "mkdir") {
        if (info.exists() && !info.isDir()) { finish("Path is an existing file.", true); return; }
        const bool recursive=!args["raw"].toBool()||args["recursive"].toBool();
        if(!recursive&&info.exists()){finish("Directory already exists.",true);return;}
        QString firstCreated;
        if(args["raw"].toBool()&&recursive&&!info.exists()){
            firstCreated=path;
            while(!QFileInfo(QFileInfo(firstCreated).absolutePath()).exists())firstCreated=QFileInfo(firstCreated).absolutePath();
        }
        const bool created=recursive?QDir().mkpath(path):QDir().mkdir(path);
        finish(created?(args["raw"].toBool()?firstCreated:QStringLiteral("Directory created.")):QStringLiteral("Could not create directory."),!created);
    } else if (name == "write" || name == "multiEdit" || name == "writeBytes") {
        if (info.isDir()) { finish("Path is a directory.", true); return; }
        QByteArray before;
        if (info.exists()) {
            if(!readBoundedFile(path,root,&before,&error)){finish(error,true);return;}
        }
        QByteArray after;
        if(name=="writeBytes"){
            const auto value=args["base64"];
            if(!value.isString()||value.toString().size()>4*((MaxFileBytes+2)/3)){finish("Invalid binary body or body exceeds 4 MiB.",true);return;}
            const auto decoded=QByteArray::fromBase64Encoding(value.toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
            if(!decoded){finish("Invalid base64 file data.",true);return;}after=decoded.decoded;
        }else{
        QString content;
        if (name == "write") {
            if (!args["content"].isString()) { finish("content must be a string.", true); return; }
            content = args["content"].toString();
        } else {
            if (!info.exists() || before.contains('\0')) { finish("Editing requires an existing text file.", true); return; }
            const auto edits = args["edits"].toArray();
            if (edits.isEmpty()) { finish("At least one edit is required.", true); return; }
            QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::ConvertInitialBom | QStringConverter::Flag::Stateless);
            content = decoder(before);
            if (decoder.hasError()) { finish("File is not valid UTF-8; editing would change its encoding.", true); return; }
            for (const auto &value : edits) {
                const auto edit = value.toObject();
                const auto old = edit["old_string"].toString();
                if (old.isEmpty() || !edit["new_string"].isString()) { finish("Edits require nonempty old_string and string new_string.", true); return; }
                const auto index = content.indexOf(old);
                if (index < 0) { finish("Edit text was not found; file was not modified.", true); return; }
                if (edit["replace_all"].toBool()) content.replace(old, edit["new_string"].toString());
                else content.replace(index, old.size(), edit["new_string"].toString());
            }
        }
        after=content.toUtf8();
        }
        if (after.size() > MaxFileBytes) { finish("Write exceeds the 4 MiB file limit.", true); return; }
        if(args["raw"].toBool()&&!QFileInfo(info.absolutePath()).isDir()){finish("Parent directory does not exist; create it before fs.writeFile.",true);return;}
        if (!QDir().mkpath(info.absolutePath())) { finish("Cannot create parent directory.", true); return; }
        if (resolvePath(path, &error).isEmpty()) { finish(error, true); return; }
        QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly) || output.write(after) != after.size() || !output.commit()) {
            finish("Atomic save failed: " + output.errorString(), true); return;
        }
        const auto result = QStringLiteral("Saved %1 bytes to %2.").arg(after.size()).arg(QDir(root).relativeFilePath(path));
        const auto generation = m_generation; const auto id = m_id;
        const QPointer<ToolExecutor> alive(this);
        emit fileChanged(path, before, after);
        // Observers can cancel, change workspace, start another operation with
        // the same ID, or destroy the host. Never finish that replacement work.
        if (!alive || alive->m_generation != generation || alive->m_id != id) return;
        finish(result);
    }else if(name=="readDirectory"){
        if(!info.isDir()){finish("Not a readable directory.",true);return;}
        const bool recursive=args["recursive"].toBool();QJsonArray entries;
        if(!walkScopedDirectory(path,root,4096,[&](const ScopedDirectoryEntry&,const QString& relative){
            if(relative.count('/')>8){error="Directory enumeration exceeds its depth bound; no partial result was returned.";return DirectoryVisit::Error;}
            entries.append(relative);
            return recursive?DirectoryVisit::Continue:DirectoryVisit::SkipChildren;
        },&error)){finish(error,true);return;}
        const auto result=QString::fromUtf8(QJsonDocument(entries).toJson(QJsonDocument::Compact));
        if(result.size()>MaxOutputChars){finish("Directory result exceeds 64 Ki characters.",true);return;}
        finish(result,false,false);
    } else if (name == "glob" || name == "grepSearch") {
        if (!info.isDir()) { finish("Search path is not a directory.", true); return; }
        const bool grep = name == "grepSearch";
        SearchRequest request;
        request.grep = grep;
        request.path = path;
        request.root = root;
        request.maxMatches = qBound(1, args["max_matches"].toInt(15), 50);
        request.timeLimitMs = m_searchTimeLimitMs;
        const auto present = [&](const char* field) { return args.contains(field) && !args[field].isNull() && !args[field].isUndefined(); };
        if (present("include_gitignored") && !args["include_gitignored"].isBool()) { finish("include_gitignored must be a boolean.", true); return; }
        // Upstream rg order: the include glob, then "!" + the exclude glob; the
        // last matching glob wins. Empty patterns are treated as absent.
        auto& scope = request.scope;
        scope.policy.respectIgnores = !args["include_gitignored"].toBool();
        scope.policy.skipHidden = grep;
        for (const auto* field : grep ? QList<const char*>{"include_file_pattern", "exclude_file_pattern"} : QList<const char*>{"pattern"}) {
            if (present(field) && !args[field].isString()) { finish(QStringLiteral("%1 must be a string.").arg(QString::fromLatin1(field)), true); return; }
            const auto value = args[field].toString();
            if (value.isEmpty() || (!grep && value == "**")) continue;
            QString patternError;
            const bool exclude = QByteArray(field) == "exclude_file_pattern";
            if (!scope.overrides.add(exclude ? '!' + value : value, &patternError)) {
                finish(QStringLiteral("Invalid %1: %2.").arg(QString::fromLatin1(field), patternError), true); return;
            }
        }
        request.regex = QRegularExpression(args["query"].toString(), args["case_sensitive"].toBool()
            ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
        if (grep && (!request.regex.isValid() || args["query"].toString().isEmpty())) { finish("Invalid or empty search regular expression.", true); return; }
        // Like read and ls, the walk runs off the owner thread; cancel() stops it.
        const auto generation=m_generation;const auto id=m_id;
        const auto cancelled=std::make_shared<std::atomic_bool>(false);m_fileReadCancelled=cancelled;
        auto* watcher=new QFutureWatcher<FormattedRead>(this);
        connect(watcher,&QFutureWatcher<FormattedRead>::finished,this,[this,watcher,generation,id]{
            const auto result=watcher->result();watcher->deleteLater();
            if(generation!=m_generation||id!=m_id)return;
            m_fileReadCancelled.reset();finish(result.output,result.error,result.error);
        });
        watcher->setFuture(QtConcurrent::run([request,cancelled]{return runSearch(request,cancelled.get());}));
    } else finish("Unsupported tool: " + name, true);
}

void ToolExecutor::executeCommand(const QJsonObject &args) {
    if(m_workspace.isEmpty()){finish("Connect a workspace before running commands.",true);return;}
    QString error;
    const auto cwd = resolvePath(args["cwd"].toString("."), &error);
    if (cwd.isEmpty() || !QFileInfo(cwd).isDir()) { finish(error.isEmpty() ? "Command directory does not exist." : error, true); return; }
    const auto command = args["command"].toString();
    if (command.trimmed().isEmpty() || command.contains(QChar::Null)) { finish("Invalid empty command.", true); return; }
    m_process = new QProcess(this);
    m_process->setWorkingDirectory(cwd);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_processOutput.clear();
    m_outputTruncated = false;
    m_commandTimedOut = false;
    const auto processForOperation = m_process;
    const auto generation = m_generation;
    connect(m_process, &QProcess::readyReadStandardOutput, this, &ToolExecutor::drainProcess);
    connect(m_process, &QProcess::errorOccurred, this, [this, processForOperation, generation](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_process != processForOperation || generation != m_generation) return;
        const auto message = m_process->errorString();
        auto process = m_process;
        m_process = nullptr;
        m_processTimeout.stop();
        closeJob();
        process->deleteLater();
        finish("Unable to start command: " + message, true);
    });
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, processForOperation, generation](int exitCode, QProcess::ExitStatus status) {
        if (m_process != processForOperation || generation != m_generation) return;
        const QPointer<ToolExecutor> alive(this);
        drainProcess();
        if (!alive || alive->m_process != processForOperation || alive->m_generation != generation) return;
        const auto output = QString::fromUtf8(m_processOutput)
            + (m_outputTruncated ? "\n[Process output truncated.]" : "")
            + (m_commandTimedOut ? "\n[Command timed out.]" : "")
            + QStringLiteral("\n[Exit code: %1]").arg(exitCode);
        auto process = m_process;
        m_process = nullptr;
        m_processTimeout.stop();
        closeJob();
        process->deleteLater();
        finish(output, m_commandTimedOut || status != QProcess::NormalExit || exitCode != 0);
    });
#ifdef Q_OS_WIN
    m_process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
    m_job = CreateJobObjectW(nullptr, nullptr);
    if (m_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(static_cast<HANDLE>(m_job), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) closeJob();
    }
    connect(m_process, &QProcess::started, this, [this, processForOperation, generation] {
        if (m_process != processForOperation || generation != m_generation || !m_job) return;
        HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(m_process->processId()));
        if (process) { AssignProcessToJobObject(static_cast<HANDLE>(m_job), process); CloseHandle(process); }
    });
    const auto shell = QStandardPaths::findExecutable("powershell.exe");
    const QPointer<ToolExecutor> alive(this);
    m_process->start(shell.isEmpty() ? "powershell.exe" : shell,
                     {"-NoLogo", "-NoProfile", "-NonInteractive", "-Command", command});
#else
    const QPointer<ToolExecutor> alive(this);
    m_process->start("/bin/sh", {"-c", command});
#endif
    if (!alive || alive->m_process != processForOperation || alive->m_generation != generation) return;
    m_processTimeout.start(qBound(1, args["timeout_seconds"].toInt(60), 300) * 1000);
}

void ToolExecutor::drainProcess() {
    if (!m_process) return;
    const auto bytes = m_process->readAllStandardOutput();
    const auto available = qMax<qsizetype>(0, MaxOutputChars - m_processOutput.size());
    m_processOutput += bytes.left(available);
    if (bytes.size() > available) m_outputTruncated = true;
    if (available > 0) emit processOutput(m_id, QString::fromUtf8(bytes.left(available)));
}

void ToolExecutor::closeJob() {
#ifdef Q_OS_WIN
    if (m_job) CloseHandle(static_cast<HANDLE>(m_job));
#endif
    m_job = nullptr;
}

void ToolExecutor::cancel() {
    ++m_generation;
    if(m_fileReadCancelled){m_fileReadCancelled->store(true);m_fileReadCancelled.reset();}
    m_processTimeout.stop();
    closeJob();
    if (m_process) {
        auto process = m_process;
        m_process = nullptr;
        process->disconnect(this);
        process->kill();
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process, &QObject::deleteLater);
        if (process->state() == QProcess::NotRunning) process->deleteLater();
    }
    finish("Tool cancelled by user.", true);
}

} // namespace ade
