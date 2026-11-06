#include "QuestionMcpStdio.h"
#include "QuestionMcpProtocol.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPointer>
#include <QStringDecoder>
#include <QTimer>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <QSocketNotifier>
#include <unistd.h>
#endif
namespace ade {
namespace {
constexpr qsizetype InputLimit = 512 * 1024, ResultLimit = 2 * 1024 * 1024,
                    WireLimit = 8 * 1024 * 1024;
class Stdio final : public QObject {
public:
  Stdio(const QJsonObject &schema, QObject *parent = nullptr)
      : QObject(parent), protocol(schema, this), network(this) {
    network.setProxy(QNetworkProxy::NoProxy);
    connect(&protocol, &QuestionMcpProtocol::output, this,
            [this](const QJsonObject &value) { write(value); });
    connect(&protocol, &QuestionMcpProtocol::questionRequested, this,
            [this](const QJsonValue &id, const QJsonObject &form) {
              request(id, form);
            });
    connect(&protocol, &QuestionMcpProtocol::questionCancelled, this,
            [this](const QJsonValue &) { cancel(); });
  }
  ~Stdio() override { cancel(); }
  bool start() {
    url = QUrl(qEnvironmentVariable("STAGEWISE_MCP_CALLBACK_URL"));
    secret = qgetenv("STAGEWISE_MCP_CALLBACK_TOKEN");
    bool hex = secret.size() == 64;
    for (const auto c : secret)
      hex &= (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex || url.scheme() != "http" || url.host() != "127.0.0.1" ||
        url.port() < 1 || url.port() > 65535 ||
        url.path() != "/tools/request-user-input" ||
        !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
      return false;
#ifdef Q_OS_WIN
    input = GetStdHandle(STD_INPUT_HANDLE);
    output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE ||
        !input || !output || GetFileType(input) != FILE_TYPE_PIPE)
      return false;
    timer.setInterval(10);
    connect(&timer, &QTimer::timeout, this, [this] { poll(); });
    timer.start();
#else
    notifier = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
    connect(notifier, &QSocketNotifier::activated, this, [this] { poll(); });
#endif
    return true;
  }

private:
  void cancel() {
    const auto old = reply;
    reply = nullptr;
    response.clear();
    if (old) {
      disconnect(old, nullptr, this, nullptr);
      old->abort();
      old->deleteLater();
    }
  }
  void request(const QJsonValue &id, const QJsonObject &form) {
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + secret);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(0);
    response.clear();
    reply = network.post(request,
                         QJsonDocument(form).toJson(QJsonDocument::Compact));
    const auto current = reply;
    connect(current, &QNetworkReply::readyRead, this, [this, current, id] {
      if (reply != current)
        return;
      if (current->bytesAvailable() > ResultLimit - response.size()) {
        cancel();
        protocol.complete(id, {}, "Native question result exceeds its limit.");
        return;
      }
      response += current->read(ResultLimit - response.size());
    });
    connect(current, &QNetworkReply::finished, this, [this, current, id] {
      if (reply != current)
        return;
      if (current->bytesAvailable() > ResultLimit - response.size()) {
        cancel();
        protocol.complete(id, {}, "Native question result exceeds its limit.");
        return;
      }
      response += current->read(ResultLimit - response.size());
      const auto bytes = response;
      response.clear();
      reply = nullptr;
      const bool ok =
          current->error() == QNetworkReply::NoError &&
          current->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                  .toInt() == 200;
      current->deleteLater();
      QJsonParseError parse;
      const auto result = QJsonDocument::fromJson(bytes, &parse);
      if (!ok || parse.error != QJsonParseError::NoError ||
          !result.isObject()) {
        protocol.complete(
            id, {},
            "The native question callback failed or became unavailable.");
        return;
      }
      protocol.complete(id, result.object());
    });
  }
  void write(const QJsonObject &value) {
    const auto data =
        QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
    if (data.size() > WireLimit) {
      QCoreApplication::exit(6);
      return;
    }
    qsizetype position = 0;
    while (position < data.size()) {
#ifdef Q_OS_WIN
      DWORD size = 0;
      if (!WriteFile(output, data.constData() + position,
                     DWORD(qMin<qsizetype>(65536, data.size() - position)),
                     &size, nullptr) ||
          !size) {
        QCoreApplication::exit(7);
        return;
      }
#else
      const auto size = ::write(STDOUT_FILENO, data.constData() + position,
                                data.size() - position);
      if (size <= 0) {
        QCoreApplication::exit(7);
        return;
      }
#endif
      position += size;
    }
  }
  void poll() {
    char buffer[16384];
    int count = 0;
#ifdef Q_OS_WIN
    DWORD available = 0;
    if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
      protocol.cancelPending();
      QCoreApplication::quit();
      return;
    }
    if (!available)
      return;
    DWORD size = 0;
    if (!ReadFile(input, buffer, qMin<DWORD>(sizeof(buffer), available), &size,
                  nullptr) ||
        !size) {
      protocol.cancelPending();
      QCoreApplication::quit();
      return;
    }
    count = int(size);
#else
    count = int(::read(STDIN_FILENO, buffer, sizeof(buffer)));
    if (count <= 0) {
      protocol.cancelPending();
      QCoreApplication::quit();
      return;
    }
#endif
    if (pending.size() + count > InputLimit) {
      QCoreApplication::exit(5);
      return;
    }
    pending.append(buffer, count);
    while (true) {
      const auto newline = pending.indexOf('\n');
      if (newline < 0)
        return;
      const auto frame = pending.left(newline);
      pending.remove(0, newline + 1);
      QStringDecoder decoder(QStringDecoder::Utf8,
                             QStringConverter::Flag::Stateless);
      const QString decoded = decoder.decode(frame);
      Q_UNUSED(decoded);
      QJsonParseError parse;
      const auto message = QJsonDocument::fromJson(frame, &parse);
      if (decoder.hasError() || parse.error != QJsonParseError::NoError ||
          !message.isObject()) {
        write({{"jsonrpc", "2.0"},
               {"id", QJsonValue(QJsonValue::Null)},
               {"error", QJsonObject{{"code", -32700},
                                     {"message", "Invalid MCP JSON frame."}}}});
        continue;
      }
      protocol.receive(message.object());
    }
  }
  QuestionMcpProtocol protocol;
  QNetworkAccessManager network;
  QPointer<QNetworkReply> reply;
  QByteArray pending, response, secret;
  QUrl url;
#ifdef Q_OS_WIN
  QTimer timer;
  HANDLE input = nullptr, output = nullptr;
#else
  QSocketNotifier *notifier = nullptr;
#endif
};
} // namespace
int runQuestionMcpStdio(int argc, char **argv, const QJsonObject &schema) {
  if (argc != 2 || QByteArray(argv[1]) != "--question-mcp-stdio")
    return 2;
  QCoreApplication app(argc, argv);
  Stdio server(schema);
  if (!server.start())
    return 3;
  return app.exec();
}
} // namespace ade
