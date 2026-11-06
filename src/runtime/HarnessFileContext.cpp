#include "HarnessFileContext.h"
#include "harness/HarnessWorkspace.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>
#include <vector>
#include <windows.h>
namespace ade {
namespace {
using Handle = std::shared_ptr<void>;
struct Failure {
  QString message;
};
[[noreturn]] void fail(const QString &text) { throw Failure{text}; }
HANDLE raw(const Handle &h) { return static_cast<HANDLE>(h.get()); }
Handle own(HANDLE h) {
  if (h == INVALID_HANDLE_VALUE)
    fail("Cannot access the owned harness files.");
  return {h, [](void *v) { CloseHandle(static_cast<HANDLE>(v)); }};
}
QString hash(const QString &id) {
  if (!QRegularExpression("^[A-Za-z0-9_-]{1,128}$").match(id).hasMatch())
    fail("Invalid harness owner identity.");
  return QString::fromLatin1(
      QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256)
          .toHex());
}
BY_HANDLE_FILE_INFORMATION inspect(const Handle &h) {
  BY_HANDLE_FILE_INFORMATION v{};
  if (!GetFileInformationByHandle(raw(h), &v))
    fail("Cannot inspect the owned harness file.");
  return v;
}
QString identity(const Handle &h) {
  FILE_ID_INFO v{};
  if (!GetFileInformationByHandleEx(raw(h), FileIdInfo, &v, sizeof(v)))
    fail("Cannot identify the owned harness file.");
  const auto info = inspect(h);
  return QString::number(v.VolumeSerialNumber, 16) + ":" +
         QString::fromLatin1(
             QByteArray(reinterpret_cast<const char *>(v.FileId.Identifier), 16)
                 .toHex()) +
         ":" + QString::number(info.ftCreationTime.dwHighDateTime, 16) + ":" +
         QString::number(info.ftCreationTime.dwLowDateTime, 16);
}
QString finalPath(const Handle &h) {
  const auto size = GetFinalPathNameByHandleW(
      raw(h), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (!size || size > 32768)
    fail("Cannot resolve the owned harness file.");
  std::vector<wchar_t> b(size + 1);
  const auto length =
      GetFinalPathNameByHandleW(raw(h), b.data(), DWORD(b.size()),
                                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (!length || length >= b.size())
    fail("Cannot resolve the owned harness file.");
  auto path = QString::fromWCharArray(b.data(), int(length));
  if (path.startsWith("\\\\?\\UNC\\"))
    path = "\\\\" + path.mid(8);
  else if (path.startsWith("\\\\?\\"))
    path.remove(0, 4);
  return QDir::cleanPath(QDir::fromNativeSeparators(path));
}
bool validName(const QString &name) {
  static const QRegularExpression device(
      "^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$",
      QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression bad("[<>:\"/\\\\|?*\\x00-\\x1f]");
  return !name.isEmpty() && name != "." && name != ".." && name.size() <= 255 &&
         !name.endsWith('.') && !name.endsWith(' ') && !name.contains(bad) &&
         !device.match(name).hasMatch();
}
Handle open(const QString &path, bool directory, DWORD access,
            DWORD disposition = OPEN_EXISTING,
            DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE) {
  auto h = own(CreateFileW(
      QDir::toNativeSeparators(path).toStdWString().c_str(), access, share,
      nullptr, disposition,
      FILE_FLAG_OPEN_REPARSE_POINT |
          (directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL),
      nullptr));
  const auto info = inspect(h);
  if (bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory ||
      (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
      (!directory && info.nNumberOfLinks != 1) ||
      finalPath(h).compare(path, Qt::CaseInsensitive))
    fail("Harness files refuse links or changed scope.");
  return h;
}
struct Entry {
  QString name;
  bool directory;
};
QList<Entry> entries(const Handle &h) {
  QList<Entry> result;
  QByteArray b(65536, Qt::Uninitialized);
  bool first = true;
  while (true) {
    b.fill(0);
    if (!GetFileInformationByHandleEx(raw(h),
                                      first ? FileIdBothDirectoryRestartInfo
                                            : FileIdBothDirectoryInfo,
                                      b.data(), DWORD(b.size()))) {
      if (GetLastError() == ERROR_NO_MORE_FILES)
        break;
      fail("Cannot enumerate owned harness files.");
    }
    first = false;
    qsizetype offset = 0;
    while (true) {
      constexpr qsizetype header = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
      if (offset > b.size() - header)
        fail("Invalid harness directory response.");
      const auto *v =
          reinterpret_cast<const FILE_ID_BOTH_DIR_INFO *>(b.data() + offset);
      if (v->FileNameLength % 2 ||
          v->FileNameLength > b.size() - offset - header)
        fail("Invalid harness filename response.");
      const auto name =
          QString::fromWCharArray(v->FileName, int(v->FileNameLength / 2));
      if (name != "." && name != "..") {
        if (!validName(name) ||
            (v->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
          fail("Harness files refuse unexpected names or links.");
        if (result.size() >= 4096)
          fail("Harness directory exceeds 4096 entries.");
        result.append(
            {name, bool(v->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)});
      }
      if (!v->NextEntryOffset)
        break;
      if (v->NextEntryOffset < header + v->FileNameLength ||
          v->NextEntryOffset > b.size() - offset)
        fail("Invalid harness directory response.");
      offset += v->NextEntryOffset;
    }
  }
  std::sort(result.begin(), result.end(),
            [](const Entry &a, const Entry &b) { return a.name < b.name; });
  return result;
}
QString signature(const QList<Entry> &values) {
  QString result;
  for (const auto &v : values)
    result += (v.directory ? "D:" : "F:") + v.name + '\n';
  return result;
}
Handle directory(const QString &path, bool create = false, DWORD extra = 0) {
  if (create && !QFileInfo::exists(path) && !QDir().mkdir(path))
    fail("Cannot create the owned harness directory.");
  return open(path, true, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | extra);
}
struct Node {
  QString relative;
  bool directory = false;
  Handle handle;
  QString children;
  quint64 size = 0;
};
void collect(const QString &root, const QString &relative, const Handle &parent,
             QList<Node> &nodes, quint64 &bytes, bool deleting, int depth = 0) {
  if (depth > 32)
    fail("Harness files exceed 32 directory levels.");
  const auto list = entries(parent);
  const auto children = signature(list);
  if (!relative.isEmpty())
    nodes.last().children = children;
  for (const auto &entry : list) {
    if (nodes.size() >= 4096)
      fail("Harness files exceed 4096 entries.");
    const auto rel =
        relative.isEmpty() ? entry.name : relative + '/' + entry.name;
    const auto path = root + '/' + rel;
    auto file =
        entry.directory
            ? directory(path, false, deleting ? DELETE : 0)
            : open(path, false,
                   FILE_READ_ATTRIBUTES | (deleting ? DELETE : GENERIC_READ),
                   OPEN_EXISTING,
                   deleting ? FILE_SHARE_READ | FILE_SHARE_WRITE
                            : FILE_SHARE_READ);
    const auto info = inspect(file);
    const quint64 size =
        (quint64(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (!entry.directory && !deleting &&
        (size > 32 * 1024 * 1024 || size > 256 * 1024 * 1024 - bytes))
      fail("Harness clone exceeds 32 MiB per file or 256 MiB total.");
    if (!entry.directory)
      bytes += size;
    nodes.append({rel, entry.directory, file, {}, size});
    if (entry.directory)
      collect(root, rel, file, nodes, bytes, deleting, depth + 1);
  }
  if (signature(entries(parent)) != children)
    fail("Harness files changed while being captured; retry the fork.");
}
void removeHandle(Handle &h) {
  FILE_DISPOSITION_INFO info{TRUE};
  if (!SetFileInformationByHandle(raw(h), FileDispositionInfo, &info,
                                  sizeof(info)))
    fail("Cannot remove an owned harness file; cleanup can be retried.");
  h.reset();
}
void removeTree(const QString &path, const QString &expected) {
  if (!QFileInfo::exists(path))
    return;
  auto root = directory(path, false, DELETE);
  if (identity(root) != expected)
    fail("Harness cleanup refuses a replaced owner directory.");
  QList<Node> nodes;
  quint64 bytes = 0;
  collect(path, {}, root, nodes, bytes, true);
  for (qsizetype i = nodes.size(); i-- > 0;)
    removeHandle(nodes[i].handle);
  removeHandle(root);
}
void renameDirectory(const Handle &directory, const QString &target) {
  const auto name = QDir::toNativeSeparators(target).toStdWString();
  std::vector<char> b(offsetof(FILE_RENAME_INFO, FileName) +
                          (name.size() + 1) * sizeof(wchar_t),
                      0);
  auto *info = reinterpret_cast<FILE_RENAME_INFO *>(b.data());
  info->ReplaceIfExists = FALSE;
  info->FileNameLength = DWORD(name.size() * sizeof(wchar_t));
  std::copy(name.begin(), name.end(), info->FileName);
  if (!SetFileInformationByHandle(raw(directory), FileRenameInfo, info,
                                  DWORD(b.size())))
    fail("Cannot publish the harness clone without replacing data.");
}
void copyFile(const Handle &source, const QString &target,
              quint64 expectedSize) {
  auto output = open(target, false, GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                     CREATE_NEW, FILE_SHARE_READ);
  char buffer[65536];
  quint64 written = 0;
  while (true) {
    DWORD count = 0;
    if (!ReadFile(raw(source), buffer, sizeof(buffer), &count, nullptr))
      fail("Cannot read the captured harness file.");
    if (!count)
      break;
    if (count > expectedSize - written)
      fail("Harness file changed during fork.");
    DWORD actual = 0;
    if (!WriteFile(raw(output), buffer, count, &actual, nullptr) ||
        actual != count)
      fail("Cannot write the harness clone.");
    written += count;
  }
  if (written != expectedSize || !FlushFileBuffers(raw(output)))
    fail("Cannot finish the harness clone.");
}
} // namespace
struct HarnessFileContext::State {
  QString agent, category, locks, path, rootId, error;
  HarnessWorkspace dataPin;
  QList<Handle> ancestors;
  Handle categoryPin, locksPin, ownerLock, root;
  State(const QString &data, const QString &owner) : agent(owner) {
    try {
      const auto name = hash(owner);
      dataPin = HarnessWorkspace::resolve(data);
      if (dataPin.path.isEmpty())
        fail("Harness profile directory is unavailable.");
      if (!QRegularExpression("^[A-Za-z]:/").match(dataPin.path).hasMatch())
        fail("Harness context requires a local Windows profile directory.");
      QString ancestor = dataPin.path.left(3);
      ancestors.append(directory(ancestor));
      for (const auto &part :
           dataPin.path.mid(3).split('/', Qt::SkipEmptyParts)) {
        ancestor = QDir::cleanPath(ancestor + '/' + part);
        ancestors.append(directory(ancestor));
      }
      category = dataPin.path + "/harness-workspaces";
      locks = dataPin.path + "/harness-workspace-locks";
      categoryPin = directory(category, true);
      locksPin = directory(locks, true);
      ownerLock = open(locks + '/' + name + ".lock", false,
                       GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS, 0);
      path = category + '/' + name;
      root = directory(path, true);
      rootId = identity(root);
    } catch (const Failure &e) {
      error = e.message;
      root.reset();
    }
  }
};
HarnessFileContext::HarnessFileContext(const QString &data,
                                       const QString &owner)
    : m_state(std::make_unique<State>(data, owner)) {}
HarnessFileContext::~HarnessFileContext() = default;
QString HarnessFileContext::rootDirectory() const {
  return m_state->root ? m_state->path : QString{};
}
QString HarnessFileContext::error() const { return m_state->error; }
bool HarnessFileContext::cloneTo(const QString &targetAgent,
                                 QString *error) const {
  if (error)
    error->clear();
  QString stage, stageId;
  Handle stageRoot;
  try {
    if (!m_state->root)
      fail(m_state->error.isEmpty() ? "Harness context is closed."
                                    : m_state->error);
    const auto targetName = hash(targetAgent);
    if (targetAgent == m_state->agent)
      fail("Harness fork needs a distinct owner.");
    const auto target = m_state->category + '/' + targetName;
    auto targetLock = open(m_state->locks + '/' + targetName + ".lock", false,
                           GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS, 0);
    if (QFileInfo::exists(target))
      fail("Harness fork target must not already exist.");
    QList<Node> source;
    quint64 bytes = 0;
    const auto rootChildren = signature(entries(m_state->root));
    collect(m_state->path, {}, m_state->root, source, bytes, false);
    stage = m_state->category + "/.ade-clone-" +
            QUuid::createUuid().toString(QUuid::WithoutBraces);
#ifdef ADE_HARNESS_CONTEXT_TESTING
    if (beforeStageCreateForTest)
      beforeStageCreateForTest(stage);
#endif
    if (!CreateDirectoryW(
            QDir::toNativeSeparators(stage).toStdWString().c_str(), nullptr))
      fail("Cannot exclusively create the harness clone staging directory.");
    stageRoot = directory(stage, false, DELETE);
    stageId = identity(stageRoot);
    QList<Handle> targetDirectories;
    for (const auto &node : source) {
      const auto path = stage + '/' + node.relative;
      if (node.directory)
        targetDirectories.append(directory(path, true));
      else
        copyFile(node.handle, path, node.size);
    }
    for (const auto &node : source)
      if (node.directory && signature(entries(node.handle)) != node.children)
        fail("Harness directory changed during fork.");
    if (signature(entries(m_state->root)) != rootChildren)
      fail("Harness directory changed during fork.");
    targetDirectories.clear();
    renameDirectory(stageRoot, target);
    return true;
  } catch (const Failure &e) {
    stageRoot.reset();
    if (!stage.isEmpty() && !stageId.isEmpty()) {
      try {
        removeTree(stage, stageId);
      } catch (const Failure &) {
        if (error)
          *error =
              e.message + " An owned partial clone remains for later cleanup.";
        return false;
      }
    }
    if (error)
      *error = e.message;
    return false;
  }
}
bool HarnessFileContext::remove(QString *error) {
  if (error)
    error->clear();
  if (!m_state->error.isEmpty()) {
    if (error)
      *error = m_state->error;
    return false;
  }
  m_state->root.reset();
  try {
    removeTree(m_state->path, m_state->rootId);
    return true;
  } catch (const Failure &e) {
    if (error)
      *error = e.message;
    return false;
  }
}
} // namespace ade
