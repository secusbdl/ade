#include "HarnessWorkspace.h"
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <vector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace ade {
HarnessWorkspace HarnessWorkspace::resolve(const QString& requested) {
  HarnessWorkspace result;
  if(requested.isEmpty()||requested.contains(QChar::Null))return result;
#ifdef Q_OS_WIN
  const auto native=QDir::toNativeSeparators(QFileInfo(requested).absoluteFilePath()).toStdWString();
  // Keep the physical root alive while its CLI session is owned. Descendant
  // writes remain possible; deletion/rename of the root is blocked on Windows.
  HANDLE handle=CreateFileW(native.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
  if(handle==INVALID_HANDLE_VALUE)return result;
  auto pin=std::shared_ptr<void>(handle,[](void* value){CloseHandle(static_cast<HANDLE>(value));});
  BY_HANDLE_FILE_INFORMATION info{};
  if(!GetFileInformationByHandle(handle,&info)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))return result;
  FILE_ID_INFO identity{};
  if(!GetFileInformationByHandleEx(handle,FileIdInfo,&identity,sizeof(identity)))return result;
  const auto count=GetFinalPathNameByHandleW(handle,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
  if(!count||count>32768)return result;
  std::vector<wchar_t> buffer(count+1);
  const auto length=GetFinalPathNameByHandleW(handle,buffer.data(),DWORD(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
  if(!length||length>=buffer.size())return result;
  auto path=QString::fromWCharArray(buffer.data(),int(length));
  if(path.startsWith("\\\\?\\UNC\\"))path="\\\\"+path.mid(8);else if(path.startsWith("\\\\?\\"))path.remove(0,4);
  result.path=QDir::cleanPath(QDir::fromNativeSeparators(path));
  const QByteArray id(reinterpret_cast<const char*>(identity.FileId.Identifier),sizeof(identity.FileId.Identifier));
  result.identity=QString::number(identity.VolumeSerialNumber,16)+":"+QString::fromLatin1(id.toHex())+":"+QString::number(info.ftCreationTime.dwHighDateTime,16)+":"+QString::number(info.ftCreationTime.dwLowDateTime,16);
  result.pin=std::move(pin);
#else
  const QFileInfo info(requested);
  if(!info.isDir())return result;
  result.path=info.canonicalFilePath();
  result.identity=result.path+":"+QString::number(info.birthTime().toMSecsSinceEpoch());
#endif
  return result;
}
}
