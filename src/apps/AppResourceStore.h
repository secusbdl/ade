#pragma once
#include <QByteArray>
#include <QMap>
#include <QString>
#include <QUrl>

namespace ade {
struct AppResourceResponse {
    int status = 404;
    QByteArray body;
    QByteArray mimeType = "text/plain; charset=utf-8";
    QMap<QByteArray, QByteArray> headers;
};

// A single, host-selected app root. Request paths cannot choose another app,
// plugin, agent or filesystem root. No network, credentials or writes.
class AppResourceStore final {
public:
    bool configure(const QUrl& baseUrl, const QString& rootDirectory, QString* error = nullptr);
    QUrl baseUrl() const { return m_baseUrl; }
    QString rootDirectory() const { return m_root; }
    bool configured() const { return !m_root.isEmpty(); }
    AppResourceResponse read(const QUrl& url, const QByteArray& method = "GET") const;
    static bool safeSegment(const QString& segment);
    static constexpr qint64 maximumAssetBytes = 16 * 1024 * 1024;

private:
    QUrl m_baseUrl;
    QString m_root;
    QStringList m_prefix;
};
}
