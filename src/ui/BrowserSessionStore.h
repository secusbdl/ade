#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>

namespace ade {
struct BrowserSessionTab {
    QString id;
    QUrl url;
    QString title;
    bool operator==(const BrowserSessionTab&) const = default;
};
struct BrowserSessionState {
    QList<BrowserSessionTab> tabs;
    QString activeTabId;
    bool operator==(const BrowserSessionState&) const = default;
};
// Ordinary global browser tabs only. Editor/app/terminal/side-chat state belongs
// to its own lifecycle and is never serialized as a browser URL.
class BrowserSessionStore {
public:
    static constexpr int MaximumTabs = 128;
    static constexpr int MaximumBytes = 1024 * 1024;
    explicit BrowserSessionStore(QString directory);
    BrowserSessionState load(QString* error = nullptr);
    bool save(const BrowserSessionState& state, QString* error = nullptr);
    bool existedAtLoad() const { return m_existed; }
    bool isValid() const { return m_loaded && m_valid; }
    static QUrl restorableUrl(const QUrl& url);
private:
    QString m_directory, m_root;
    QByteArray m_identity, m_revision;
    bool m_loaded = false, m_valid = false, m_existed = false;
};
}
