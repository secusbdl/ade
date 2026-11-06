#include "GitRepositoryWatcher.h"
#include "ProcessRunner.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <algorithm>
#include <atomic>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace ade {
namespace {
using namespace gitdetail;
using Cancel=std::shared_ptr<std::atomic_bool>;
struct ScanResult { QByteArray fingerprint,rootIdentity; QStringList paths; bool complete=true,cancelled=false; QString reason; };
void incomplete(ScanResult &r,const QString &reason) { r.complete=false;if(r.reason.isEmpty())r.reason=reason; }
void token(QCryptographicHash &hash,const QByteArray &bytes) {
    hash.addData(QByteArray::number(bytes.size()));hash.addData(QByteArrayView(":",1));hash.addData(bytes);hash.addData(QByteArrayView(";",1));
}
#ifdef Q_OS_WIN
QString openedPath(HANDLE handle) {
    const DWORD size=GetFinalPathNameByHandleW(handle,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!size)return {};
    std::wstring buffer(size+1,L'\0');
    const DWORD written=GetFinalPathNameByHandleW(handle,buffer.data(),DWORD(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!written||written>=buffer.size())return {};
    QString result=QString::fromWCharArray(buffer.data(),written);
    if(result.startsWith("\\\\?\\UNC\\"))result="\\\\"+result.mid(8);else if(result.startsWith("\\\\?\\"))result.remove(0,4);
    return QDir::cleanPath(QDir::fromNativeSeparators(result));
}
#endif
struct RootProbe { QByteArray identity;QString reason; };
RootProbe rootProbe(const QString &root) {
    RootProbe result;
#ifdef Q_OS_WIN
    const auto native=QDir::toNativeSeparators(root).toStdWString();
    HANDLE handle=CreateFileW(native.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(handle==INVALID_HANDLE_VALUE){result.reason="Repository metadata directory is unavailable.";return result;}
    BY_HANDLE_FILE_INFORMATION info{};
    const bool valid=GetFileInformationByHandle(handle,&info)&&(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&key(openedPath(handle))==key(root);
    CloseHandle(handle);
    if(!valid){result.reason="Repository metadata directory has been redirected or is not a regular directory.";return result;}
    result.identity=QByteArray::number(info.dwVolumeSerialNumber)+':'+QByteArray::number(info.nFileIndexHigh)+':'+QByteArray::number(info.nFileIndexLow)+':'+QByteArray::number(info.ftCreationTime.dwHighDateTime)+':'+QByteArray::number(info.ftCreationTime.dwLowDateTime);
#else
    struct stat info{};const auto encoded=QFile::encodeName(root);
    if(::lstat(encoded.constData(),&info)!=0||!S_ISDIR(info.st_mode)||key(canonical(root))!=key(root)){result.reason="Repository metadata directory is unavailable or redirected.";return result;}
    result.identity=QByteArray::number(quint64(info.st_dev))+':'+QByteArray::number(quint64(info.st_ino));
#endif
    return result;
}
QByteArray readHead(const QString &root,const QString &path,int limit,int &remaining,const Cancel &cancel,ScanResult &result) {
    if(cancel->load())return {};
    QFileInfo info(path);
    if(!info.exists()&&!info.isSymLink()&&!info.isJunction())return "missing";
    if(info.isSymLink()||info.isJunction()||!info.isFile()||!inside(root,canonical(path))) {
        incomplete(result,"HEAD metadata resolves outside the repository or is not a regular file.");return "unsafe";
    }
    QByteArray bytes;
#ifdef Q_OS_WIN
    const auto native=QDir::toNativeSeparators(path).toStdWString();
    HANDLE file=CreateFileW(native.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(file==INVALID_HANDLE_VALUE){incomplete(result,"Cannot open HEAD metadata.");return "unreadable";}
    BY_HANDLE_FILE_INFORMATION details{};
    if(!GetFileInformationByHandle(file,&details)||(details.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||GetFileType(file)!=FILE_TYPE_DISK||!inside(root,openedPath(file))) {
        CloseHandle(file);incomplete(result,"Opened HEAD metadata is not a regular file inside the repository.");return "unsafe";
    }
    char chunk[4096];bool failed=false;
    while(bytes.size()<=limit&&!cancel->load()) {
        DWORD count=0;
        const DWORD wanted=DWORD(std::min<qsizetype>(sizeof(chunk),limit+1-bytes.size()));
        if(!ReadFile(file,chunk,wanted,&count,nullptr)){failed=true;break;}
        if(!count)break;
        bytes.append(chunk,qsizetype(count));
    }
    CloseHandle(file);
    remaining-=std::min(remaining,int(bytes.size()));
    if(failed){incomplete(result,"Cannot read HEAD metadata.");return "unreadable";}
#else
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)||!inside(root,canonical(path))) {incomplete(result,"Cannot open HEAD metadata inside repository.");return "unreadable";}
    bytes=file.read(qint64(limit)+1);
    remaining-=std::min(remaining,int(bytes.size()));
    if(file.error()!=QFileDevice::NoError){incomplete(result,"Cannot read HEAD metadata.");return "unreadable";}
#endif
    if(bytes.size()>limit){incomplete(result,"HEAD metadata exceeds the configured byte limit.");return "oversized";}
    result.paths.append(path);
    return "bytes:"+bytes;
}
ScanResult fingerprint(const QString &root,const GitRepositoryWatchOptions &options,const Cancel &cancel) {
    ScanResult result;QCryptographicHash hash(QCryptographicHash::Sha256);
    if(cancel->load()){result.cancelled=true;return result;}
    const auto rootState=rootProbe(root);result.rootIdentity=rootState.identity;
    if(rootState.identity.isEmpty()) {
        incomplete(result,rootState.reason);token(hash,"unavailable");result.fingerprint=hash.result();return result;
    }
    token(hash,result.rootIdentity);
    result.paths.append(root);
    int budget=options.maxTotalHeadBytes;
    auto head=[&](const QString &relative) {
        token(hash,relative.toUtf8());
        if(budget<=0){incomplete(result,"Combined HEAD metadata exceeds the configured byte limit.");token(hash,"budget");return;}
        const QByteArray bytes=readHead(root,QDir(root).filePath(relative),std::min(budget,options.maxHeadBytes),budget,cancel,result);
        token(hash,bytes);
    };
    head("HEAD");
    const QString worktrees=QDir(root).filePath("worktrees");QFileInfo treeInfo(worktrees);
    if(treeInfo.exists()||treeInfo.isSymLink()||treeInfo.isJunction()) {
        if(!treeInfo.isDir()||treeInfo.isSymLink()||treeInfo.isJunction()||key(canonical(worktrees))!=key(worktrees)) {
            incomplete(result,"Worktree metadata directory is not a regular directory inside the repository.");token(hash,"unsafe-worktrees");
        } else {
            result.paths.append(worktrees);QStringList names;
            // Do not use entryInfoList: a hostile metadata directory may contain an
            // unbounded number of entries. Count every entry, including files.
            QDirIterator entries(worktrees,QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot,QDirIterator::NoIteratorFlags);
            int count=0;
            while(entries.hasNext()&&!cancel->load()) {
                entries.next();if(++count>options.maxWorktrees){incomplete(result,"Worktree metadata exceeds the configured entry limit.");token(hash,"entry-limit");break;}
                const auto info=entries.fileInfo();
                if(info.isDir()||info.isSymLink()||info.isJunction())names.append(info.fileName());
            }
            std::sort(names.begin(),names.end());
            for(const QString &name:names) {
                if(cancel->load())break;
                const QString dir=QDir(worktrees).filePath(name);QFileInfo info(dir);token(hash,name.toUtf8());
                if(!info.isDir()||info.isSymLink()||info.isJunction()||key(canonical(dir))!=key(dir)) {
                    incomplete(result,"A worktree metadata entry is redirected or unreadable.");token(hash,"unsafe-entry");continue;
                }
                result.paths.append(dir);head("worktrees/"+name+"/HEAD");
            }
        }
    } else token(hash,"no-worktrees");
    result.cancelled=cancel->load();result.fingerprint=hash.result();return result;
}
}

struct GitRepositoryWatcher::Private {
    struct Group {
        QString path,key;quint64 generation=0,revision=0;
        QSet<QString> owners;QObject *scope=nullptr;QFileSystemWatcher *watcher=nullptr;
        QTimer *scanTimer=nullptr,*debounce=nullptr,*retry=nullptr;
        Cancel cancel=std::make_shared<std::atomic_bool>(false);
        bool initialized=false,inFlight=false,rescan=false,publishPending=false,complete=false,probeDeferred=false;
        QString coverageReason;QByteArray latest,published,rootIdentity;
    };
    GitRepositoryWatchOptions options;QThreadPool pool;quint64 generation=0;int outstandingScans=0;
    QHash<QString,std::shared_ptr<Group>> groups;QHash<QString,QString> owners;
};

GitRepositoryWatcher::GitRepositoryWatcher(GitRepositoryWatchOptions options,QObject *parent):QObject(parent),d(std::make_unique<Private>()) {
    options.debounceMs=std::clamp(options.debounceMs,1,60000);options.scanDelayMs=std::clamp(options.scanDelayMs,1,10000);options.retryMs=std::clamp(options.retryMs,10,60000);
    options.availabilityCheckMs=std::clamp(options.availabilityCheckMs,10,60000);
    options.maxWorktrees=std::clamp(options.maxWorktrees,1,8192);options.maxHeadBytes=std::clamp(options.maxHeadBytes,1,1024*1024);
    options.maxTotalHeadBytes=std::clamp(options.maxTotalHeadBytes,1,16*1024*1024);
    options.maxRepositories=std::clamp(options.maxRepositories,1,1024);options.maxOwners=std::clamp(options.maxOwners,1,65536);
    d->options=options;d->pool.setMaxThreadCount(2);
}
GitRepositoryWatcher::~GitRepositoryWatcher() {
    for(const auto &group:d->groups)group->cancel->store(true);
    // Workers only post to this QObject; destruction waits before its lifetime
    // ends, then QObject drops queued callbacks. No worker reads mutable state.
    d->pool.waitForDone();
}
bool GitRepositoryWatcher::acquire(const QString &ownerId,const GitSnapshot &repository,QString *error) {
    Q_ASSERT(thread()==QThread::currentThread());
    auto fail=[&](const QString &message){if(error)*error=message;return false;};
    if(ownerId.isEmpty()||ownerId.size()>4096||ownerId.contains(QChar::Null))return fail("A valid watcher owner ID is required.");
    const QString root=canonical(repository.commonGitDir),workspace=canonical(repository.repoRoot);
    if(root.isEmpty()||workspace.isEmpty()||!QFileInfo(root).isDir()||!QFileInfo(workspace).isDir())return fail("An available, authorized Git repository snapshot is required.");
    const QString groupKey=key(root);
    if(d->owners.value(ownerId)==groupKey){if(error)error->clear();return true;}
    if(!d->owners.contains(ownerId)&&d->owners.size()>=d->options.maxOwners)return fail("Repository watcher owner limit reached.");
    const auto oldGroup=d->groups.value(d->owners.value(ownerId));
    const auto retainedGroups=d->groups.size()-(oldGroup&&oldGroup->owners.size()==1?1:0);
    if(!d->groups.contains(groupKey)&&retainedGroups>=d->options.maxRepositories)return fail("Repository watcher limit reached.");
    // Validate the replacement before releasing the old subscription.
    release(ownerId);
    auto group=d->groups.value(groupKey);
    if(!group) {
        group=std::make_shared<Private::Group>();group->path=root;group->key=groupKey;group->generation=++d->generation;
        group->scope=new QObject(this);group->watcher=new QFileSystemWatcher(group->scope);
        group->scanTimer=new QTimer(group->scope);group->scanTimer->setSingleShot(true);group->scanTimer->setInterval(d->options.scanDelayMs);
        group->debounce=new QTimer(group->scope);group->debounce->setSingleShot(true);group->debounce->setInterval(d->options.debounceMs);
        group->retry=new QTimer(group->scope);group->retry->setInterval(d->options.retryMs);
        connect(group->watcher,&QFileSystemWatcher::directoryChanged,group->scope,[this,groupKey]{scheduleScan(groupKey);});
        connect(group->watcher,&QFileSystemWatcher::fileChanged,group->scope,[this,groupKey]{scheduleScan(groupKey);});
        connect(group->scanTimer,&QTimer::timeout,group->scope,[this,groupKey]{scan(groupKey);});
        connect(group->debounce,&QTimer::timeout,group->scope,[this,groupKey]{publish(groupKey);});
        connect(group->retry,&QTimer::timeout,group->scope,[this,groupKey]{const auto current=d->groups.value(groupKey);if(!current)return;if(current->complete)probeAvailability(groupKey);else scheduleScan(groupKey);});
        d->groups.insert(groupKey,group);group->watcher->addPath(root);
    }
    d->owners.insert(ownerId,groupKey);group->owners.insert(ownerId);scheduleScan(groupKey);
    if(error)error->clear();
    return true;
}
void GitRepositoryWatcher::release(const QString &ownerId) {
    Q_ASSERT(thread()==QThread::currentThread());
    const QString groupKey=d->owners.take(ownerId);auto group=d->groups.value(groupKey);if(!group)return;
    group->owners.remove(ownerId);if(!group->owners.isEmpty())return;
    group->cancel->store(true);d->groups.remove(groupKey);
    group->scanTimer->stop();group->debounce->stop();group->retry->stop();
    group->scope->disconnect();group->watcher->disconnect();
    const auto paths=group->watcher->files()+group->watcher->directories();if(!paths.isEmpty())group->watcher->removePaths(paths);
    group->scope->deleteLater();
}
void GitRepositoryWatcher::refresh(const QString &commonGitDir) {
    Q_ASSERT(thread()==QThread::currentThread());
    if(commonGitDir.isEmpty()){for(auto i=d->groups.cbegin();i!=d->groups.cend();++i)scheduleScan(i.key());}
    else {const QString resolved=canonical(commonGitDir);scheduleScan(key(resolved.isEmpty()?commonGitDir:resolved));}
}
int GitRepositoryWatcher::repositoryCount()const{return int(d->groups.size());}
int GitRepositoryWatcher::ownerCount(const QString &path)const{const QString resolved=canonical(path);const auto group=d->groups.value(key(resolved.isEmpty()?path:resolved));return group?int(group->owners.size()):0;}
QStringList GitRepositoryWatcher::watchedPaths(const QString &path)const{const QString resolved=canonical(path);const auto group=d->groups.value(key(resolved.isEmpty()?path:resolved));return group?group->watcher->files()+group->watcher->directories():QStringList();}
void GitRepositoryWatcher::scheduleScan(const QString &groupKey) {
    const auto group=d->groups.value(groupKey);if(!group)return;
    if(group->inFlight){group->rescan=true;return;}
    if(!group->scanTimer->isActive())group->scanTimer->start(d->options.scanDelayMs);
}
void GitRepositoryWatcher::probeAvailability(const QString &groupKey) {
    const auto group=d->groups.value(groupKey);
    if(!group||!group->initialized||group->inFlight||group->scanTimer->isActive())return;
    if(d->outstandingScans>=2){
        // Synchronized repository timers must not repeatedly lose to the same
        // first two groups. Keep at most one deferred probe per live owner set.
        if(!group->probeDeferred){
            group->probeDeferred=true;const auto generation=group->generation;
            QTimer::singleShot(50,group->scope,[this,groupKey,generation]{const auto current=d->groups.value(groupKey);if(!current||current->generation!=generation)return;current->probeDeferred=false;probeAvailability(groupKey);});
        }
        return;
    }
    // Windows directory watches can stay attached to a renamed directory with
    // no notification for the old pathname. Check only its physical identity;
    // this is not a HEAD/status/ref scan or a watch on the workspace parent.
    group->inFlight=true;group->rescan=false;++d->outstandingScans;
    const auto generation=group->generation;const auto path=group->path;const auto cancel=group->cancel;
    d->pool.start([this,groupKey,generation,path,cancel]{
        const auto result=cancel->load()?RootProbe():rootProbe(path);
        QMetaObject::invokeMethod(this,[this,groupKey,generation,result,cancel]{
            --d->outstandingScans;
            const auto group=d->groups.value(groupKey);if(!group||group->generation!=generation||cancel->load())return;
            group->inFlight=false;
            const bool needsScan=group->rescan||result.identity!=group->rootIdentity;group->rescan=false;
            if(needsScan)scheduleScan(groupKey);
            else if(group->publishPending&&!group->debounce->isActive())publish(groupKey);
        },Qt::QueuedConnection);
    });
}
void GitRepositoryWatcher::scan(const QString &groupKey) {
    const auto group=d->groups.value(groupKey);if(!group||group->inFlight)return;
    // Released/reacquired owners must not build an unbounded queue behind a
    // slow filesystem. At most two scans (including queued deliveries) exist.
    if(d->outstandingScans>=2){group->scanTimer->start(std::max(50,d->options.scanDelayMs));return;}
    ++d->outstandingScans;
    group->inFlight=true;group->rescan=false;
    const auto options=d->options;const auto generation=group->generation;const QString path=group->path;const Cancel cancel=group->cancel;
    d->pool.start([this,groupKey,generation,path,options,cancel] {
        const ScanResult result=fingerprint(path,options,cancel);
        QMetaObject::invokeMethod(this,[this,groupKey,generation,result] {
            --d->outstandingScans;
            const auto group=d->groups.value(groupKey);if(!group||group->generation!=generation||result.cancelled)return;
            group->inFlight=false;
            if(group->rootIdentity!=result.rootIdentity){
                const auto oldPaths=group->watcher->files()+group->watcher->directories();
                if(!oldPaths.isEmpty())group->watcher->removePaths(oldPaths);
                group->rootIdentity=result.rootIdentity;
            }
            QSet<QString> desired(result.paths.begin(),result.paths.end());
            const QStringList current=group->watcher->files()+group->watcher->directories();QStringList remove,add;
            for(const auto &path:current)if(!desired.remove(path))remove.append(path);
            if(!remove.isEmpty())group->watcher->removePaths(remove);
            for(const auto &path:desired)add.append(path);
            const QStringList failures=add.isEmpty()?QStringList():group->watcher->addPaths(add);
            const bool complete=result.complete&&failures.isEmpty();
            const QString reason=!result.complete?result.reason:failures.isEmpty()?QString():"Native watches could not be attached to all repository metadata paths.";
            const bool first=!group->initialized;
            const bool coverageChangedNow=first||group->complete!=complete||group->coverageReason!=reason;
            group->complete=complete;group->coverageReason=reason;
            const int retryInterval=complete?d->options.availabilityCheckMs:d->options.retryMs;
            if(group->retry->interval()!=retryInterval)group->retry->setInterval(retryInterval);
            if(!group->retry->isActive())group->retry->start();
            if(first){group->initialized=true;group->latest=result.fingerprint;group->published=result.fingerprint;}
            else if(group->latest!=result.fingerprint){group->latest=result.fingerprint;group->debounce->start();group->publishPending=false;}
            // Close the scan-to-watch-attachment gap once new paths are attached.
            const bool rescan=group->rescan||add.size()>failures.size();group->rescan=false;
            QPointer<GitRepositoryWatcher> guard(this);
            if(coverageChangedNow){emit coverageChanged(group->path,complete,reason);if(!guard||!d->groups.contains(groupKey)||d->groups.value(groupKey)!=group)return;}
            if(first){emit ready(group->path);if(!guard||!d->groups.contains(groupKey)||d->groups.value(groupKey)!=group)return;}
            if(group->publishPending&&!group->debounce->isActive()){publish(groupKey);if(!guard||!d->groups.contains(groupKey)||d->groups.value(groupKey)!=group)return;}
            if(rescan)scheduleScan(groupKey);
        },Qt::QueuedConnection);
    });
}
void GitRepositoryWatcher::publish(const QString &groupKey) {
    const auto group=d->groups.value(groupKey);if(!group||!group->initialized)return;
    if(group->inFlight||group->scanTimer->isActive()){group->publishPending=true;return;}
    group->publishPending=false;
    if(group->published==group->latest)return;
    group->published=group->latest;const auto revision=++group->revision;
    emit topologyChanged(group->path,revision);
}
}
