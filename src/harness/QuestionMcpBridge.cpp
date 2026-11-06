#include "QuestionMcpBridge.h"
#include "QuestionMcpProtocol.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QStringDecoder>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <cstring>
namespace ade {
namespace {
constexpr qsizetype HeaderLimit = 8192, BodyLimit = 256 * 1024,
                    ReplyLimit = 2 * 1024 * 1024;
bool sameSecret(const QByteArray &actual, const QByteArray &expected) {
  if (actual.size() != expected.size())
    return false;
  unsigned char difference = 0;
  for (qsizetype i = 0; i < actual.size(); ++i)
    difference |= static_cast<unsigned char>(actual[i]) ^
                  static_cast<unsigned char>(expected[i]);
  return difference == 0;
}
QByteArray cancelledBytes() {
  return QJsonDocument(QuestionMcpProtocol::cancelledResult())
      .toJson(QJsonDocument::Compact);
}
} // namespace
struct QuestionMcpBridge::Connection {
  QPointer<QTcpSocket> socket;
  QPointer<QTimer> timer;
  QByteArray bytes;
  qsizetype length = -1;
  bool waiting = false, finished = false;
};
QuestionMcpBridge::QuestionMcpBridge(QObject *parent)
    : QObject(parent), m_server(this) {
  connect(&m_server, &QTcpServer::newConnection, this,
          &QuestionMcpBridge::accept);
}
QuestionMcpBridge::~QuestionMcpBridge() { stop(false); }
QJsonObject QuestionMcpBridge::start(const QString &executable,
                                     QString *error) {
  if (error)
    error->clear();
  if (!m_config.isEmpty())
    return m_config;
  if (executable.isEmpty() || !m_server.listen(QHostAddress::LocalHost, 0)) {
    if (error)
      *error = "The native question callback could not start.";
    return {};
  }
  QByteArray secret(32, Qt::Uninitialized);
  for (int i = 0; i < 8; ++i) {
    const quint32 value = QRandomGenerator::system()->generate();
    memcpy(secret.data() + i * 4, &value, 4);
  }
  m_secret = secret.toHex();
  const auto url = QString("http://127.0.0.1:%1/tools/request-user-input")
                       .arg(m_server.serverPort());
  m_config = {
      {"name", "stagewise"},
      {"command", executable},
      {"args", QJsonArray{"--question-mcp-stdio"}},
      {"env",
       QJsonArray{
           QJsonObject{{"name", "STAGEWISE_MCP_CALLBACK_URL"}, {"value", url}},
           QJsonObject{{"name", "STAGEWISE_MCP_CALLBACK_TOKEN"},
                       {"value", QString::fromLatin1(m_secret)}}}}};
  return m_config;
}
void QuestionMcpBridge::beginTurn(const QString &turn) {
  const QPointer<QuestionMcpBridge> self(this);
  const auto epoch = m_epoch + 1;
  endTurn();
  if (self && m_epoch == epoch)
    m_turn = turn;
}
void QuestionMcpBridge::endTurn() {
  ++m_epoch;
  m_turn.clear();
  const auto token = m_pendingToken;
  const auto pending = m_pending;
  m_pending.reset();
  m_pendingToken.clear();
  m_pendingTurn.clear();
  const QPointer<QuestionMcpBridge> self(this);
  if (pending)
    reply(pending, 200, cancelledBytes());
  if (self && !token.isEmpty())
    emit questionCancelled(token);
}
void QuestionMcpBridge::revoke() { stop(true); }
void QuestionMcpBridge::stop(bool notify) {
  ++m_epoch;
  const auto token = m_pendingToken;
  m_pending.reset();
  m_pendingToken.clear();
  m_pendingTurn.clear();
  m_turn.clear();
  m_server.close();
  m_config = {};
  m_secret.clear();
  const auto connections = m_connections;
  m_connections.clear();
  const QPointer<QuestionMcpBridge> self(this);
  for (const auto &connection : connections) {
    connection->finished = true;
    if (connection->timer)
      connection->timer->stop();
    if (connection->socket) {
      disconnect(connection->socket, nullptr, this, nullptr);
      if (!notify)
        connection->socket->blockSignals(true);
      connection->socket->abort();
      if (!self)
        return;
      if (connection->socket)
        connection->socket->deleteLater();
    }
  }
  if (notify && !token.isEmpty())
    emit questionCancelled(token);
}
void QuestionMcpBridge::accept() {
  while (m_server.hasPendingConnections()) {
    auto *socket = m_server.nextPendingConnection();
    if (!socket)
      continue;
    if (m_connections.size() >= 8) {
      socket->abort();
      socket->deleteLater();
      continue;
    }
    auto connection = std::make_shared<Connection>();
    connection->socket = socket;
    socket->setReadBufferSize(HeaderLimit + BodyLimit + 1);
    auto *timer = new QTimer(socket);
    connection->timer = timer;
    timer->setSingleShot(true);
    timer->start(5000);
    m_connections.insert(socket, connection);
    connect(timer, &QTimer::timeout, this, [this, connection] {
      reply(connection, 408, "Incomplete native question request.");
    });
    connect(socket, &QTcpSocket::readyRead, this,
            [this, connection] { receive(connection); });
    connect(socket, &QTcpSocket::disconnected, this,
            [this, connection] { disconnected(connection); });
    const QPointer<QuestionMcpBridge> self(this);
    receive(connection);
    if (!self)
      return;
  }
}
void QuestionMcpBridge::reply(const std::shared_ptr<Connection> &connection,
                              int status, const QByteArray &body) {
  if (connection->finished)
    return;
  connection->finished = true;
  if (connection->timer)
    connection->timer->stop();
  if (!connection->socket)
    return;
  const auto socket = connection->socket;
  const QByteArray reason = status == 200 ? "OK" : "Rejected";
  const QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " " +
                              reason + "\r\nContent-Type: " +
                              (status == 200 ? QByteArray("application/json")
                                             : QByteArray("text/plain")) +
                              "\r\nConnection: close\r\nContent-Length: " +
                              QByteArray::number(body.size()) + "\r\n\r\n" +
                              body;
  socket->write(response);
  if (socket)
    socket->disconnectFromHost();
}
void QuestionMcpBridge::disconnected(
    const std::shared_ptr<Connection> &connection) {
  const auto socket = connection->socket;
  if (socket) {
    m_connections.remove(socket);
    socket->deleteLater();
  }
  if (connection->timer)
    connection->timer->stop();
  if (m_pending != connection)
    return;
  const auto token = m_pendingToken;
  m_pending.reset();
  m_pendingToken.clear();
  m_pendingTurn.clear();
  emit questionCancelled(token);
}
void QuestionMcpBridge::receive(const std::shared_ptr<Connection> &connection) {
  const auto socket = connection->socket;
  if (!socket || connection->finished)
    return;
  if (connection->waiting) {
    if (socket->bytesAvailable())
      reply(connection, 400, "Pipelined requests are not supported.");
    return;
  }
  const auto available = socket->bytesAvailable();
  if (available > HeaderLimit + BodyLimit - connection->bytes.size()) {
    reply(connection, 413, "Native question request exceeds its limit.");
    return;
  }
  connection->bytes +=
      socket->read(HeaderLimit + BodyLimit - connection->bytes.size());
  if (connection->length < 0) {
    const auto delimiter = connection->bytes.indexOf("\r\n\r\n");
    if (delimiter < 0) {
      if (connection->bytes.size() > HeaderLimit)
        reply(connection, 413, "Native question headers exceed their limit.");
      return;
    }
    if (delimiter > HeaderLimit) {
      reply(connection, 413, "Native question headers exceed their limit.");
      return;
    }
    const auto lines = connection->bytes.left(delimiter).split('\n');
    if (lines.isEmpty() ||
        lines[0] != "POST /tools/request-user-input HTTP/1.1\r") {
      reply(connection, 401, "Unauthorized native question callback.");
      return;
    }
    QHash<QByteArray, QByteArray> headers;
    for (int i = 1; i < lines.size(); ++i) {
      const auto line = lines[i].trimmed();
      const auto colon = line.indexOf(':');
      if (colon <= 0) {
        reply(connection, 400, "Invalid native question headers.");
        return;
      }
      const auto name = line.left(colon).toLower();
      if (headers.contains(name)) {
        reply(connection, 400, "Duplicate native question headers.");
        return;
      }
      headers.insert(name, line.mid(colon + 1).trimmed());
    }
    const auto expectedHost =
        "127.0.0.1:" + QByteArray::number(m_server.serverPort());
    if (headers.value("host") != expectedHost || headers.contains("origin") ||
        !sameSecret(headers.value("authorization"), "Bearer " + m_secret)) {
      reply(connection, 401, "Unauthorized native question callback.");
      return;
    }
    if (headers.contains("transfer-encoding") ||
        !headers.contains("content-length")) {
      reply(connection, 400, "A bounded content length is required.");
      return;
    }
    const auto lengthText = headers.value("content-length");
    bool digits = !lengthText.isEmpty();
    for (const auto c : lengthText)
      digits &= c >= '0' && c <= '9';
    bool converted = false;
    const auto length = lengthText.toLongLong(&converted);
    if (!digits || !converted || length < 0 || length > BodyLimit) {
      reply(connection, 413, "Native question body exceeds its limit.");
      return;
    }
    connection->length = length;
    connection->bytes.remove(0, delimiter + 4);
  }
  if (connection->bytes.size() < connection->length)
    return;
  if (connection->bytes.size() != connection->length) {
    reply(connection, 400, "Pipelined requests are not supported.");
    return;
  }
  QStringDecoder decoder(QStringDecoder::Utf8,
                         QStringConverter::Flag::Stateless);
  const QString decoded = decoder.decode(connection->bytes);
  Q_UNUSED(decoded);
  if (decoder.hasError()) {
    reply(connection, 400, "Invalid native question UTF-8.");
    return;
  }
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(connection->bytes, &parse);
  connection->bytes.clear();
  if (parse.error != QJsonParseError::NoError || !document.isObject()) {
    reply(connection, 400, "Invalid native question JSON.");
    return;
  }
  if (m_turn.isEmpty()) {
    reply(connection, 400, "No active native question owner.");
    return;
  }
  if (m_pending) {
    reply(connection, 200, cancelledBytes());
    return;
  }
  connection->waiting = true;
  connection->timer->stop();
  m_pending = connection;
  m_pendingTurn = m_turn;
  m_pendingToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
  emit questionRequested(m_pendingToken, m_pendingTurn, document.object());
}
bool QuestionMcpBridge::complete(const QString &token,
                                 const QJsonObject &result,
                                 const QString &issue) {
  if (token.isEmpty() || token != m_pendingToken || m_pendingTurn != m_turn ||
      !m_pending)
    return false;
  const auto connection = m_pending;
  m_pending.reset();
  m_pendingToken.clear();
  m_pendingTurn.clear();
  const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
  if (!issue.isEmpty())
    reply(connection, 400, issue.left(8192).toUtf8());
  else if (bytes.size() > ReplyLimit)
    reply(connection, 413, "Native question result exceeds its limit.");
  else
    reply(connection, 200, bytes);
  return true;
}
} // namespace ade
