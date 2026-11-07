#include "InstanceCoordinator.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QtEndian>
#include <utility>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ade {
namespace {
constexpr qint64 maxSidecarBytes = 4096;

QByteArray frame(const QByteArray& json) {
    QByteArray bytes(4, '\0');
    qToBigEndian<quint32>(quint32(json.size()), bytes.data());
    return bytes + json;
}

QByteArray compact(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

bool validUrl(const QUrl& url) {
    const auto scheme = url.scheme().toLower();
    return url.isValid() && (scheme == "http" || scheme == "https") && !url.host().isEmpty()
        && url.toEncoded().size() <= InstanceCoordinator::maxUrlBytes;
}

bool parseRequest(const QByteArray& json, InstanceRequest* request, bool* probe) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const auto object = document.object();
    if (!object.value("v").isDouble() || object.value("v").toDouble() != 1.0) return false;
    *probe = object.value("type") == QJsonValue("probe");
    if (*probe) return true;
    if (object.value("type") != QJsonValue("activate")) return false;
    if (!object.value("cwd").isString() || !object.value("urls").isArray()) return false;
    const auto urls = object.value("urls").toArray();
    if (urls.size() > InstanceCoordinator::maxUrls) return false;
    request->cwd = object.value("cwd").toString();
    for (const auto& entry : urls) {
        if (!entry.isString()) return false;
        // Defense in depth: the server applies the client's URL policy again.
        const QUrl url(entry.toString(), QUrl::StrictMode);
        if (validUrl(url)) request->urls.append(url);
    }
    return true;
}

struct Sidecar {
    qint64 pid = 0;
    QString server;
};

bool readSidecar(const QString& path, Sidecar* sidecar) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > maxSidecarBytes) return false;
    const auto object = QJsonDocument::fromJson(file.read(maxSidecarBytes)).object();
    static const QRegularExpression name(QRegularExpression::anchoredPattern("ade-[0-9a-f]{32}"));
    const auto server = object.value("server").toString();
    const auto pid = object.value("pid").toDouble();
    if (object.value("v").toDouble() != 1.0 || !name.match(server).hasMatch() || pid < 1 || pid > 4294967295.0) return false;
    sidecar->pid = qint64(pid);
    sidecar->server = server;
    return true;
}

// Blocking read of one reply frame, for the client side.
bool readFrame(QLocalSocket& socket, int timeoutMs, QByteArray* json) {
    QByteArray buffer;
    QElapsedTimer clock;
    clock.start();
    while (true) {
        buffer += socket.readAll();
        if (buffer.size() >= 4) {
            const auto length = qFromBigEndian<quint32>(buffer.constData());
            if (length == 0 || length > quint32(InstanceCoordinator::maxFrameBytes)) return false;
            if (buffer.size() >= qsizetype(4 + length)) {
                *json = buffer.mid(4, length);
                return true;
            }
        }
        const auto remaining = timeoutMs - clock.elapsed();
        if (remaining <= 0) return false;
        if (!socket.waitForReadyRead(int(remaining)) && socket.bytesAvailable() == 0) return false;
    }
}
}

InstanceCoordinator::InstanceCoordinator(QString dataDir, QObject* parent)
    : QObject(parent), m_dataDir(QDir::cleanPath(QDir(dataDir).absolutePath())) {}

InstanceCoordinator::~InstanceCoordinator() {
    if (m_server) {
        m_server->close();
        delete m_server;
    }
    if (m_primary && !m_serverName.isEmpty()) {
        Sidecar sidecar;
        if (readSidecar(sidecarPath(), &sidecar) && sidecar.pid == QCoreApplication::applicationPid() && sidecar.server == m_serverName)
            QFile::remove(sidecarPath());
    }
    if (m_lock) m_lock->unlock();
}

QString InstanceCoordinator::sidecarPath() const { return m_dataDir + "/ade-instance.json"; }
QString InstanceCoordinator::lockPath() const { return m_dataDir + "/ade-instance.lock"; }
bool InstanceCoordinator::isServing() const { return m_server && m_server->isListening(); }

QList<QUrl> InstanceCoordinator::filterUrls(const QStringList& arguments) {
    QList<QUrl> urls;
    for (const auto& argument : arguments) {
        if (argument.startsWith('-')) continue;
        const QUrl url(argument, QUrl::StrictMode);
        if (!validUrl(url)) continue;
        if (urls.size() == maxUrls) break;
        urls.append(url);
    }
    return urls;
}

