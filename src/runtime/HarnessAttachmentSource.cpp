#include "HarnessAttachmentSource.h"
#include "harness/HarnessAttachmentStore.h"
#include <QCryptographicHash>
#include <QDir>
#include <QRegularExpression>
#include <vector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace ade {
namespace {
using Status = HarnessAttachmentAvailability;
HarnessAttachmentRead invalid(const QString &why) {
  return {Status::Invalid, {}, why};
}
bool token(const QString &s) {
  return QRegularExpression("^[A-Za-z0-9_-]{1,128}$").match(s).hasMatch();
}
bool component(const QString &s) {
  return !s.isEmpty() && s != "." && s != ".." && !s.endsWith('.') &&
         !s.endsWith(' ') &&
         !s.contains(QRegularExpression("[\\x00-\\x1f<>:\"/\\\\|?*]")) &&
         !QRegularExpression(
              "^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$",
              QRegularExpression::CaseInsensitiveOption)
              .match(s)
              .hasMatch();
}
bool reference(const QString &path) {
  const auto parts = path.split('/');
  if (path.size() > 32768 || parts.size() < 2 || !token(parts.first()))
    return false;
  for (const auto &p : parts)
    if (!component(p))
      return false;
  return parts.first() != "att" ||
         (parts.size() == 2 && parts.last().size() <= 180 &&
          QRegularExpression("^[A-Za-z0-9_.-]+$")
              .match(parts.last())
              .hasMatch());
}
#ifdef Q_OS_WIN
QString finalPath(HANDLE handle) {
  const auto count = GetFinalPathNameByHandleW(
      handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (!count || count > 32768)
    return {};
  std::vector<wchar_t> buffer(count + 1);
  const auto length =
      GetFinalPathNameByHandleW(handle, buffer.data(), DWORD(buffer.size()),
                                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (!length || length >= buffer.size())
    return {};
  auto path = QString::fromWCharArray(buffer.data(), int(length));
  if (path.startsWith("\\\\?\\UNC\\"))
    path = "\\\\" + path.mid(8);
  else if (path.startsWith("\\\\?\\"))
    path.remove(0, 4);
  return QDir::cleanPath(QDir::fromNativeSeparators(path));
}
QString nativePath(const QString &path) {
  auto native = QDir::toNativeSeparators(path);
  return native.startsWith("\\\\") ? "\\\\?\\UNC\\" + native.mid(2)
                                   : "\\\\?\\" + native;
}
#endif
HarnessWorkspace pinScope(const QString &requested) {
  auto scope = HarnessWorkspace::resolve(requested);
  if (scope.path.isEmpty() ||
      scope.path.compare(QDir::cleanPath(QDir::fromNativeSeparators(requested)),
                         Qt::CaseInsensitive) != 0)
    return {};
#ifdef Q_OS_WIN
  // READ_ATTRIBUTES alone does not deny a directory rename on supported Windows
  // builds. Retain a read handle with no delete sharing for this entire job.
  const auto native = nativePath(scope.path).toStdWString();
  HANDLE handle = CreateFileW(
      native.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return {};
  auto pin = std::shared_ptr<void>(
      handle, [](void *p) { CloseHandle(static_cast<HANDLE>(p)); });
  BY_HANDLE_FILE_INFORMATION info{};
  FILE_ID_INFO id{};
  if (!GetFileInformationByHandle(handle, &info) ||
      !GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) ||
      !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
      (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
      finalPath(handle).compare(scope.path, Qt::CaseInsensitive) != 0)
    return {};
  const auto identity =
      QString::number(id.VolumeSerialNumber, 16) + ":" +
      QString::fromLatin1(
          QByteArray(reinterpret_cast<const char *>(id.FileId.Identifier),
                     sizeof(id.FileId.Identifier))
              .toHex()) +
      ":" + QString::number(info.ftCreationTime.dwHighDateTime, 16) + ":" +
      QString::number(info.ftCreationTime.dwLowDateTime, 16);
  if (identity != scope.identity)
    return {};
  scope.pin = std::move(pin);
  return scope;
#else
  return {};
#endif
}
HarnessAttachmentRead read(const HarnessWorkspace &scope,
                           const QStringList &parts,
                           const std::atomic_bool *cancelled) {
  const auto stopped = [&] { return cancelled && cancelled->load(); };
  if (stopped())
    return invalid("Attachment preparation was cancelled.");
  if (scope.path.isEmpty() || scope.identity.isEmpty() || parts.isEmpty())
    return invalid("The captured attachment scope is unavailable.");
  for (const auto &part : parts)
    if (!component(part))
      return invalid("Invalid captured attachment path.");
#ifdef Q_OS_WIN
  const auto current = HarnessWorkspace::resolve(scope.path);
  if (!scope.pin || !current.pin || current.identity != scope.identity ||
      current.path.compare(scope.path, Qt::CaseInsensitive) != 0)
    return invalid("The captured attachment scope changed physical identity.");
  std::vector<std::shared_ptr<void>> pins;
  pins.push_back(current.pin);
  QString parent = current.path;
  for (qsizetype i = 0; i < parts.size(); ++i) {
    if (stopped())
      return invalid("Attachment preparation was cancelled.");
    const bool leaf = i == parts.size() - 1;
    const auto path = QDir(parent).filePath(parts[i]);
    const auto native = nativePath(path).toStdWString();
    const auto handle = CreateFileW(
        native.c_str(), GENERIC_READ,
        leaf ? FILE_SHARE_READ : (FILE_SHARE_READ | FILE_SHARE_WRITE), nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS |
            (leaf ? FILE_FLAG_SEQUENTIAL_SCAN : 0),
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      const auto code = GetLastError();
      if ((code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) &&
          finalPath(static_cast<HANDLE>(pins.back().get()))
                  .compare(parent, Qt::CaseInsensitive) == 0)
        return {Status::Missing, {}, {}};
      return invalid("Cannot open the scoped attachment (Windows error " +
                     QString::number(code) + ").");
    }
    auto pin = std::shared_ptr<void>(
        handle, [](void *p) { CloseHandle(static_cast<HANDLE>(p)); });
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        finalPath(handle).compare(path, Qt::CaseInsensitive) != 0)
      return invalid("Attachment path is linked or changed physical identity.");
    if (!leaf) {
      if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return invalid("An attachment parent is not a directory.");
      pins.push_back(std::move(pin));
      parent = path;
      continue;
    }
    LARGE_INTEGER size{};
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        GetFileType(handle) != FILE_TYPE_DISK ||
        !GetFileSizeEx(handle, &size) || size.QuadPart < 0 ||
        size.QuadPart > HarnessAttachmentStore::MaxFileBytes)
      return invalid("Attachment is not a regular file within its read bound.");
    QByteArray bytes("");
    bytes.reserve(qsizetype(size.QuadPart));
    QByteArray chunk(128 * 1024, Qt::Uninitialized);
    while (true) {
      if (stopped())
        return invalid("Attachment preparation was cancelled.");
      DWORD count = 0;
      if (!ReadFile(handle, chunk.data(), DWORD(chunk.size()), &count, nullptr))
        return invalid("Cannot read complete attachment bytes.");
      if (!count)
        break;
      if (bytes.size() + count > HarnessAttachmentStore::MaxFileBytes)
        return invalid("Attachment exceeded its read bound.");
      bytes.append(chunk.constData(), count);
    }
    return {Status::Available, std::move(bytes), {}};
  }
#else
  Q_UNUSED(parts)
#endif
  return invalid(
      "Validated attachment availability is unsupported on this platform.");
}
} // namespace
HarnessAttachmentSource::HarnessAttachmentSource(const QString &profile,
                                                 const QString &owner,
                                                 const AttachmentMounts &mounts)
    : m_owner(owner) {
  if (token(owner)) {
    m_profile = pinScope(profile);
    for (auto it = mounts.begin(); it != mounts.end(); ++it) {
      if (it.key() == "att" || !token(it.key()))
        continue;
      auto scope = pinScope(it.value());
      m_mounts.insert(it.key(), scope);
    }
  }
}
HarnessAttachmentRead
HarnessAttachmentSource::snapshot(const AttachmentRef &ref, const QString &hash,
                                  const std::atomic_bool *cancelled) const {
  if (!token(m_owner) || !reference(ref.path) ||
      !QRegularExpression("^[0-9a-f]{64}$").match(hash).hasMatch())
    return invalid("Invalid captured attachment identity.");
  const auto name =
      QString::fromLatin1(
          QCryptographicHash::hash(ref.path.toUtf8() + '\0' + hash.toLatin1(),
                                   QCryptographicHash::Sha256)
              .toHex()) +
      ".bin";
  auto result =
      read(m_profile, {"attachment-snapshots", m_owner, name}, cancelled);
  if (result.status == Status::Available &&
      QString::fromLatin1(
          QCryptographicHash::hash(result.bytes, QCryptographicHash::Sha256)
              .toHex()) != hash)
    return invalid("Attachment snapshot content failed its hash check.");
  return result;
}
HarnessAttachmentRead
HarnessAttachmentSource::original(const AttachmentRef &ref,
                                  const std::atomic_bool *cancelled) const {
  if (!token(m_owner) || m_profile.path.isEmpty() || !reference(ref.path))
    return invalid("Invalid captured attachment identity.");
  auto parts = ref.path.split('/');
  const auto mount = parts.takeFirst();
  if (mount == "att") {
    parts.prepend(m_owner);
    parts.prepend("attachments");
    return read(m_profile, parts, cancelled);
  }
  return read(m_mounts.value(mount), parts, cancelled);
}
} // namespace ade
