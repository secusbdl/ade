#pragma once
#include <QByteArray>
#include <QList>
#include <QLockFile>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <functional>
#include <memory>

class QLocalServer;
class QLocalSocket;

namespace ade {

struct InstanceRequest {
    QList<QUrl> urls;
    QString cwd;
};

// Per-data-directory single instance. The primary holds a lock file inside
// the data directory, listens on a random user-only local server name and
// publishes {v, pid, server} in <dataDir>/ade-instance.json. A second launch
// with the same data directory forwards its http(s) URLs and exits; other
// data directories (isolated --data-dir profiles) are independent.
//
// Wire frame: 4-byte big-endian length + compact JSON (at most 64 KiB):
// {"v":1,"type":"activate","cwd":"...","urls":[...]}; reply {"ok":true} or
// {"ok":false,"reason":"closing"|"busy"}. {"v":1,"type":"probe"} only asks
// whether the primary is closing: {"ok":true} or {"ok":false,"reason":"closing"}.
//
// acquire() must run on the coordinator's thread; the client side blocks.
class InstanceCoordinator final : public QObject {
    Q_OBJECT
public:
    enum class Role { Primary, Forwarded, Failed, Refused };
    explicit InstanceCoordinator(QString dataDir, QObject* parent = nullptr);
    ~InstanceCoordinator() override;

    // refuseIfRunning: return Refused instead of forwarding when another
    // instance holds this data directory. A primary that answers "closing"
    // is waited for until the deadline, as for forwarding launches.
    // A lock error other than "held by another instance" is logged and the
    // caller continues as an uncoordinated primary (Primary, !isPrimary()).
    Role acquire(const InstanceRequest& request, int timeoutMs = 5000, QString* error = nullptr, bool refuseIfRunning = false);
    // Requests received before this are acknowledged and buffered (at most 8).
    void markReady();
    // Later requests are answered {ok:false, reason:"closing"}.
    void setClosing();

    bool isPrimary() const { return m_primary; }
    bool isServing() const;
    QString serverName() const { return m_serverName; }
    QString sidecarPath() const;
    QString lockPath() const;

    // http(s) only, StrictMode, at most 2048 bytes each and 16 in total.
    // Arguments that start with '-' are skipped.
    static QList<QUrl> filterUrls(const QStringList& arguments);
    static QByteArray encodeRequest(const InstanceRequest& request);

    // Test seams.
    using ListenFunction = std::function<bool(QLocalServer& server, const QString& name)>;
    void setListenFunction(ListenFunction listen) { m_listen = std::move(listen); }
    // Returns NoError when the lock was taken.
    using LockFunction = std::function<QLockFile::LockError(QLockFile& lock)>;
    void setLockFunction(LockFunction lock) { m_tryLock = std::move(lock); }
    void setFrameTimeoutMs(int milliseconds) { m_frameTimeoutMs = milliseconds; }

    static constexpr int maxFrameBytes = 64 * 1024;
    static constexpr int maxUrls = 16;
    static constexpr int maxUrlBytes = 2048;
    static constexpr int maxBufferedRequests = 8;
    static constexpr int maxPendingConnections = 4;

signals:
    void requestReceived(ade::InstanceRequest request);

private:
    void becomePrimary();
    void acceptConnections();
    void handleFrame(QLocalSocket* socket);
    void reply(QLocalSocket* socket, const QByteArray& json);

    QString m_dataDir;
    std::unique_ptr<QLockFile> m_lock;
    QPointer<QLocalServer> m_server;
    QString m_serverName;
    ListenFunction m_listen;
    LockFunction m_tryLock;
    QList<InstanceRequest> m_buffered;
    int m_frameTimeoutMs = 2000;
    int m_openSockets = 0;
    bool m_primary = false;
    bool m_ready = false;
    bool m_closing = false;
};

} // namespace ade

Q_DECLARE_METATYPE(ade::InstanceRequest)
