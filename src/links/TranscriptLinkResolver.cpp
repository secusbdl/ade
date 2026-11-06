#include "TranscriptLinkResolver.h"
#include "core/ScopedDirectory.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QStringDecoder>
#include <vector>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ade {
namespace {
bool failure(QString* error, const QString& message) {
    if (error)
        *error = message;
    return false;
}
bool safeText(const QString& text) {
    for (qsizetype i = 0; i < text.size(); ++i) {
        const auto ch = text[i];
        if (ch.unicode() < 32 || ch.unicode() == 127)
            return false;
        if (ch.isHighSurrogate()) {
            if (i + 1 == text.size() || !text[++i].isLowSurrogate())
                return false;
        } else if (ch.isLowSurrogate())
            return false;
    }
    return true;
}
bool decode(const QString& encoded, QString* result) {
    auto bytes = encoded.toUtf8();
    QByteArray decoded;
    decoded.reserve(bytes.size());
    const auto digit = [](char value) {
        if (value >= '0' && value <= '9')
            return value - '0';
        if (value >= 'A' && value <= 'F')
            return value - 'A' + 10;
        if (value >= 'a' && value <= 'f')
            return value - 'a' + 10;
        return -1;
    };
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        if (bytes[i] != '%') {
            decoded += bytes[i];
            continue;
        }
        if (i + 2 >= bytes.size())
            return false;
        const int a = digit(bytes[i + 1]), b = digit(bytes[i + 2]);
        if (a < 0 || b < 0)
            return false;
        decoded += char(a * 16 + b);
        i += 2;
    }
    QStringDecoder converter(QStringDecoder::Utf8,
                             QStringConverter::Flag::Stateless | QStringConverter::Flag::ConvertInitialBom);
    *result = converter(decoded);
    return !converter.hasError() && safeText(*result);
}
bool webUrl(const QUrl& url) {
    return url.isValid() && (url.scheme() == "http" || url.scheme() == "https") && !url.host().isEmpty() &&
           url.userInfo().isEmpty();
}
bool prefixValid(const QString& prefix) {
    return QRegularExpression("^[A-Za-z0-9_-]{1,64}$").match(prefix).hasMatch();
}
bool segmentValid(const QString& name) {
    return !name.isEmpty() && name != "." && name != ".." && safeText(name) && !name.endsWith('.') &&
           !name.endsWith(' ') && !name.contains(QRegularExpression("[\\\\/:*?\"<>|]")) &&
           !QRegularExpression("^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$",
                               QRegularExpression::CaseInsensitiveOption)
                .match(name)
                .hasMatch();
}
QString digest(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
struct Root {
    QString path, identity, binding;
    bool readOnly = false;
};
struct Pins {
    QList<std::shared_ptr<void>> directories;
    std::shared_ptr<void> file;
};
QString directoryIdentity(const ScopedDirectorySnapshot& snapshot, QString* error) {
#ifdef Q_OS_WIN
    FILE_ID_INFO id{};
    BY_HANDLE_FILE_INFORMATION info{};
    const auto handle = static_cast<HANDLE>(snapshot.pin.get());
    if (!handle || !GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) ||
        !GetFileInformationByHandle(handle, &info)) {
        failure(error, "Cannot identify the selected directory handle.");
        return {};
    }
    QByteArray bytes(reinterpret_cast<const char*>(&id.VolumeSerialNumber), sizeof(id.VolumeSerialNumber));
    bytes.append(reinterpret_cast<const char*>(id.FileId.Identifier), sizeof(id.FileId.Identifier));
    bytes.append(reinterpret_cast<const char*>(&info.ftCreationTime), sizeof(info.ftCreationTime));
    return digest(bytes);
#else
    Q_UNUSED(snapshot)
    failure(error, "Scoped transcript file links currently require Windows handle validation.");
    return {};
#endif
}
bool pinDirectory(const QString& path, const QString& root, ScopedDirectorySnapshot* snapshot, QString* error,
                  const std::atomic_bool* canceled = nullptr) {
    bool truncated = false;
    return readScopedDirectory(path, root, snapshot, error, 0, canceled, false, &truncated);
}
bool captureRoot(const QString& path, Root* result, QString* error) {
    // Host maps already contain accepted physical roots. Do not canonicalize a
    // replacement alias into fresh authority before opening the actual handle.
    if (!QDir::isAbsolutePath(path) || !safeText(path))
        return failure(error, "Link mounts require an absolute host-approved path.");
    const auto canonical = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (canonical.isEmpty())
        return failure(error, "A selected link mount is unavailable.");
    ScopedDirectorySnapshot opened;
    if (!pinDirectory(canonical, canonical, &opened, error))
        return false;
    result->path = opened.canonicalPath;
    result->identity = directoryIdentity(opened, error);
    return !result->identity.isEmpty();
}
QString leafName(QString name) {
    name.replace('\\', '/');
    name = name.section('/', -1);
    return safeText(name) ? name.left(1024) : QString();
}
} // namespace
struct TranscriptLinkResolver::State {
    TranscriptLinkOptions options;
    QMap<QString, Root> roots;
    std::atomic_bool revoked = false;
};
bool TranscriptLinkResolver::Scope::isCurrent(const QString& expected) const {
    return state && !state->revoked.load(std::memory_order_relaxed) && state->options.agentId == expected &&
           state->options.lease.belongsTo(expected);
}
QJsonObject TranscriptLinkResolver::Scope::mountBindings() const {
    QJsonObject result;
    if (!state || !isCurrent(state->options.agentId))
        return result;
    for (auto it = state->roots.cbegin(); it != state->roots.cend(); ++it)
        if (it.key() != "att")
            result[it.key()] = it->binding;
    return result;
}
void TranscriptLinkResolver::Scope::revoke() const {
    if (state)
        state->revoked.store(true, std::memory_order_relaxed);
}
TranscriptLinkResolver::Scope TranscriptLinkResolver::create(const TranscriptLinkOptions& options,
                                                             QString* error) {
    if (error)
        error->clear();
    Scope result;
    if (options.agentId.isEmpty() || !options.lease.belongsTo(options.agentId) || options.maxFileBytes < 1 ||
        options.maxFileBytes > 64 * 1024 * 1024) {
        failure(error, "Invalid or inactive transcript link context.");
        return result;
    }
    auto state = std::make_shared<State>();
    state->options = options;
    const auto data = QFileInfo(options.dataDirectory).canonicalFilePath();
    if (data.isEmpty()) {
        failure(error, "Transcript attachment storage is unavailable.");
        return result;
    }
    state->options.dataDirectory = data;
    AttachmentStore store(data);
    const auto attachments = store.agentDirectory(options.agentId, error);
    if (attachments.isEmpty())
        return result;
    Root att;
    if (!captureRoot(attachments, &att, error))
        return result;
    att.readOnly = true;
    state->roots.insert("att", att);
    for (auto it = options.mounts.cbegin(); it != options.mounts.cend(); ++it) {
        if (it.key() == "att" || !prefixValid(it.key())) {
            failure(error, "Invalid host transcript mount name.");
            return result;
        }
        Root root;
        if (!captureRoot(it.value(), &root, error))
            return result;
        root.readOnly = options.readOnlyMounts.contains(it.key());
        root.binding = digest(options.agentId.toUtf8() + '\0' + it.key().toUtf8() + '\0' +
                              root.path.toCaseFolded().toUtf8() + '\0' + root.identity.toLatin1());
        state->roots.insert(it.key(), root);
    }
    result.state = std::move(state);
    if (!result.isCurrent(options.agentId)) {
        failure(error, "Transcript link context was revoked.");
        return {};
    }
    return result;
}
TranscriptLinkResolver::Parsed TranscriptLinkResolver::parse(const QString& href, QString* error) {
    if (error)
        error->clear();
    Parsed result;
    const auto reject = [&](const QString& reason) {
        failure(error, reason);
        return Parsed{};
    };
    if (href.isEmpty() || href.size() > 16384 || !safeText(href))
        return reject("Invalid or oversized transcript link.");
    const int colon = href.indexOf(':');
    if (colon <= 0)
        return reject("Transcript links require a supported scheme.");
    const auto scheme = href.left(colon);
    QString rest = href.mid(colon + 1);
    if (scheme == "http" || scheme == "https") {
        result.url = QUrl(href, QUrl::StrictMode);
        if (!webUrl(result.url))
            return reject("Invalid web link.");
        result.kind = Kind::ExternalUrl;
        return result;
    }
    if (scheme == "color") {
        if (!decode(rest, &result.color) || result.color.isEmpty() || result.color.size() > 1024)
            return reject("Invalid color link.");
        result.kind = Kind::CopyColor;
        return result;
    }
    if (scheme == "tab") {
        if (rest.isEmpty() || rest.size() > 512)
            return reject("Invalid tab identifier.");
        result.kind = Kind::ActivateTab;
        result.tabId = rest;
        return result;
    }
    if (scheme == "att")
        rest = "att/" + rest;
    else if (scheme == "mention") {
        const int provider = rest.indexOf(':');
        if (provider > 0)
            rest = rest.mid(provider + 1);
    } else if (scheme != "path" && scheme != "workspace" && scheme != "wsfile")
        return reject("Unsupported transcript link scheme.");
    if (rest.startsWith("incomplete:")) {
        result.incomplete = true;
        rest.remove(0, 11);
    }
    const int query = rest.indexOf('?');
    const auto rawPath = query < 0 ? rest : rest.left(query);
    if (query >= 0) {
        for (const auto& pair : rest.mid(query + 1).split('&')) {
            const int equal = pair.indexOf('=');
            result.params[equal < 0 ? pair : pair.left(equal)] =
                equal < 0 ? QString("true") : pair.mid(equal + 1);
        }
    }
    if (!decode(rawPath, &result.path) || result.path.size() > 4096)
        return reject("Invalid encoded file link.");
    const int line = result.path.lastIndexOf(':');
    if (line > 0 && QRegularExpression("^[0-9]+$").match(result.path.mid(line + 1)).hasMatch()) {
        result.lineNumber = result.path.mid(line + 1);
        result.path.truncate(line);
    }
    result.directory = result.path.endsWith('/') || !result.path.contains('/');
    if (result.path.endsWith('/'))
        result.path.chop(1);
    const auto segments = result.path.split('/');
    if (segments.isEmpty() || !prefixValid(segments.first()))
        return reject("File link requires an authorized mount prefix.");
    for (const auto& segment : segments)
        if (!segmentValid(segment))
            return reject("File link contains an unsafe path segment.");
    if (segments.first() == "att" && segments.size() > 1 &&
        (segments.size() != 2 || segments.last().size() > 180 ||
         !QRegularExpression("^[A-Za-z0-9_.-]+$").match(segments.last()).hasMatch()))
        return reject("Attachment links require one flat blob name.");
    result.kind = Kind::File;
    return result;
}
TranscriptLinkResolver::Action TranscriptLinkResolver::resolve(const Scope& scope, const QString& expected,
                                                               const QString& href,
                                                               const TranscriptLinkProvenance& provenance,
                                                               const std::atomic_bool* canceled,
                                                               QString* error) {
    if (error)
        error->clear();
    Action result;
    const auto reject = [&](const QString& reason) {
        failure(error, reason);
        return Action{};
    };
    const auto current = [&] {
        return scope.isCurrent(expected) && !(canceled && canceled->load(std::memory_order_relaxed));
    };
    if (!current())
        return reject("Transcript link context is stale, canceled, or belongs to another chat.");
    const auto parsed = parse(href, error);
    if (parsed.kind == Kind::Invalid)
        return result;
    result.owner = scope;
    result.kind = parsed.kind;
    result.params = parsed.params;
    result.lineNumber = parsed.lineNumber;
    const auto state = scope.state;
    if (parsed.kind == Kind::ExternalUrl) {
        result.url = parsed.url;
        return current() ? result : reject("Transcript link context was revoked.");
    }
    if (parsed.kind == Kind::CopyColor) {
        result.color = parsed.color;
        return current() ? result : reject("Transcript link context was revoked.");
    }
    if (parsed.kind == Kind::ActivateTab) {
        if (!provenance.browserSessionId.isEmpty() &&
            provenance.browserSessionId == state->options.browserSessionId &&
            state->options.liveTabIds.contains(parsed.tabId)) {
            result.tabId = parsed.tabId;
            return current() ? result : reject("Transcript link context was revoked.");
        }
        const auto original = provenance.historicalTabUrls.value(parsed.tabId);
        if (!webUrl(original))
            return reject("The referenced tab is unavailable and has no recorded web URL.");
        result.kind = Kind::ExternalUrl;
        result.url = original;
        return current() ? result : reject("Transcript link context was revoked.");
    }
    const auto prefix = parsed.path.section('/', 0, 0);
    if (!state->roots.contains(prefix))
        return reject("The linked mount is not authorized for this chat.");
    const auto root = state->roots.value(prefix);
    if (prefix != "att" && provenance.mountBindings[prefix].toString() != root.binding)
        return reject(
            "This message has no matching trusted workspace binding; the current workspace was not opened.");
    if (parsed.directory) {
        result.kind = Kind::NoOp;
        return current() ? result : reject("Transcript link context was revoked.");
    }
    auto held = std::make_shared<Pins>();
    ScopedDirectorySnapshot directory;
    if (!pinDirectory(root.path, root.path, &directory, error, canceled))
        return {};
    if (directoryIdentity(directory, error) != root.identity)
        return reject("The linked mount was replaced after this context was created.");
    held->directories.append(directory.pin);
    const auto relative = parsed.path.section('/', 1);
    const auto segments = relative.split('/');
    QString parent = root.path;
    for (int i = 0; i + 1 < segments.size(); ++i) {
        parent += '/' + segments[i];
        ScopedDirectorySnapshot ancestor;
        if (!pinDirectory(parent, root.path, &ancestor, error, canceled))
            return {};
        held->directories.append(ancestor.pin);
    }
#ifdef Q_OS_WIN
    const auto path = parent + '/' + segments.last();
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    HANDLE handle =
        CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return reject("Cannot open the linked file.");
    held->file = std::shared_ptr<void>(handle, [](void* value) { CloseHandle(static_cast<HANDLE>(value)); });
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(handle, &info) || !GetFileSizeEx(handle, &size) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return reject("Linked files cannot be directories or filesystem links.");
    if (size.QuadPart < 0 || size.QuadPart > state->options.maxFileBytes)
        return reject("The linked file exceeds the native preview byte limit.");
    const DWORD required =
        GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!required || required > 32768)
        return reject("Cannot validate the linked file handle.");
    std::vector<wchar_t> buffer(required + 1);
    const DWORD written = GetFinalPathNameByHandleW(handle, buffer.data(), DWORD(buffer.size()),
                                                    FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!written || written >= buffer.size())
        return reject("Cannot validate the linked file handle.");
    auto final = QString::fromWCharArray(buffer.data(), written);
    if (final.startsWith("\\\\?\\UNC\\"))
        final = "\\\\" + final.mid(8);
    else if (final.startsWith("\\\\?\\"))
        final.remove(0, 4);
    final = QDir::cleanPath(QDir::fromNativeSeparators(final));
    if (!final.startsWith(root.path.endsWith('/') ? root.path : root.path + '/', Qt::CaseInsensitive))
        return reject("The opened file is outside the selected mount.");
    QByteArray bytes("");
    bytes.reserve(qsizetype(size.QuadPart));
    QByteArray chunk(128 * 1024, Qt::Uninitialized);
    while (true) {
        if (!current())
            return reject("Transcript file read was canceled or revoked.");
        DWORD count = 0;
        if (!ReadFile(handle, chunk.data(), DWORD(chunk.size()), &count, nullptr))
            return reject("Cannot read the complete linked file.");
        if (!count)
            break;
        if (bytes.size() + count > state->options.maxFileBytes)
            return reject("The linked file exceeded its preview bound while changing.");
        bytes.append(chunk.constData(), count);
    }
    result.bytes = std::move(bytes);
    result.canonicalPath = final;
#else
    return reject("Scoped transcript file links currently require Windows handle validation.");
#endif
    if (!current())
        return reject("Transcript file read was canceled or revoked.");
    result.mountedPath = parsed.path;
    result.readOnly = root.readOnly;
    result.displayName =
        prefix == "att" ? leafName(provenance.attachmentNames.value(parsed.path)) : QString();
    if (result.displayName.isEmpty())
        result.displayName = segments.last();
    result.mimeType =
        QMimeDatabase().mimeTypeForFile(result.displayName, QMimeDatabase::MatchExtension).name();
    result.pins = std::move(held);
    return result;
}
} // namespace ade
