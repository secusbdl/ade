#include "BrowserSessionStore.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <memory>
#include <vector>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace ade {
namespace {
struct DirectoryPin {
    QString path;
    QByteArray identity;
    std::shared_ptr<void> handle;
};
bool pin(const QString& path, DirectoryPin* result) {
#ifdef Q_OS_WIN
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    const auto handle = CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    result->handle = std::shared_ptr<void>(handle, [](void* p) { CloseHandle(p); });
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    const auto size = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!size || size > 32768) return false;
    std::vector<wchar_t> buffer(size + 1);
    const auto count = GetFinalPathNameByHandleW(handle, buffer.data(), DWORD(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!count || count >= buffer.size()) return false;
    auto finalPath = QString::fromWCharArray(buffer.data(), int(count));
    if (finalPath.startsWith("\\\\?\\UNC\\")) finalPath = "\\\\" + finalPath.mid(8);
    else if (finalPath.startsWith("\\\\?\\")) finalPath.remove(0, 4);
    result->path = QDir::cleanPath(QDir::fromNativeSeparators(finalPath));
    result->identity = QByteArray::number(info.dwVolumeSerialNumber) + ':'
        + QByteArray::number(info.nFileIndexHigh) + ':' + QByteArray::number(info.nFileIndexLow);
#else
    result->path = QFileInfo(path).canonicalFilePath();
    result->identity = result->path.toUtf8();
#endif
    return !result->path.isEmpty();
}
bool read(const QString& path, bool* exists, QByteArray* bytes, QString* error) {
    const QFileInfo info(path);
    *exists = info.exists() || info.isSymLink();
    bytes->clear();
    if (!*exists) return true;
    if (info.isSymLink() || !info.isFile()) {
        if (error) *error = "Browser tab storage is not a regular file. Original data was preserved.";
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > BrowserSessionStore::MaximumBytes) {
        if (error) *error = "Cannot read browser tab storage, or it exceeds 1 MiB. Original data was preserved.";
        return false;
    }
    *bytes = file.read(BrowserSessionStore::MaximumBytes + 1);
    if (bytes->size() > BrowserSessionStore::MaximumBytes || file.error() != QFileDevice::NoError) {
        if (error) *error = "Browser tab storage changed or could not be read. Original data was preserved.";
        return false;
    }
    return true;
}
QByteArray revision(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
bool encode(const BrowserSessionState& state, QByteArray* bytes) {
    if (state.tabs.size() > BrowserSessionStore::MaximumTabs) return false;
    static const QRegularExpression idPattern("^[A-Za-z0-9_-]{1,128}$");
    QSet<QString> ids;
    QJsonArray tabs;
    for (const auto& tab : state.tabs) {
        const auto url = BrowserSessionStore::restorableUrl(tab.url);
        if (!idPattern.match(tab.id).hasMatch() || ids.contains(tab.id) || url.isEmpty() || tab.title.size() > 1024) return false;
        ids.insert(tab.id);
        tabs.append(QJsonObject{{"id", tab.id}, {"url", url.toString(QUrl::FullyEncoded)}, {"title", tab.title}});
    }
    if ((!state.tabs.isEmpty() && !ids.contains(state.activeTabId)) || (state.tabs.isEmpty() && !state.activeTabId.isEmpty())) return false;
    *bytes = QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"tabs", tabs}, {"activeTabId", state.activeTabId}}).toJson(QJsonDocument::Compact);
    return bytes->size() <= BrowserSessionStore::MaximumBytes;
}
}
BrowserSessionStore::BrowserSessionStore(QString directory) : m_directory(std::move(directory)) {}
QUrl BrowserSessionStore::restorableUrl(const QUrl& input) {
    if (!input.isValid() || input.isRelative() || input.isEmpty()) return {};
    QUrl url(input);
    if (url.scheme() == "about") return url == QUrl("about:blank") ? url : QUrl();
    if ((url.scheme() != "https" && url.scheme() != "http") || url.host().isEmpty()) return {};
    url.setUserInfo({});
    return url.toEncoded().size() <= 8192 ? url : QUrl();
}
BrowserSessionState BrowserSessionStore::load(QString* error) {
    if (error) error->clear();
    m_loaded = true; m_valid = false; m_existed = false; m_revision.clear(); m_root.clear(); m_identity.clear();
    DirectoryPin directory;
    if (!QDir().mkpath(m_directory) || !pin(m_directory, &directory)) {
        if (error) *error = "Cannot open browser tab storage directory.";
        return {};
    }
    m_root = directory.path; m_identity = directory.identity;
    QByteArray bytes;
    if (!read(m_root + "/browser-tabs.json", &m_existed, &bytes, error)) return {};
    m_revision = revision(bytes);
    if (!m_existed) { m_valid = true; return {}; }
    const auto invalid = [&]() {
        if (error) *error = "Browser tab storage has an invalid or unsupported format. Original data was preserved.";
        return BrowserSessionState{};
    };
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) return invalid();
    const auto object = document.object();
    if (object.value("schemaVersion") != QJsonValue(1) || !object.value("tabs").isArray() || !object.value("activeTabId").isString()) return invalid();
    const auto tabs = object.value("tabs").toArray();
    if (tabs.size() > MaximumTabs) return invalid();
    BrowserSessionState state;
    state.activeTabId = object.value("activeTabId").toString();
    for (const auto& entry : tabs) {
        if (!entry.isObject()) return invalid();
        const auto tab = entry.toObject();
        if (!tab.value("id").isString() || !tab.value("url").isString() || !tab.value("title").isString()) return invalid();
        const auto text = tab.value("url").toString();
        if (text.size() > 8192) return invalid();
        const auto url = restorableUrl(QUrl(text, QUrl::StrictMode));
        if (url.isEmpty()) return invalid();
        state.tabs.append({tab.value("id").toString(), url, tab.value("title").toString()});
    }
    QByteArray validated;
    if (!encode(state, &validated)) return invalid();
    m_valid = true;
    return state;
}
bool BrowserSessionStore::save(const BrowserSessionState& state, QString* error) {
    if (error) error->clear();
    if (!m_loaded) load(error);
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (!m_valid) return fail("Browser tab storage failed validation. Original data was preserved.");
    QByteArray bytes;
    if (!encode(state, &bytes)) return fail("Browser tabs exceed their storage limits or contain an invalid record. Original data was preserved.");
    DirectoryPin directory;
    if (!pin(m_directory, &directory) || directory.path != m_root || directory.identity != m_identity)
        return fail("Browser tab storage directory changed. Reopen the window before saving.");
    const auto path = m_root + "/browser-tabs.json";
    QLockFile lock(path + ".lock");
    if (!lock.tryLock(0)) return fail("Another window is saving browser tabs. Retry shortly.");
    bool exists = false;
    QByteArray previous;
    if (!read(path, &exists, &previous, error)) return false;
    if (exists != m_existed || revision(previous) != m_revision)
        return fail("Browser tabs changed in another window. Saved tabs were preserved; reopen before saving.");
    if (exists && previous == bytes) return true;
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
        return fail("Could not save browser tabs. Original data was preserved.");
    m_revision = revision(bytes); m_existed = true;
    return true;
}
}
