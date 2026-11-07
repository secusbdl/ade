#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QProcessEnvironment>
#include <QRegularExpression>
namespace ade {
struct ShellWaitPolicy {
  int timeoutMs = 10000, idleMs = 5000;
  QRegularExpression pattern;
  bool hasPattern = false;
  static bool parse(const QJsonObject &arguments, ShellWaitPolicy *policy,
                    QString *error = nullptr);
};
struct ShellProtocolEvent {
  enum Kind {
    Output,
    CommandStart,
    CommandDone,
    WorkingDirectory,
    Ready
  } kind = Output;
  QByteArray bytes;
  QString text;
  int exitCode = 0;
};
// Incremental OSC133/OSC7 framing, independent of QObject/signal lifetimes.
// Only command-end markers bearing the private session token close a command.
class ShellProtocol {
public:
  explicit ShellProtocol(QString token = {});
  QList<ShellProtocolEvent> feed(const QByteArray &bytes);
  void beginCommand() { m_inCommand = true; }
  bool ready() const { return m_ready; }
  bool inCommand()const{return m_inCommand;}
  static QString expandStdinEscapes(const QString &text);
  static QString powerShellIntegration(const QString &token);
  static QProcessEnvironment
  agentEnvironment(const QProcessEnvironment &source);

private:
  QString m_token;
  QByteArray m_pending;
  bool m_inCommand = false, m_authenticatedPrompt = false, m_ready = false;
};
} // namespace ade