QByteArray InstanceCoordinator::encodeRequest(const InstanceRequest& request) {
    QJsonObject object{{"v", 1}, {"type", "activate"}, {"cwd", request.cwd}, {"urls", QJsonArray{}}};
    if (compact(object).size() > maxFrameBytes) object["cwd"] = QString();
    QJsonArray urls;
    for (const auto& url : request.urls) {
        if (urls.size() == maxUrls) break;
        if (!validUrl(url)) continue;
        urls.append(QString::fromUtf8(url.toEncoded()));
        object["urls"] = urls;
        // Drop URLs that would overflow the frame.
        if (compact(object).size() > maxFrameBytes) {
            urls.removeLast();
            object["urls"] = urls;
            break;
        }
    }
    return compact(object);
}

InstanceCoordinator::Role InstanceCoordinator::acquire(const InstanceRequest& request, int timeoutMs, QString* error, bool refuseIfRunning) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return Role::Failed;
    };
    if (m_primary) return Role::Primary;
    if (!QDir().mkpath(m_dataDir)) return fail("Cannot create the data directory.");
    if (!m_lock) {
        m_lock = std::make_unique<QLockFile>(lockPath());
        // Only a dead or foreign owner makes the lock stale, never its age.
        m_lock->setStaleLockTime(0);
    }
    // An exclusive launch only asks whether the primary is closing; it never
    // activates the primary's window.
    const auto payload = refuseIfRunning ? compact({{"v", 1}, {"type", "probe"}}) : encodeRequest(request);
    QElapsedTimer clock;
    clock.start();
    while (true) {
        const auto lockError = m_tryLock ? m_tryLock(*m_lock) : (m_lock->tryLock(0) ? QLockFile::NoError : m_lock->error());
        if (lockError == QLockFile::NoError) {
            becomePrimary();
            return Role::Primary;
        }
        if (lockError != QLockFile::LockFailedError) {
            // Same policy as a listen failure: start uncoordinated rather
            // than refuse to start over a lock-file problem.
            const auto message = "ADE cannot lock the data directory " + QDir::toNativeSeparators(m_dataDir)
                + "; continuing without single-instance coordination.";
            qWarning("%s", qPrintable(message));
            if (error) *error = message;
            return Role::Primary;
        }
        const auto remaining = [&] { return qMax<qint64>(0, timeoutMs - clock.elapsed()); };
        if (remaining() <= 0) break;
        // A missing sidecar means the primary is still starting (or has no
        // server); retry until the deadline.
        Sidecar sidecar;
        if (readSidecar(sidecarPath(), &sidecar)) {
#ifdef Q_OS_WIN
            // Let the primary bring its window to the foreground for us.
            if (!refuseIfRunning) AllowSetForegroundWindow(DWORD(sidecar.pid));
#endif
            QLocalSocket socket;
            socket.connectToServer(sidecar.server);
            if (socket.waitForConnected(int(qBound(qint64(1), remaining(), qint64(1000))))) {
                socket.write(frame(payload));
                if (socket.bytesToWrite() > 0) socket.waitForBytesWritten(int(qBound(qint64(1), remaining(), qint64(2000))));
                QByteArray json;
                if (readFrame(socket, int(qBound(qint64(1), remaining(), qint64(2000))), &json)) {
                    const auto answer = QJsonDocument::fromJson(json).object();
                    if (answer.value("ok").toBool()) return refuseIfRunning ? Role::Refused : Role::Forwarded;
                    // "closing" or "busy": retry; the lock may be handed over.
                }
            }
            socket.abort();
        }
        if (remaining() <= 0) break;
        QThread::msleep(ulong(qMin<qint64>(100, remaining())));
    }
    // The lock is still held by another instance.
    if (refuseIfRunning) return Role::Refused;
    return fail("ADE is already running for this data directory but did not respond.");
}

