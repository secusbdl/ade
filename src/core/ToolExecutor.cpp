#include "ToolExecutor.h"
#include "ScopedDirectory.h"
#include "attachments/FileTransformer.h"

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
bool readBoundedFile(const QString& path,const QString& root,QByteArray* bytes,QString* error,qint64 maximum=MaxFileBytes,QDateTime* modified=nullptr,const std::atomic_bool* cancelled=nullptr) {
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
        DWORD count=0;if(!ReadFile(handle,chunk.data(),static_cast<DWORD>(chunk.size()),&count,nullptr)){*error="Cannot read file bytes.";return false;}
        if(!count)break;
        if(bytes->size()+count>maximum){*error="File grew beyond the bounded read limit.";return false;}
        bytes->append(chunk.constData(),count);
    }
#else
    const auto resolved=QFileInfo(path).canonicalFilePath();QFile file(path);
    if(resolved.isEmpty()||!within(root,resolved)||!file.open(QIODevice::ReadOnly)){*error="Cannot open scoped file.";return false;}
    if(modified)*modified=QFileInfo(file).lastModified().toUTC();
    *bytes=file.read(maximum+1);if(bytes->size()>maximum||!file.atEnd()){*error="File exceeds the bounded read limit.";return false;}
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
struct FormattedRead {QString output;bool error=false;QJsonObject metadata={};};
FormattedRead formattedAttachmentRead(const FileReadSource& source,const QString& name,const QString& path,
    const FileReadParams& params,const std::atomic_bool* cancelled,const FileReadCommitter& committer,const QString& inputHash){
    try{
        FileReadCapture capture;QString error;
        if(!source(name,path,params,&capture,cancelled,&error))return {error.isEmpty()?QStringLiteral("Attachment capture failed."):error,true};
        if(cancelled&&cancelled->load())return {"Attachment read canceled.",true};
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
    }catch(...){return {"The attachment capture could not be completed.",true,{}};}
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
QRegularExpression globRegex(QString glob) {
    QString expression;
    for (qsizetype i = 0; i < glob.size(); ++i) {
        const auto c = glob.at(i);
        if (c == '*') {
            if (i + 1 < glob.size() && glob.at(i + 1) == '*') {
                ++i;
                if (i + 1 < glob.size() && glob.at(i + 1) == '/') {
                    ++i;
                    expression += QStringLiteral("(?:.*/)?");
                } else expression += QStringLiteral(".*");
            } else expression += QStringLiteral("[^/]*");
        } else if (c == '?') expression += QStringLiteral("[^/]");
        else expression += QRegularExpression::escape(c);
    }
    return QRegularExpression(QStringLiteral("^(?:") + expression + QStringLiteral(")$"));
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
    else if(QRegularExpression(QStringLiteral("^(att|plugins|logs|shells|memory|w[0-9a-f]+|ws[0-9]+)(/|$)")).match(path).hasMatch())root.clear();
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
    const auto readPath = prop("string", "Path under the workspace (ws1/ or w1/), chat-owned apps/, or read-only att/ attachment mount. att/ is a flat blob directory. Unprefixed paths use the workspace.");
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
        definition("write", "Write a UTF-8 file atomically. Creates parent directories; overwrites existing content.",
                   {{"path", path}, {"content", prop("string", "Complete new file content.")}}, {"path", "content"}),
        definition("multiEdit", "Apply exact replacements sequentially to a file, then save atomically. No write if any edit fails.",
                   {{"path", path}, {"edits", edits}}, {"path", "edits"}),
        definition("mkdir", "Create a directory and missing parent directories.", {{"path", path}}, {"path"}),
        definition("glob", "Find files by workspace-relative glob pattern, supporting *, ** and ?. Traversal is bounded to 10000 entries and depth 64; limits are reported.",
                   {{"pattern", prop("string", "Glob pattern, e.g. **/*.cpp.")}, {"mount_prefix", prop("string", "Workspace mount; ws1.")}}, {"pattern"}),
        definition("grepSearch", "Search text files using a regular expression. Output, byte reads and traversal are bounded; search depth is at most 64.",
                   {{"query", prop("string", "Regular expression to search.")}, {"case_sensitive", prop("boolean", "Default false.")},
                    {"include_file_pattern", prop("string", "Optional relative-path glob.")},
                    {"max_matches", prop("integer", "Maximum result count, 1-50.")}, {"mount_prefix", prop("string", "Workspace mount; ws1.")}}, {"query"}),
        definition("runCommand", "Execute a shell command in the workspace (PowerShell on Windows). Requires approval unless always-allow is enabled. Commands run with the user's OS permissions; they are not filesystem sandboxed.",
                   {{"command", prop("string", "Shell command to execute.")}, {"cwd", path},
                    {"timeout_seconds", prop("integer", "Timeout in seconds, 1-300; default 60.")}}, {"command"})
    };
}

