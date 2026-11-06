#include "AttachmentStore.h"
#include "core/ToolExecutor.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QTimeZone>
#include <QUuid>
#include <utility>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vector>
#endif

namespace ade {
namespace {
constexpr qint64 MaxImportBytes = 100 * 1024 * 1024;
bool validAgent(const QString &agent) { return QRegularExpression("^[A-Za-z0-9_-]{1,128}$").match(agent).hasMatch(); }
bool validBlob(const QString &name) {
    return !name.isEmpty() && name.size() <= 180 && name != "." && name != ".."
        && QRegularExpression("^[A-Za-z0-9_.-]+$").match(name).hasMatch()
        && !name.endsWith('.') && !QRegularExpression("^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$", QRegularExpression::CaseInsensitiveOption).match(name).hasMatch();
}
QString generatedName(const QString &original) {
    auto base = QFileInfo(original).completeBaseName().toLower(); base.replace(QRegularExpression("\\s+"), "_"); base.remove(QRegularExpression("[^a-z0-9_-]")); base = base.left(10);
    if (base.isEmpty()) base = "file";
    auto extension = QFileInfo(original).suffix().toLower(); extension.remove(QRegularExpression("[^a-z0-9_-]")); extension = extension.left(24);
    auto name = base + '_' + QUuid::createUuid().toString(QUuid::Id128);
    return extension.isEmpty() ? name : name + '.' + extension;
}
bool copyNew(QIODevice &source, const QString &destination, QString *error) {
    QTemporaryFile target(QFileInfo(destination).absolutePath() + "/.ade-attachment-XXXXXX");
    if (!target.open()) { *error = "Cannot create an attachment temporary file."; return false; }
    qint64 total = 0;
    while (!source.atEnd()) {
        const auto chunk = source.read(128 * 1024);
        if (chunk.isEmpty() && !source.atEnd()) { *error = "Cannot read the selected attachment."; return false; }
        total += chunk.size();
        if (total > MaxImportBytes) { *error = "Attachment exceeds the 100 MiB local import limit."; return false; }
        if (target.write(chunk) != chunk.size()) { *error = "Cannot write the complete attachment."; return false; }
    }
    if (!target.flush()) { *error = "Cannot flush the attachment."; return false; }
    target.close();
    if (!target.rename(destination)) { *error = "Cannot install the attachment without replacing an existing file."; return false; }
    target.setAutoRemove(false); return true;
}
QString errorResult(QString *error, const QString &message) { if (error) *error = message; return {}; }
bool validReference(const QString &path) {
    const auto parts = path.split('/');
    if (parts.size() < 2 || path.contains('\\') || path.contains(':') || path.contains(QChar::Null)
        || parts.contains("..") || parts.contains(".") || parts.contains(QString())) return false;
    if (!QRegularExpression("^[A-Za-z0-9_-]+$").match(parts.first()).hasMatch()) return false;
    return parts.first() != "att" || (parts.size() == 2 && validBlob(parts.last()));
}
QString snapshotName(const AttachmentRef &reference, const QString &hash) {
    return QString::fromLatin1(QCryptographicHash::hash(reference.path.toUtf8() + '\0' + hash.toLatin1(), QCryptographicHash::Sha256).toHex()) + ".bin";
}
QByteArray readScoped(const QString &path, const QString &root, qint64 maxBytes, QString *error, bool touch = false, QDateTime *modified = nullptr) {
    if (modified) *modified = {};
#ifdef Q_OS_WIN
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    HANDLE handle = CreateFileW(native.c_str(), GENERIC_READ | (touch ? FILE_WRITE_ATTRIBUTES : 0), FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { errorResult(error, "Cannot open the attachment for a scoped read."); return {}; }
    struct CloseHandleGuard { HANDLE value; ~CloseHandleGuard() { CloseHandle(value); } } close{handle};
    const DWORD size = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!size || size > 32768) { errorResult(error, "Cannot validate the opened attachment path."); return {}; }
    std::vector<wchar_t> buffer(size + 1);
    const DWORD written = GetFinalPathNameByHandleW(handle, buffer.data(), DWORD(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!written || written >= buffer.size()) { errorResult(error, "Cannot validate the opened attachment path."); return {}; }
    auto final = QString::fromWCharArray(buffer.data(), written);
    if (final.startsWith("\\\\?\\UNC\\")) final = "\\\\" + final.mid(8);
    else if (final.startsWith("\\\\?\\")) final.remove(0, 4);
    final = QDir::cleanPath(QDir::fromNativeSeparators(final));
    const auto canonicalRoot = QDir::cleanPath(QDir::fromNativeSeparators(root));
    if (!final.startsWith(canonicalRoot.endsWith('/') ? canonicalRoot : canonicalRoot + '/', Qt::CaseInsensitive)) { errorResult(error, "Opened attachment resolves outside its permitted directory."); return {}; }
    BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER bytes{};
    if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        || !GetFileSizeEx(handle, &bytes) || bytes.QuadPart > maxBytes) { errorResult(error, "Attachment exceeds its read bound or is not a regular file."); return {}; }
    QByteArray result(""); result.reserve(qsizetype(bytes.QuadPart)); QByteArray chunk(128 * 1024, Qt::Uninitialized);
    while (true) {
        DWORD count = 0;
        if (!ReadFile(handle, chunk.data(), DWORD(chunk.size()), &count, nullptr)) { errorResult(error, "Cannot read complete attachment bytes."); return {}; }
        if (!count) break;
        if (result.size() + count > maxBytes) { errorResult(error, "Attachment exceeded its read bound while changing."); return {}; }
        result.append(chunk.constData(), count);
    }
    if (touch) { FILETIME now; GetSystemTimeAsFileTime(&now); SetFileTime(handle, nullptr, nullptr, &now); }
    if (modified) {
        const quint64 ticks = (quint64(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
        *modified = QDateTime::fromMSecsSinceEpoch(qint64(ticks / 10000) - 11644473600000LL, QTimeZone::UTC);
    }
    return result;
#else
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) { errorResult(error, "Cannot read attachment bytes."); return {}; }
    const auto bytes = file.read(maxBytes + 1);
    if (bytes.size() > maxBytes || file.error() != QFileDevice::NoError) { errorResult(error, "Attachment exceeds its read bound."); return {}; }
    Q_UNUSED(root)
    Q_UNUSED(touch)
    if (modified) *modified = file.fileTime(QFileDevice::FileModificationTime).toUTC();
    return bytes;
#endif
}
}
QJsonObject AttachmentRef::toJson() const {
    QJsonObject result{{"path", path}}; if (!originalFileName.isEmpty()) result["originalFileName"] = originalFileName; return result;
}
AttachmentRef AttachmentRef::fromJson(const QJsonObject &object, QString *error) {
    if (error) error->clear();
    if (!object["path"].isString() || object["path"].toString().isEmpty() || object["path"].toString().size() > 32768
        || (object.contains("originalFileName") && (!object["originalFileName"].isString() || object["originalFileName"].toString().size() > 1024))) {
        if (error) *error = "Invalid attachment reference metadata.";
        return {};
    }
    return {object["path"].toString(), object["originalFileName"].toString()};
}
AttachmentStore::AttachmentStore(QString directory) : m_dataDirectory(std::move(directory)) {}
QString AttachmentStore::agentDirectory(const QString &agentId, QString *error, bool create) const {
    return ownedDirectory(agentId, "attachments", error, create);
}
QString AttachmentStore::ownedDirectory(const QString &agentId, const QString &category, QString *error, bool create) const {
    if (error) error->clear();
    if (!validAgent(agentId) || m_dataDirectory.isEmpty()) return errorResult(error, "Invalid attachment agent or data directory.");
    if (create && !QDir().mkpath(m_dataDirectory)) return errorResult(error, "Cannot create the attachment data directory.");
    ToolExecutor scope;
    if (!scope.setWorkspace(m_dataDirectory)) return errorResult(error, "Attachment data directory is unavailable.");
    const auto relative = category + '/' + agentId; const auto path = scope.resolvePath(relative, error);
    if (path.isEmpty()) return {};
    for (const auto &part : QStringList{category, relative}) {
        const QFileInfo info(QDir(scope.workspace()).filePath(part));
        if (info.isSymLink() || info.isJunction()) return errorResult(error, "Attachment storage cannot use links or junctions.");
    }
    if (create && !QDir().mkpath(path)) return errorResult(error, "Cannot create the agent attachment directory.");
    return path;
}
QString AttachmentStore::resolve(const QString &agentId, const AttachmentRef &reference, const AttachmentMounts &mounts, QString *error) const {
    if (error) error->clear();
    const auto path = reference.path;
    if (path.contains(QChar::Null) || path.contains('\\') || path.contains(':') || path.split('/').contains(".."))
        return errorResult(error, "Attachment reference must be an owned blob or mounted relative path.");
    const auto slash = path.indexOf('/'); if (slash <= 0) return errorResult(error, "Attachment reference requires a mount prefix.");
    const auto prefix = path.left(slash), relative = path.mid(slash + 1);
    QString root;
    if (prefix == "att") {
        if (!validBlob(relative)) return errorResult(error, "Invalid attachment blob name.");
        root = agentDirectory(agentId, error);
    } else root = mounts.value(prefix);
    if (root.isEmpty()) return errorResult(error, "Attachment mount is unavailable.");
    ToolExecutor scope;
    if (!scope.setWorkspace(root)) return errorResult(error, "Attachment directory is unavailable.");
    const auto resolved = scope.resolvePath(relative, error);
    if (resolved.isEmpty()) return {};
    const QFileInfo info(resolved);
    if (prefix == "att" && (info.isSymLink() || info.isJunction())) return errorResult(error, "Attachment blobs cannot be links.");
    if (!info.isFile()) return errorResult(error, "Attachment file is missing or is not a regular file.");
    return resolved;
}
AttachmentRef AttachmentStore::existingBlob(const QString &agentId, const QString &fileName, const QString &originalName, QString *error) const {
    AttachmentRef result{"att/" + fileName, originalName};
    return resolve(agentId, result, {}, error).isEmpty() ? AttachmentRef{} : result;
}
AttachmentRef AttachmentStore::importFile(const QString &agentId, const QString &absoluteFile, const AttachmentMounts &mounts, QString *error) const {
    if (error) error->clear();
    const QFileInfo info(absoluteFile);
    if (!validAgent(agentId) || !info.isAbsolute() || !info.isFile()) { errorResult(error, "Select an existing local attachment file for this chat."); return {}; }
    for (auto it = mounts.cbegin(); it != mounts.cend(); ++it) {
        if (it.key() == "att" || !QRegularExpression("^[A-Za-z0-9_-]+$").match(it.key()).hasMatch()) continue;
        ToolExecutor scope;
        if (scope.setWorkspace(it.value()) && !scope.resolvePath(info.absoluteFilePath()).isEmpty())
            return {it.key() + '/' + QDir(scope.workspace()).relativeFilePath(info.absoluteFilePath()), {}};
    }
    if (info.size() > MaxImportBytes) { errorResult(error, "Attachment exceeds the 100 MiB local import limit."); return {}; }
    const auto root = agentDirectory(agentId, error, true); if (root.isEmpty()) return {};
    QFile source(absoluteFile); if (!source.open(QIODevice::ReadOnly)) { errorResult(error, "Cannot read the selected attachment."); return {}; }
    const auto name = generatedName(info.fileName()); QString failure;
    if (!copyNew(source, QDir(root).filePath(name), &failure)) { errorResult(error, failure); return {}; }
    return {"att/" + name, info.fileName()};
}
AttachmentRef AttachmentStore::write(const QString &agentId, const QString &originalName, const QByteArray &bytes, QString *error) const {
    if (error) error->clear();
    if (originalName.isEmpty() || originalName.size() > 1024 || bytes.size() > MaxImportBytes) { errorResult(error, "Attachment name or bytes exceed their limit."); return {}; }
    const auto root = agentDirectory(agentId, error, true); if (root.isEmpty()) return {};
    const auto name = generatedName(originalName); QBuffer source;
    source.setData(bytes); source.open(QIODevice::ReadOnly);
    QString failure;
    if (!copyNew(source, QDir(root).filePath(name), &failure)) { errorResult(error, failure); return {}; }
    return {"att/" + name, QFileInfo(originalName).fileName()};
}
QByteArray AttachmentStore::read(const QString &agentId, const AttachmentRef &reference, const AttachmentMounts &mounts, QString *error, qint64 maxBytes, QDateTime *modified) const {
    if (error) error->clear();
    if (modified) *modified = {};
    if (maxBytes < 0 || maxBytes > MaxImportBytes) { errorResult(error, "Invalid attachment read bound."); return {}; }
    const auto path = resolve(agentId, reference, mounts, error); if (path.isEmpty()) return {};
    const auto prefix = reference.path.section('/', 0, 0);
    ToolExecutor scope; const auto directory = prefix == "att" ? agentDirectory(agentId, error) : mounts.value(prefix);
    if (!scope.setWorkspace(directory)) { errorResult(error, "Attachment scope became unavailable."); return {}; }
    return readScoped(path, scope.workspace(), maxBytes, error, false, modified);
}
bool AttachmentStore::saveSnapshot(const QString &agentId, const AttachmentRef &reference, const QByteArray &bytes, QString *error) const {
    if (error) error->clear();
    if (!validReference(reference.path) || bytes.size() > 64 * 1024 * 1024) { errorResult(error, "Invalid attachment snapshot reference or byte size."); return false; }
    const auto root = ownedDirectory(agentId, "attachment-snapshots", error, true); if (root.isEmpty()) return false;
    const auto digest = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const auto path = QDir(root).filePath(snapshotName(reference, digest));
    if (QFileInfo::exists(path)) {
        QString failure; const auto previous = readScoped(path, root, 64 * 1024 * 1024, &failure);
        if (!failure.isEmpty() || previous != bytes) { errorResult(error, "Existing attachment snapshot is corrupt; it was not overwritten."); return false; }
        return true;
    }
    const auto entries = QDir(root).entryInfoList(QDir::Files | QDir::Hidden, QDir::Time | QDir::Reversed);
    qint64 total = bytes.size(); for (const auto &entry : entries) total += entry.size();
    int count = entries.size();
    for (const auto &entry : entries) {
        if (count < 500 && total <= 512 * 1024 * 1024) break;
        if (entry.isSymLink() || !QRegularExpression("^[0-9a-f]{64}\\.bin$").match(entry.fileName()).hasMatch()) continue;
        if (QFile::remove(entry.absoluteFilePath())) { total -= entry.size(); --count; }
    }
    if (count >= 500 || total > 512 * 1024 * 1024) { errorResult(error, "Attachment snapshot cache cannot fit the captured bytes."); return false; }
    QBuffer source; source.setData(bytes); source.open(QIODevice::ReadOnly); QString failure;
    if (!copyNew(source, path, &failure)) { errorResult(error, failure); return false; }
    return true;
}
QByteArray AttachmentStore::readSnapshot(const QString &agentId, const AttachmentRef &reference, const QString &hash, QString *error) const {
    if (error) error->clear();
    if (!validReference(reference.path) || !QRegularExpression("^[0-9a-f]{64}$").match(hash).hasMatch()) { errorResult(error, "Invalid captured attachment reference or hash."); return {}; }
    const auto root = ownedDirectory(agentId, "attachment-snapshots", error, false); if (root.isEmpty()) return {};
    const auto path = QDir(root).filePath(snapshotName(reference, hash));
    if (!QFileInfo::exists(path)) { errorResult(error, "The attachment snapshot is no longer cached."); return {}; }
    auto bytes = readScoped(path, root, 64 * 1024 * 1024, error, true);
    if (QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) != hash) { errorResult(error, "Attachment snapshot content failed its hash check."); return {}; }
    return bytes;
}
bool AttachmentStore::cloneAgent(const QString &sourceAgentId, const QString &targetAgentId, QString *error) const {
    if (error) error->clear();
    if (!validAgent(sourceAgentId) || !validAgent(targetAgentId) || sourceAgentId == targetAgentId) { errorResult(error, "Attachment clone needs distinct valid chat identities."); return false; }
    QList<QPair<QString, QString>> copies;
    for (const auto &category : QStringList{"attachments", "attachment-snapshots"}) {
        const auto source = ownedDirectory(sourceAgentId, category, error, false); if (source.isEmpty()) return false;
        const auto existingTarget = ownedDirectory(targetAgentId, category, error, false); if (existingTarget.isEmpty()) return false;
        if (QFileInfo::exists(existingTarget) && !QDir(existingTarget).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).isEmpty()) { errorResult(error, "Attachment clone destination must be empty."); return false; }
        if (!QFileInfo::exists(source)) continue;
        const auto target = ownedDirectory(targetAgentId, category, error, true); if (target.isEmpty()) return false;
        if (!QDir(target).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).isEmpty()) { errorResult(error, "Attachment clone destination must be empty."); return false; }
        const auto entries = QDir(source).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const auto &entry : entries) {
            if (!validBlob(entry.fileName()) || !entry.isFile() || entry.isSymLink() || entry.isJunction()) { errorResult(error, "Attachment clone source contains an unsupported entry."); return false; }
            copies.emplaceBack(entry.absoluteFilePath(), QDir(target).filePath(entry.fileName()));
        }
    }
    QStringList created;
    for (const auto &copy : copies) {
        QString failure; const auto bytes = readScoped(copy.first, QFileInfo(copy.first).absolutePath(), MaxImportBytes, &failure);
        QBuffer file; file.setData(bytes); file.open(QIODevice::ReadOnly);
        if (!failure.isEmpty() || !copyNew(file, copy.second, &failure)) {
            for (const auto &path : created) QFile::remove(path);
            errorResult(error, failure.isEmpty() ? "Cannot read an attachment while cloning." : failure); return false;
        }
        created.append(copy.second);
    }
    return true;
}
bool AttachmentStore::removeAgent(const QString &agentId, QString *error) const {
    if (error) error->clear();
    QStringList files, directories;
    for (const auto &category : QStringList{"attachments", "attachment-snapshots"}) {
        const auto root = ownedDirectory(agentId, category, error, false); if (root.isEmpty()) return false;
        if (!QFileInfo::exists(root)) continue;
        const auto entries = QDir(root).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const auto &entry : entries) {
            if (!entry.isFile() || entry.isSymLink() || entry.isJunction()) { errorResult(error, "Attachment cleanup refuses unexpected directories or links."); return false; }
            files.append(entry.absoluteFilePath());
        }
        directories.append(root);
    }
    for (const auto &file : files) if (!QFile::remove(file)) { errorResult(error, "Cannot remove a chat attachment."); return false; }
    for (const auto &directory : directories) if (!QDir().rmdir(directory)) { errorResult(error, "Cannot remove an empty chat attachment directory."); return false; }
    return true;
}
}
