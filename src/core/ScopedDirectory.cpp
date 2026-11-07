#include "ScopedDirectory.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QTimeZone>
#include <vector>
#include <utility>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace ade {
namespace {
bool contained(const QString& root,const QString& path){
#ifdef Q_OS_WIN
    constexpr auto sensitivity=Qt::CaseInsensitive;
#else
    constexpr auto sensitivity=Qt::CaseSensitive;
#endif
    return path.compare(root,sensitivity)==0||path.startsWith(root.endsWith('/')?root:root+'/',sensitivity);
}
}
bool readScopedDirectory(const QString& path,const QString& root,ScopedDirectorySnapshot* result,
                         QString* error,int maxEntries,const std::atomic_bool* cancelled,bool allowRootAlias,bool* truncated,
                         const ScopedDirectoryAdmission* admission){
    if(error)error->clear();
    if(truncated)*truncated=false;
    if(result)*result={};
    const auto fail=[&](const QString& reason){if(error)*error=reason;return false;};
    if(!result||root.isEmpty()||path.isEmpty()||maxEntries<0||maxEntries>10000)return fail("Invalid scoped directory request.");
    const auto stopped=[&]{return cancelled&&cancelled->load(std::memory_order_relaxed);};
    if(stopped())return fail("Directory read cancelled.");
    ScopedDirectorySnapshot snapshot;
    const auto accept=[&](const QString& name,bool directory,bool reparse,bool hidden){
        if(name=="."||name=="..")return 0;
        if(name.isEmpty()||name.contains(QChar::Null)||name.contains('/')||name.contains('\\'))return -1;
        if(admission&&admission->scanned&&++*admission->scanned>admission->scanLimit){if(truncated){*truncated=true;return 2;}return -2;}
        ScopedDirectoryEntry entry{name,snapshot.canonicalPath+'/'+name,directory,reparse,hidden};
        if(admission&&admission->admit&&!admission->admit(entry))return 0;
        if(snapshot.entries.size()>=maxEntries){if(truncated){*truncated=true;return 2;}return -2;}
        snapshot.entries.append(std::move(entry));return 0;
    };
#ifdef Q_OS_WIN
    const auto native=QDir::toNativeSeparators(path).toStdWString();
    const DWORD flags=FILE_FLAG_BACKUP_SEMANTICS|(allowRootAlias?0:FILE_FLAG_OPEN_REPARSE_POINT);
    HANDLE handle=CreateFileW(native.c_str(),FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,flags,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return fail("Cannot open directory for scoped enumeration.");
    snapshot.pin=std::shared_ptr<void>(handle,[](void* value){CloseHandle(static_cast<HANDLE>(value));});
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(handle,&info)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))return fail("Not a readable directory.");
    if(!allowRootAlias&&(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return fail("Directory links are not followed.");
    const DWORD size=GetFinalPathNameByHandleW(handle,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!size||size>32768)return fail("Cannot validate the opened directory path.");
    std::vector<wchar_t> pathBuffer(size+1);
    const DWORD written=GetFinalPathNameByHandleW(handle,pathBuffer.data(),DWORD(pathBuffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!written||written>=pathBuffer.size())return fail("Cannot validate the opened directory path.");
    auto resolved=QString::fromWCharArray(pathBuffer.data(),written);
    if(resolved.startsWith("\\\\?\\UNC\\"))resolved="\\\\"+resolved.mid(8);else if(resolved.startsWith("\\\\?\\"))resolved.remove(0,4);
    snapshot.canonicalPath=QDir::cleanPath(QDir::fromNativeSeparators(resolved));
    const auto canonicalRoot=QDir::cleanPath(QDir::fromNativeSeparators(root));
    if(!contained(canonicalRoot,snapshot.canonicalPath))return fail("Opened directory is outside the selected mount.");
    ULARGE_INTEGER ticks{};ticks.LowPart=info.ftLastWriteTime.dwLowDateTime;ticks.HighPart=info.ftLastWriteTime.dwHighDateTime;
    snapshot.modified=QDateTime::fromMSecsSinceEpoch(qint64(ticks.QuadPart/10000)-11644473600000LL,QTimeZone::UTC);
    QByteArray buffer(65536,Qt::Uninitialized);bool first=true,done=false;
    while(!done){
        if(stopped())return fail("Directory read cancelled.");
        buffer.fill('\0');
        if(!GetFileInformationByHandleEx(handle,first?FileIdBothDirectoryRestartInfo:FileIdBothDirectoryInfo,buffer.data(),DWORD(buffer.size()))){
            if(GetLastError()==ERROR_NO_MORE_FILES)break;
            return fail("Cannot enumerate the opened directory.");
        }
        first=false;qsizetype offset=0;
        while(true){
            if(stopped())return fail("Directory read cancelled.");
            constexpr qsizetype header=offsetof(FILE_ID_BOTH_DIR_INFO,FileName);
            if(offset>buffer.size()-header)return fail("Invalid native directory entry.");
            const auto* item=reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.constData()+offset);
            if(item->FileNameLength%sizeof(wchar_t)||item->FileNameLength>buffer.size()-offset-header)return fail("Invalid native directory filename.");
            const auto name=QString::fromWCharArray(item->FileName,int(item->FileNameLength/sizeof(wchar_t)));
            const auto accepted=accept(name,item->FileAttributes&FILE_ATTRIBUTE_DIRECTORY,item->FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT,item->FileAttributes&FILE_ATTRIBUTE_HIDDEN);
            if(accepted==-1)return fail("Invalid directory entry name.");
            if(accepted==-2)return fail("Directory enumeration exceeds its entry bound; no partial result was returned.");
            if(accepted==2){done=true;break;}
            if(!item->NextEntryOffset)break;
            if(item->NextEntryOffset<header+item->FileNameLength||item->NextEntryOffset>buffer.size()-offset)return fail("Invalid native directory offset.");
            offset+=item->NextEntryOffset;
        }
    }
#else
    const QFileInfo info(path);snapshot.canonicalPath=info.canonicalFilePath();
    if(!info.isDir()||snapshot.canonicalPath.isEmpty()||!contained(QDir::cleanPath(root),snapshot.canonicalPath))return fail("Directory is outside the selected mount.");
    if(!allowRootAlias&&(info.isSymLink()||info.isJunction()))return fail("Directory links are not followed.");
    snapshot.modified=info.lastModified().toUTC();
    QDirIterator iterator(snapshot.canonicalPath,QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot);
    while(iterator.hasNext()){
        if(stopped())return fail("Directory read cancelled.");
        iterator.next();const auto child=iterator.fileInfo();const auto accepted=accept(child.fileName(),child.isDir(),child.isSymLink()||child.isJunction(),false);
        if(accepted==-1)return fail("Invalid directory entry name.");
        if(accepted==-2)return fail("Directory enumeration exceeds its entry bound; no partial result was returned.");
        if(accepted==2)break;
    }
#endif
    *result=std::move(snapshot);return true;
}
}