void ToolExecutor::execute(const QString &id, const QString &name, const QJsonObject &arguments) {
    if (!m_id.isEmpty()) {
        emit completed(id, QStringLiteral("A tool is already running."), true);
        return;
    }
    m_id = id;
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
    if((name=="read"||name=="ls")&&(!args["path"].isString()||args["path"].toString().isEmpty())){finish("A nonempty string path is required.",true);return;}
    QString requested=args["path"].toString(".");
    const bool attachment=requested=="att"||requested.startsWith("att/")||requested.startsWith("att\\");
    if(attachment){
        if((name!="read"&&name!="ls")||args["raw"].toBool()){finish("The att/ mount allows formatted read and ls only; raw reads use the attachment broker.",true);return;}
        if(!m_attachmentReadSource){finish("The att/ mount is unavailable.",true);return;}
        FileReadParams params;
        if(!(name=="ls"?FileTransformer::listParams(args,&params,&error):FileTransformer::readParams(args,&params,&error))){finish(error,true);return;}
        const auto generation=m_generation;const auto id=m_id;const auto source=m_attachmentReadSource;
        const auto committer=m_fileReadCommitter;const auto inputHash=fileReadInputHash(name,args);
        const auto cancelled=std::make_shared<std::atomic_bool>(false);m_fileReadCancelled=cancelled;
        auto* watcher=new QFutureWatcher<FormattedRead>(this);
        connect(watcher,&QFutureWatcher<FormattedRead>::finished,this,[this,watcher,generation,id]{
            const auto result=watcher->result();watcher->deleteLater();
            if(generation!=m_generation||id!=m_id)return;
            m_fileReadCancelled.reset();finish(result.output,result.error,false,result.metadata);
        });
        watcher->setFuture(QtConcurrent::run([source,name,requested,params,cancelled,committer,inputHash]{return formattedAttachmentRead(source,name,requested,params,cancelled.get(),committer,inputHash);}));
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
        const int maxMatches = qBound(1, args["max_matches"].toInt(15), 50);
        const auto pattern = args[name == "glob" ? "pattern" : "include_file_pattern"].toString("**/*");
        const auto glob = globRegex(pattern);
        QRegularExpression regex(args["query"].toString(), args["case_sensitive"].toBool()
            ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
        if (name == "grepSearch" && (!regex.isValid() || args["query"].toString().isEmpty())) { finish("Invalid or empty search regular expression.", true); return; }
        QString output;int matches=0;qint64 readBytes=0;bool truncated=false;
        if(!walkScopedDirectory(path,root,10000,[&](const ScopedDirectoryEntry& child,const QString& relative){
            if(output.size()>MaxOutputChars||readBytes>8*1024*1024){truncated=true;return DirectoryVisit::Stop;}
            if(child.name==".git"||child.name=="node_modules"||child.isReparsePoint)return DirectoryVisit::SkipChildren;
            if(child.isDirectory)return DirectoryVisit::Continue;
            if(!glob.match(relative).hasMatch())return DirectoryVisit::SkipChildren;
            if(name=="glob"){output+=relative+'\n';return DirectoryVisit::SkipChildren;}
            const auto remaining=8*1024*1024-readBytes;
            if(remaining<=0){truncated=true;return DirectoryVisit::Stop;}
            QByteArray bytes;QString readError;
            if(!readBoundedFile(child.absolutePath,root,&bytes,&readError,qMin<qint64>(512*1024,remaining)))return DirectoryVisit::SkipChildren;
            readBytes+=bytes.size();if(bytes.contains('\0'))return DirectoryVisit::SkipChildren;
            const auto lines=QString::fromUtf8(bytes).split('\n');
            for(qsizetype i=0;i<lines.size();++i){
                if(!regex.match(lines[i]).hasMatch())continue;
                output+=relative+':'+QString::number(i+1)+": "+lines[i]+'\n';
                if(++matches>=maxMatches||output.size()>MaxOutputChars){truncated=true;return DirectoryVisit::Stop;}
            }
            return DirectoryVisit::SkipChildren;
        },&error,&truncated,64)){finish(error,true);return;}
        if (truncated) output += "\n[Search limit reached; narrow the query.]";
        finish(output.isEmpty() ? QStringLiteral("No matches.") : output);
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