void InstanceCoordinator::becomePrimary() {
    m_primary = true;
    // A sidecar left by a crashed owner names a dead server; never let a
    // later launch read it while this primary is not (yet) listening.
    QFile::remove(sidecarPath());
    auto* server = new QLocalServer(this);
    server->setSocketOptions(QLocalServer::UserAccessOption);
    server->setMaxPendingConnections(maxPendingConnections);
    // A random name: a predictable pipe could be pre-created by another user.
    const auto name = "ade-" + QUuid::createUuid().toString(QUuid::Id128);
    const bool listening = m_listen ? m_listen(*server, name) : server->listen(name);
    if (!listening) {
        qWarning("ADE single-instance server is unavailable: %s", qPrintable(server->errorString()));
        delete server;
        return;
    }
    QSaveFile sidecar(sidecarPath());
    const auto bytes = QJsonDocument(QJsonObject{{"v", 1}, {"pid", QCoreApplication::applicationPid()}, {"server", name}}).toJson(QJsonDocument::Compact);
    if (!sidecar.open(QIODevice::WriteOnly) || sidecar.write(bytes) != bytes.size() || !sidecar.commit()) {
        qWarning("ADE single-instance sidecar could not be written: %s", qPrintable(sidecar.errorString()));
        server->close();
        delete server;
        return;
    }
    m_server = server;
    m_serverName = name;
    connect(server, &QLocalServer::newConnection, this, &InstanceCoordinator::acceptConnections);
}

void InstanceCoordinator::acceptConnections() {
    while (m_server && m_server->hasPendingConnections()) {
        auto* socket = m_server->nextPendingConnection();
        if (!socket) break;
        if (m_openSockets >= maxPendingConnections) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        ++m_openSockets;
        connect(socket, &QObject::destroyed, this, [this] { --m_openSockets; });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { handleFrame(socket); });
        // Incomplete frames are dropped without a reply.
        auto* deadline = new QTimer(socket);
        deadline->setObjectName("adeInstanceFrameDeadline");
        deadline->setSingleShot(true);
        connect(deadline, &QTimer::timeout, socket, [socket] {
            socket->abort();
            socket->deleteLater();
        });
        deadline->start(m_frameTimeoutMs);
        if (socket->bytesAvailable() > 0) handleFrame(socket);
    }
}

void InstanceCoordinator::handleFrame(QLocalSocket* socket) {
    if (socket->property("adeHandled").toBool()) {
        socket->readAll();
        return;
    }
    const auto drop = [socket] {
        socket->abort();
        socket->deleteLater();
    };
    const auto buffer = socket->property("adeFrame").toByteArray() + socket->readAll();
    if (buffer.size() > 4 + maxFrameBytes) return drop();
    if (buffer.size() < 4) {
        socket->setProperty("adeFrame", buffer);
        return;
    }
    const auto length = qFromBigEndian<quint32>(buffer.constData());
    if (length == 0 || length > quint32(maxFrameBytes)) return drop();
    if (buffer.size() < qsizetype(4 + length)) {
        socket->setProperty("adeFrame", buffer);
        return;
    }
    if (buffer.size() > qsizetype(4 + length)) return drop();
    socket->setProperty("adeFrame", QByteArray());
    socket->setProperty("adeHandled", true);
    InstanceRequest request;
    bool probe = false;
    if (!parseRequest(buffer.mid(4, length), &request, &probe)) return drop();
    if (auto* deadline = socket->findChild<QTimer*>("adeInstanceFrameDeadline")) deadline->start(m_frameTimeoutMs);
    if (m_closing) return reply(socket, compact({{"ok", false}, {"reason", "closing"}}));
    if (probe) return reply(socket, compact({{"ok", true}}));
    if (!m_ready && m_buffered.size() >= maxBufferedRequests) return reply(socket, compact({{"ok", false}, {"reason", "busy"}}));
    reply(socket, compact({{"ok", true}}));
    if (!m_ready) {
        m_buffered.append(request);
        return;
    }
    emit requestReceived(request);
}

void InstanceCoordinator::reply(QLocalSocket* socket, const QByteArray& json) {
    socket->write(frame(json));
    socket->flush();
    socket->disconnectFromServer();
}

void InstanceCoordinator::markReady() {
    if (m_ready) return;
    m_ready = true;
    const QPointer<InstanceCoordinator> self(this);
    const auto pending = std::exchange(m_buffered, {});
    for (const auto& request : pending) {
        if (!self || m_closing) return;
        emit requestReceived(request);
    }
}

void InstanceCoordinator::setClosing() {
    m_closing = true;
    m_buffered.clear();
}

} // namespace ade
