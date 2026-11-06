#include "SessionLogStore.h"
#include "core/ScopedDirectory.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QTimeZone>
#include <windows.h>
#include <vector>

namespace ade {
namespace {
using Handle = std::shared_ptr<void>;
constexpr auto Marker = "\n[log truncated: exceeded 50MB]\n";
constexpr qint64 CaptureBound = SessionLogStore::MaxLogBytes + 64;
struct Failure { QString message; };
[[noreturn]] void fail(const QString& message) { throw Failure{message}; }
void saveError(QString* error, const QString& message) { if (error) *error = message; }
HANDLE raw(const Handle& handle) { return static_cast<HANDLE>(handle.get()); }
Handle owned(HANDLE handle) {
    if (handle == INVALID_HANDLE_VALUE) fail(QString("Cannot open shell log scope (Windows %1).").arg(GetLastError()));
    return {handle, [](void* value) { CloseHandle(static_cast<HANDLE>(value)); }};
}
QString absolute(const QString& path) { return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath())); }
QString finalPath(HANDLE handle) {
    const auto size = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!size || size > 32768) fail("Cannot validate shell log path.");
    std::vector<wchar_t> buffer(size + 1);
    const auto length = GetFinalPathNameByHandleW(handle, buffer.data(), DWORD(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length || length >= buffer.size()) fail("Cannot validate shell log path.");
    auto path = QString::fromWCharArray(buffer.data(), length);
    if (path.startsWith("\\\\?\\")) path.remove(0, 4);
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}
BY_HANDLE_FILE_INFORMATION info(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION value{};
    if (!GetFileInformationByHandle(handle, &value)) fail("Cannot inspect shell log handle.");
    return value;
}
Handle directory(const QString& path) {
    auto handle = owned(CreateFileW(QDir::toNativeSeparators(path).toStdWString().c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    const auto value = info(raw(handle));
    if (!(value.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (value.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        || finalPath(raw(handle)).compare(path, Qt::CaseInsensitive)) fail("Shell log directory is a link or changed scope.");
    return handle;
}
QList<Handle> pinAbsolute(const QString& path) {
    if (!QRegularExpression("^[A-Za-z]:/").match(path).hasMatch() || path.indexOf(':', 2) >= 0) fail("Shell logs require a local drive directory.");
    QString at = path.left(3); QList<Handle> pins{directory(at)};
    for (const auto& part : path.mid(3).split('/', Qt::SkipEmptyParts)) {
        if (part == "." || part == ".." || part.endsWith('.') || part.endsWith(' ')) fail("Invalid shell log directory component.");
        if (!at.endsWith('/')) at += '/';
        at += part; pins.append(directory(at));
    }
    return pins;
}
bool validAgent(const QString& id) { return QRegularExpression("^[A-Za-z0-9_-]{1,128}$").match(id).hasMatch(); }
bool validSession(const QString& id) { return QRegularExpression("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$").match(id).hasMatch(); }
bool validName(const QString& name) { return name.endsWith(".shell.log") && validSession(name.chopped(10)); }
QString nameFor(const QString& id) { if (!validSession(id)) fail("Invalid shell session identity."); return id + ".shell.log"; }
QString pathName(const QString& path) {
    if (!path.startsWith("shells/") || !validName(path.mid(7))) fail("Shell reads require a flat shells/<session-id>.shell.log path.");
    return path.mid(7);
}
Handle openLog(const QString& path, DWORD access, DWORD disposition = OPEN_EXISTING) {
    auto handle = owned(CreateFileW(QDir::toNativeSeparators(path).toStdWString().c_str(), access,
        FILE_SHARE_READ | ((access & (GENERIC_WRITE | DELETE)) ? 0 : (FILE_SHARE_WRITE | FILE_SHARE_DELETE)), nullptr, disposition,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    const auto value = info(raw(handle));
    if (value.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) || value.nNumberOfLinks != 1
        || finalPath(raw(handle)).compare(path, Qt::CaseInsensitive)) fail("Shell log file is a link or changed scope.");
    return handle;
}
void writeAll(const Handle& handle, const QByteArray& bytes) {
    LARGE_INTEGER zero{}; if (!SetFilePointerEx(raw(handle), zero, nullptr, FILE_END)) fail("Cannot seek shell log.");
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        DWORD count = 0;
        if (!WriteFile(raw(handle), bytes.constData() + offset, DWORD(qMin<qsizetype>(65536, bytes.size() - offset)), &count, nullptr) || !count)
            fail("Cannot append shell log; storage may be full.");
        offset += count;
    }
}
QByteArray readAll(const Handle& handle, qint64 maximum, const std::atomic_bool* canceled, QDateTime* modified = nullptr) {
    if (maximum < 0 || maximum > CaptureBound) fail("Invalid shell read bound.");
    const auto value = info(raw(handle));
    const auto size = (quint64(value.nFileSizeHigh) << 32) | value.nFileSizeLow;
    if (size > quint64(maximum)) fail("Shell log exceeds the read byte bound; no partial result was returned.");
    LARGE_INTEGER zero{}; if (!SetFilePointerEx(raw(handle), zero, nullptr, FILE_BEGIN)) fail("Cannot seek shell log.");
    QByteArray bytes; bytes.reserve(qsizetype(size)); char buffer[65536];
    while (true) {
        if (canceled && canceled->load()) fail("Shell log read canceled.");
        DWORD count = 0; if (!ReadFile(raw(handle), buffer, sizeof(buffer), &count, nullptr)) fail("Cannot read shell log.");
        if (!count) break;
        if (bytes.size() > maximum - count) fail("Shell log grew beyond the read bound; no partial result was returned.");
        bytes.append(buffer, count);
    }
    if (modified) {
        ULARGE_INTEGER ticks{}; ticks.LowPart = value.ftLastWriteTime.dwLowDateTime; ticks.HighPart = value.ftLastWriteTime.dwHighDateTime;
        *modified = QDateTime::fromMSecsSinceEpoch(qint64(ticks.QuadPart / 10000) - 11644473600000LL, QTimeZone::UTC);
    }
    return bytes;
}
void deleteOpened(Handle& handle) {
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(raw(handle), FileDispositionInfo, &disposition, sizeof(disposition))) fail("Cannot remove owned shell log.");
    handle.reset();
}
QMutex registryMutex;
QHash<QString, QList<std::weak_ptr<void>>> registry;
QString keyFor(const QString& data, const QString& agent) { return absolute(data).toCaseFolded() + '\n' + agent.toCaseFolded(); }
struct Log { Handle writer; qint64 bytes = 0, lines = 0; bool truncated = false, closed = false; };
}
struct SessionLogStore::State {
    QString data, agent, root, failure;
    mutable QMutex mutex;
    QList<Handle> pins;
    std::unique_ptr<QLockFile> owner;
    QMap<QString, Log> logs;
    bool revoked = false;
};
namespace {
void ready(const auto& state) { if (!state.failure.isEmpty()) fail(state.failure); if (state.revoked) fail("Shell log context was revoked."); }
ScopedDirectorySnapshot listing(const auto& state, const std::atomic_bool* canceled = nullptr) {
    ready(state); ScopedDirectorySnapshot result; QString error;
    if (!readScopedDirectory(state.root, state.root, &result, &error, 10000, canceled, false)) fail(error);
    for (const auto& entry : result.entries) if (entry.isDirectory || entry.isReparsePoint || !validName(entry.name))
        fail("Shell log listing contains an unexpected file or directory.");
    return result;
}
QByteArray readState(const auto& state, const QString& path, qint64 maximum, const std::atomic_bool* canceled, QDateTime* modified = nullptr) {
    ready(state); if (canceled && canceled->load()) fail("Shell log read canceled.");
    const auto name = pathName(path); const auto handle = openLog(state.root + '/' + name, GENERIC_READ);
    return readAll(handle, maximum, canceled, modified);
}
}
SessionLogStore::SessionLogStore(QString dataDirectory, QString agentId) : m_state(std::make_shared<State>()) {
    auto& state = *m_state; state.data = absolute(dataDirectory); state.agent = std::move(agentId);
    try {
        if (!validAgent(state.agent)) fail("Invalid shell log owner.");
        state.pins = pinAbsolute(state.data);
        const auto base = state.data + "/shell-logs-v1";
        if (!QFileInfo::exists(base) && !QDir().mkdir(base)) fail("Cannot create shell log storage.");
        state.pins.append(directory(base));
        const auto digest = QString::fromLatin1(QCryptographicHash::hash(state.agent.toCaseFolded().toUtf8(), QCryptographicHash::Sha256).toHex());
        state.owner = std::make_unique<QLockFile>(base + "/owner-" + digest + ".lock"); state.owner->setStaleLockTime(0);
        if (!state.owner->tryLock(0)) fail("Shell logs already have an active owner.");
        state.root = base + '/' + state.agent;
        if (!QFileInfo::exists(state.root) && !QDir().mkdir(state.root)) fail("Cannot create chat shell log directory.");
        state.pins.append(directory(state.root));
    } catch (const Failure& failure) { state.failure = failure.message; state.owner.reset(); state.pins.clear(); }
    QMutexLocker lock(&registryMutex); registry[keyFor(state.data, state.agent)].append(m_state);
}
SessionLogStore::~SessionLogStore() { revoke(); }
bool SessionLogStore::create(const QString& sessionId, QString* error) {
    if (error) error->clear();
    QMutexLocker lock(&m_state->mutex);
    try { ready(*m_state); const auto name = nameFor(sessionId); if (m_state->logs.contains(sessionId)) fail("Shell session log already exists.");
        if (listing(*m_state).entries.size() >= 10000) fail("Shell log count exceeds its bound.");
        Log log; log.writer = openLog(m_state->root + '/' + name, GENERIC_READ | GENERIC_WRITE | DELETE, CREATE_NEW);
        m_state->logs.insert(sessionId, std::move(log)); return true;
    } catch (const Failure& failure) { saveError(error, failure.message); return false; }
}
bool SessionLogStore::append(const QString& sessionId, const QByteArray& bytes, QString* error) {
    if (error) error->clear();
    QMutexLocker lock(&m_state->mutex);
    try { ready(*m_state); nameFor(sessionId); auto found = m_state->logs.find(sessionId);
        if (found == m_state->logs.end() || found->closed || !found->writer) fail("Shell session log is not open.");
        if (found->truncated) return true;
        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless); const QString text = decoder(bytes); Q_UNUSED(text);
        if (decoder.hasError()) fail("Shell log input must be complete UTF-8 text.");
        const bool truncating = bytes.size() > MaxLogBytes - found->bytes;
        const auto& chunk = truncating ? QByteArray(Marker) : bytes;
        writeAll(found->writer, chunk); found->bytes += chunk.size(); found->lines += chunk.count('\n'); found->truncated = truncating;
        return true;
    } catch (const Failure& failure) { auto found = m_state->logs.find(sessionId); if (found != m_state->logs.end()) { found->closed = true; found->writer.reset(); } saveError(error, failure.message); return false; }
}
bool SessionLogStore::flush(const QString& sessionId, QString* error) {
    if (error) error->clear();
    QMutexLocker lock(&m_state->mutex);
    try { ready(*m_state); nameFor(sessionId); auto found = m_state->logs.find(sessionId);
        if (found == m_state->logs.end()) fail("Unknown shell session log.");
        if (found->writer && !FlushFileBuffers(raw(found->writer))) fail("Cannot flush shell log.");
        return true;
    } catch (const Failure& failure) { saveError(error, failure.message); return false; }
}
bool SessionLogStore::close(const QString& sessionId, QString* error) {
    if (error) error->clear();
    QMutexLocker lock(&m_state->mutex);
    try { ready(*m_state); nameFor(sessionId); auto found = m_state->logs.find(sessionId);
        if (found == m_state->logs.end()) fail("Unknown shell session log.");
        if (found->writer && !FlushFileBuffers(raw(found->writer))) fail("Cannot flush shell log.");
        found->writer.reset(); found->closed = true; return true;
    } catch (const Failure& failure) { saveError(error, failure.message); return false; }
}
QJsonObject SessionLogStore::stats(const QString& sessionId) const {
    QMutexLocker lock(&m_state->mutex); const auto value = m_state->logs.value(sessionId);
    return {{"bytesWritten", double(value.bytes)}, {"lineCount", double(value.lines)}, {"truncated", value.truncated}, {"closed", value.closed}};
}
QString SessionLogStore::rootDirectory() const { QMutexLocker lock(&m_state->mutex); return m_state->failure.isEmpty() && !m_state->revoked ? m_state->root : QString{}; }
QStringList SessionLogStore::list(QString* error) const {
    if (error) error->clear();
    QMutexLocker lock(&m_state->mutex);
    try { QStringList names; const auto snapshot = listing(*m_state); for (const auto& entry : snapshot.entries) names.append(entry.name); names.sort(); return names;
    } catch (const Failure& failure) { saveError(error, failure.message); return {}; }
}
QByteArray SessionLogStore::rawRead(const QString& path, qint64 maximum, const std::atomic_bool* canceled, QString* error) const {
    if (error) error->clear();
    QMutexLocker lock(&m_state->mutex);
    try { return readState(*m_state, path, maximum, canceled); } catch (const Failure& failure) { saveError(error, failure.message); return {}; }
}
FileReadSource SessionLogStore::formattedSource() const {
    const auto state = m_state;
    return [state](const QString& tool, const QString& path, const FileReadParams& params, FileReadCapture* result, const std::atomic_bool* canceled, QString* error) {
        if (error) error->clear();
        if (result) *result = {};
        QMutexLocker lock(&state->mutex);
        try { ready(*state); if (!result || (tool != "read" && tool != "ls")) fail("Shell logs allow read and ls only.");
            const bool directory = path == "shells" || path == "shells/";
            if (tool == "ls" && !directory) fail("Shell listing requires its flat root.");
            FileReadCapture capture; capture.tool = tool; capture.params = params; capture.directory = directory;
            capture.descriptor = {directory ? QStringLiteral("shells") : path, directory ? QStringLiteral("shells") : pathName(path), -1, {}};
            if (directory) {
                const auto snapshot = listing(*state, canceled); capture.descriptor.modified = snapshot.modified;
                capture.capturedDepth = params.preview.value_or(false) ? 0 : params.depth.value_or(2);
                if (capture.capturedDepth > 32) fail("Shell listing depth exceeds the native limit.");
                for (const auto& entry : snapshot.entries) capture.entries.append({entry.name, false, {}, {}});
            } else { capture.bytes = readState(*state, path, CaptureBound, canceled, &capture.descriptor.modified); capture.descriptor.size = capture.bytes.size(); }
            if (canceled && canceled->load()) fail("Shell log read canceled.");
            *result = std::move(capture); return true;
        } catch (const Failure& failure) { saveError(error, failure.message); return false; }
    };
}
void SessionLogStore::revoke() {
    QMutexLocker lock(&m_state->mutex); if (m_state->revoked) return; m_state->revoked = true;
    for (auto& log : m_state->logs) { if (log.writer) FlushFileBuffers(raw(log.writer)); log.writer.reset(); log.closed = true; }
    m_state->owner.reset(); m_state->pins.clear();
}
bool SessionLogStore::cloneTo(const QString& targetAgentId, QString* error) const {
    if (error) error->clear();
    QMutexLocker sourceLock(&m_state->mutex);
    try { ready(*m_state); if (targetAgentId.compare(m_state->agent, Qt::CaseInsensitive) == 0) fail("Cannot clone shell logs into their source owner.");
        SessionLogStore target(m_state->data, targetAgentId); QString failure; const auto existing = target.list(&failure);
        if (!failure.isEmpty()) fail(failure);
        if (!existing.isEmpty()) fail("Shell log clone target must be empty.");
        const auto snapshot = listing(*m_state); qint64 total = 0;
        try {
            for (const auto& entry : snapshot.entries) {
                const auto bytes = readState(*m_state, "shells/" + entry.name, CaptureBound, nullptr);
                if (total > 256 * 1024 * 1024 - bytes.size()) fail("Shell log clone exceeds its combined byte bound.");
                total += bytes.size();
                const auto id = entry.name.chopped(10); if (!target.create(id, &failure)) fail(failure);
                QMutexLocker targetLock(&target.m_state->mutex); auto& log = target.m_state->logs[id];
                writeAll(log.writer, bytes); log.bytes = bytes.size(); log.lines = bytes.count('\n'); log.truncated = bytes.endsWith(Marker);
                if (!FlushFileBuffers(raw(log.writer))) fail("Cannot flush cloned shell log.");
            }
        } catch (...) {
            QMutexLocker targetLock(&target.m_state->mutex);
            for (auto& log : target.m_state->logs) { if (log.writer) { try { deleteOpened(log.writer); } catch (const Failure&) {} } }
            throw;
        }
        return true;
    } catch (const Failure& failure) { saveError(error, failure.message); return false; }
}
bool SessionLogStore::removeAgent(const QString& data, const QString& agent, QString* error) {
    if (error) error->clear();
    QList<std::shared_ptr<State>> states;
    { QMutexLocker lock(&registryMutex); for (const auto& weak : registry.take(keyFor(data, agent))) if (auto value = weak.lock()) states.append(std::static_pointer_cast<State>(value)); }
    for (const auto& state : states) { QMutexLocker lock(&state->mutex); state->revoked = true; for (auto& log : state->logs) { log.writer.reset(); log.closed = true; } state->owner.reset(); state->pins.clear(); }
    try {
        SessionLogStore store(data, agent); QMutexLocker lock(&store.m_state->mutex); ready(*store.m_state);
        const auto snapshot = listing(*store.m_state); QList<Handle> files;
        for (const auto& entry : snapshot.entries) files.append(openLog(entry.absolutePath, GENERIC_READ | DELETE));
        for (auto& file : files) deleteOpened(file);
        return true;
    } catch (const Failure& failure) { saveError(error, failure.message); return false; }
}
}
