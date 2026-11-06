#include "WorktreeUsageIdentity.h"
#include <QDir>
#include <QFileInfo>
#include <QStringDecoder>
#include <vector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace ade {
namespace {
#ifdef Q_OS_WIN
struct Opened {HANDLE handle=INVALID_HANDLE_VALUE;QString path,token;Opened()=default;Opened(const Opened&)=delete;Opened&operator=(const Opened&)=delete;~Opened(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}};
QString key(const QString &path){return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();}
QString finalPath(HANDLE handle){const DWORD size=GetFinalPathNameByHandleW(handle,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);if(!size||size>32768)return {};std::vector<wchar_t> buffer(size+1);const auto written=GetFinalPathNameByHandleW(handle,buffer.data(),DWORD(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);if(!written||written>=buffer.size())return {};auto path=QString::fromWCharArray(buffer.data(),written);if(path.startsWith("\\\\?\\UNC\\"))path="\\\\"+path.mid(8);else if(path.startsWith("\\\\?\\"))path.remove(0,4);return QDir::cleanPath(QDir::fromNativeSeparators(path));}
bool openDirectory(const QString &path,Opened *result,QString *error){
    const auto native=QDir::toNativeSeparators(path).toStdWString();result->handle=CreateFileW(native.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if(result->handle==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(result->handle,&info)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){*error="Cannot verify a regular Git administrative directory.";return false;}
    FILE_ID_INFO identity{};
    if(!GetFileInformationByHandleEx(result->handle,FileIdInfo,&identity,sizeof(identity))){*error="The filesystem cannot provide a verified 128-bit Git directory identity.";return false;}
    const QByteArray fileId(reinterpret_cast<const char*>(identity.FileId.Identifier),sizeof(identity.FileId.Identifier));
    if(fileId==QByteArray(16,'\0')){*error="The filesystem returned an empty Git directory identity.";return false;}
    result->path=finalPath(result->handle);if(result->path.isEmpty()){*error="Cannot resolve the opened Git directory.";return false;}
    result->token=QString("%1:%2:%3:%4").arg(identity.VolumeSerialNumber).arg(QString::fromLatin1(fileId.toHex())).arg(info.ftCreationTime.dwHighDateTime).arg(info.ftCreationTime.dwLowDateTime);return true;
}
bool readPointer(const QString &path,const QString &expectedParent,QString *value,QString *error){
    Opened file;const auto native=QDir::toNativeSeparators(path).toStdWString();file.handle=CreateFileW(native.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if(file.handle==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(file.handle,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||GetFileType(file.handle)!=FILE_TYPE_DISK){*error="Cannot read a regular Git metadata pointer.";return false;}
    file.path=finalPath(file.handle);if(file.path.isEmpty()||key(QFileInfo(file.path).absolutePath())!=key(expectedParent)){*error="Git metadata pointer escaped its expected directory.";return false;}
    QByteArray bytes(4097,Qt::Uninitialized);DWORD count=0;if(!ReadFile(file.handle,bytes.data(),DWORD(bytes.size()),&count,nullptr)||count>4096){*error="Git metadata pointer exceeds its byte bound.";return false;}bytes.resize(count);
    QStringDecoder decoder(QStringDecoder::Utf8,QStringConverter::Flag::Stateless);const QString text=decoder(bytes);
    if(decoder.hasError()||text.contains(QChar::Null)){*error="Invalid UTF-8 Git metadata pointer.";return false;}
    *value=text;while(value->endsWith('\n')||value->endsWith('\r'))value->chop(1);
    if(value->isEmpty()||value->contains('\n')||value->contains('\r')){*error="Invalid Git metadata pointer lines.";return false;}return true;
}
QString resolve(const QString &base,const QString &relative){return QDir::isAbsolutePath(relative)?QDir::cleanPath(QDir::fromNativeSeparators(relative)):QDir::cleanPath(QDir(base).filePath(relative));}
#endif
}
WorktreeUsageIdentity probeWorktreeUsageIdentity(const GitSnapshot &snapshot,const QString &administrativeDirectory,QString *error){
    QString message;auto fail=[&](const QString &reason){if(error)*error=reason;return WorktreeUsageIdentity();};
    if(error)error->clear();
    if(snapshot.repoRoot.isEmpty()||snapshot.commonGitDir.isEmpty()||administrativeDirectory.isEmpty())return fail("Current authorized Git paths are required.");
#ifdef Q_OS_WIN
    Opened root,common,admin;
    if(!openDirectory(snapshot.repoRoot,&root,&message)||!openDirectory(snapshot.commonGitDir,&common,&message)||!openDirectory(administrativeDirectory,&admin,&message))return fail(message);
    if(key(root.path)!=key(snapshot.repoRoot)||key(common.path)!=key(snapshot.commonGitDir)||key(admin.path)!=key(administrativeDirectory))return fail("Git metadata paths changed after repository inspection.");
    const bool main=key(admin.path)==key(common.path);
    if(!main&&key(QFileInfo(admin.path).absolutePath())!=key(QDir(common.path).filePath("worktrees")))return fail("Administrative directory is not an immediate registered-worktree directory.");
    const QString gitFile=QDir(root.path).filePath(".git");
    if(QFileInfo(gitFile).isDir()){
        Opened gitDirectory;if(!main||!openDirectory(gitFile,&gitDirectory,&message)||key(gitDirectory.path)!=key(admin.path))return fail("Workspace Git directory does not match the inspected repository.");
    }else{
        QString pointer;if(!readPointer(gitFile,root.path,&pointer,&message))return fail(message);
        if(!pointer.startsWith("gitdir: ")||key(resolve(root.path,pointer.mid(8)))!=key(admin.path))return fail("Workspace Git pointer does not match its administrative directory.");
    }
    if(!main){
        QString commonPointer,backPointer;
        if(!readPointer(QDir(admin.path).filePath("commondir"),admin.path,&commonPointer,&message)||!readPointer(QDir(admin.path).filePath("gitdir"),admin.path,&backPointer,&message))return fail(message);
        if(key(resolve(admin.path,commonPointer))!=key(common.path)||key(resolve(admin.path,backPointer))!=key(gitFile))return fail("Linked worktree metadata relationships changed during inspection.");
    }
    WorktreeUsageIdentity identity{common.path,root.path,"win-file-id128-root-v1/"+common.token+'/'+admin.token+'/'+root.token,true};
    if(!identity.valid())return fail("Git instance identity exceeds its bounds.");
    return identity;
#else
    Q_UNUSED(snapshot);Q_UNUSED(administrativeDirectory);
    return fail("Verified physical Git instance identities are not implemented on this platform.");
#endif
}
}
