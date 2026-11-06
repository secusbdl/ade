#include "ShellService.h"
#include "SessionLogStore.h"
#include "ShellProtocol.h"
#include "core/ScopedDirectory.h"
#include "terminal/ConPtyProcess.h"
#include "terminal/TerminalModel.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QTimer>
#include <QUuid>
#include <algorithm>
namespace ade {
namespace {
QString json(const QJsonObject &object) {
  return QString::fromUtf8(
      QJsonDocument(object).toJson(QJsonDocument::Compact));
}
QJsonObject prop(const char *type) { return {{"type", type}}; }
QJsonObject tool(const QString &name, const QString &description,
                 const QJsonObject &properties, const QJsonArray &required) {
  return {{"type", "function"},
          {"function",
           QJsonObject{{"name", name},
                       {"description", description},
                       {"parameters", QJsonObject{{"type", "object"},
                                                  {"properties", properties},
                                                  {"required", required}}}}}};
}
QString headTail(const QString &text) {
  auto lines = text.split('\n');
  if (lines.size() <= 400)
    return text;
  return lines.mid(0, 100).join('\n') +
         QString("\n... [%1 lines truncated] ...\n").arg(lines.size() - 400) +
         lines.mid(lines.size() - 300).join('\n');
}
QString utf8Tail(const QString &text, int maximum) {
  const auto bytes = text.toUtf8();
  int start = qMax(0, int(bytes.size()) - maximum);
  while (start < bytes.size() &&
         (static_cast<unsigned char>(bytes[start]) & 0xc0) == 0x80)
    ++start;
  return QString::fromUtf8(bytes.mid(start));
}
QByteArray stripControls(QByteArray *pending, const QByteArray &bytes) {
  *pending += bytes;
  QByteArray result;
  int pos = 0;
  while (pos < pending->size()) {
    const auto ch = (*pending)[pos];
    if (ch != '\x1b') {
      if (ch != '\r' &&
          (static_cast<unsigned char>(ch) >= 32 || ch == '\n' || ch == '\t'))
        result += ch;
      ++pos;
      continue;
    }
    if (pos + 1 >= pending->size())
      break;
    const auto next = (*pending)[pos + 1];
    if (next == '[') {
      int end = pos + 2;
      while (end < pending->size() &&
             !(static_cast<unsigned char>((*pending)[end]) >= 0x40 &&
               static_cast<unsigned char>((*pending)[end]) <= 0x7e))
        ++end;
      if (end == pending->size() && end - pos < 8192)
        break;
      pos = qMin(end + 1, int(pending->size()));
    } else if (next == ']') {
      const auto bel = pending->indexOf('\a', pos + 2),
                 st = pending->indexOf("\x1b\\", pos + 2);
      const auto end = bel < 0 ? st : st < 0 ? bel : qMin(bel, st);
      if (end < 0 && pending->size() - pos < 8192)
        break;
      pos = end < 0 ? int(pending->size()) : int(end + (end == bel ? 1 : 2));
    } else if (next == '(' || next == ')') {
      if (pos + 2 >= pending->size())
        break;
      pos += 3;
    } else
      pos += 2;
  }
  pending->remove(0, pos);
  return result;
}
QString shellExecutable() {
  auto executable = QStandardPaths::findExecutable("pwsh.exe");
  if (executable.isEmpty())
    executable = QStandardPaths::findExecutable("powershell.exe");
  return executable;
}
} // namespace
struct ShellService::Session {
  QString id, cwd, cwdPrefix, token, requestId, createRequest, command,
      rawInput, matchText;
  QJsonObject watcher;
  qint64 created = 0;
  QPointer<ConPtyProcess> process;
  QPointer<TerminalModel> model;
  QPointer<QTimer> timeout, idle, initialization, watcherTimeout, stream,
      inputPump;
  ScopedDirectorySnapshot cwdPin;
  ShellProtocol protocol;
  ShellWaitPolicy policy;
  QByteArray ansiPending, inputPending;
  QStringDecoder decoder{QStringDecoder::Utf8};
  int markLine = 0, exitCode = -1;
  bool ready = false, dispatched = false, exited = false, watcherActive = false,
       raw = false, hasOutput = false, commandActive = false;
  quint64 generation = 0, waitSerial = 0, exitSerial = 0;
};
ShellService::ShellService(QString agentId,
                           std::shared_ptr<SessionLogStore> logs,
                           QObject *parent)
    : QObject(parent), m_agentId(std::move(agentId)), m_logs(std::move(logs)) {}
ShellService::~ShellService() {
  m_closing = true;
  ++m_generation;
  for (const auto &session : m_sessions) {
    session->watcherActive = false;
    if (session->process) {
      session->process->disconnect(this);
      session->process->stop();
    }
    if (m_logs)
      m_logs->close(session->id);
  }
  m_sessions.clear();
}
QJsonArray ShellService::definitions() {
  const QJsonObject wait{
      {"type", "object"},
      {"properties", QJsonObject{{"timeout_ms", prop("integer")},
                                 {"exited", prop("boolean")},
                                 {"output_pattern", prop("string")},
                                 {"idle_ms", prop("integer")}}}};
  return {
      tool(
          "createShellSession",
          "Create a persistent PowerShell PTY owned by this chat. Variables, "
          "cwd and running processes survive calls. Use an existing idle "
          "session where possible; at most five sessions may run per chat. cwd "
          "must be a configured mount prefix, never '.' or an absolute path.",
          {{"cwd", prop("string")}}, {"cwd"}),
      tool("executeShellCommand",
           "Send command, raw stdin, empty-command poll, or kill to an "
           "existing session_id. These actions are mutually exclusive. An "
           "idle/pattern/timeout snapshot does not stop the command; poll the "
           "same session. Tool cancellation kills its owned process tree. "
           "wait_until timeout defaults to 10s (5s stdin), 15s with "
           "wait_until, or 300s when exited=true; caps60s/300s. Idle after "
           "output defaults5s/15s. Persisted logs are readable at "
           "shells/<session_id>.shell.log. explanation is required on every "
           "call. Shell commands have the user's OS permissions and are not a "
           "filesystem sandbox.",
           {{"explanation", prop("string")},
            {"session_id", prop("string")},
            {"command", prop("string")},
            {"stdin", prop("string")},
            {"kill", prop("boolean")},
            {"wait_until", wait}},
           {"explanation", "session_id"}),
      tool("createWatcherSession",
           "Start a one-shot foreground polling command in a dedicated "
           "session. Keep polls read-only; print the matched condition and "
           "exit0, or exit nonzero on permanent failure. Returns once armed; a "
           "later result wakes this chat. Watchers are process-local and never "
           "restart automatically. timeout_ms must be10000..604800000. Do not "
           "embed credentials or detach background work.",
           {{"cwd", prop("string")},
            {"command", prop("string")},
            {"title", prop("string")},
            {"description", prop("string")},
            {"timeout_ms", prop("integer")}},
           {"cwd", "command", "title", "timeout_ms"})};
}
bool ShellService::handles(const QString &tool) {
  return tool == "createShellSession" || tool == "executeShellCommand" ||
         tool == "createWatcherSession";
}
bool ShellService::owns(const SessionPtr &session) const {
  return !m_closing && session && m_sessions.value(session->id) == session &&
         session->generation == m_generation;
}
bool ShellService::setMounts(const QMap<QString, QString> &mounts,
                             QString *error) {
  if (error)
    error->clear();
  QMap<QString, QString> canonical;
  QMap<QString, std::shared_ptr<void>> pins;
  for (auto it = mounts.begin(); it != mounts.end(); ++it) {
    if (!QRegularExpression("^(ws[0-9]+|w[0-9a-f]+|apps)$")
             .match(it.key())
             .hasMatch()) {
      if (error)
        *error = "Shell mount prefix is not supported.";
      return false;
    }
    ScopedDirectorySnapshot root;
    bool truncated = false;
    const auto expected = QFileInfo(it.value()).canonicalFilePath();
    if (expected.isEmpty() ||
        !readScopedDirectory(it.value(), expected, &root, error, 0, nullptr,
                             true, &truncated))
      return false;
    canonical[it.key()] = root.canonicalPath;
    pins[it.key()] = root.pin;
  }
  if (canonical == m_mounts)
    return true;
  const QPointer<ShellService> self(this);
  closeAll();
  if (!self)
    return false;
  m_mounts = canonical;
  m_mountPins = pins;
  return true;
}
QString ShellService::resolveCwd(const QString &mounted, QString *error) const {
  auto reject = [&](const QString &text) {
    if (error)
      *error = text;
    return QString();
  };
  const auto parts = mounted.split('/');
  if (mounted.isEmpty() || mounted.size() > 32768 || mounted.contains('\\') ||
      mounted.contains(':') || mounted.contains(QChar::Null) ||
      parts.contains("..") || parts.contains(".") ||
      !m_mounts.contains(parts.first()))
    return reject("cwd must name an authorized mounted directory.");
  const auto root = m_mounts.value(parts.first());
  const auto candidate = QDir::cleanPath(root + '/' + parts.mid(1).join('/'));
  const auto canonical = QFileInfo(candidate).canonicalFilePath();
  if (canonical.isEmpty() ||
      (!canonical.startsWith(root + '/', Qt::CaseInsensitive) &&
       canonical.compare(root, Qt::CaseInsensitive) != 0))
    return reject("Shell working directory is outside its configured mount.");
  return candidate;
}
void ShellService::execute(const QString &requestId, const QString &name,
                           const QJsonObject &args) {
  if (requestId.isEmpty() || requestId.size() > 512 || m_closing ||
      m_requests.contains(requestId)) {
    emit completed(requestId, "Invalid or duplicate shell request identity.",
                   true);
    return;
  }
  if (name == "createShellSession" || name == "createWatcherSession")
    create(requestId, args, name == "createWatcherSession");
  else if (name == "executeShellCommand")
    command(requestId, args);
  else
    emit completed(requestId, "Unknown shell tool.", true);
}
void ShellService::create(const QString &requestId, const QJsonObject &args,
                          bool watcher) {
  if (!pruneExited())
    return;
  QString error;
  const auto cwd = resolveCwd(args["cwd"].toString(), &error);
  const auto executable = shellExecutable();
  int active = 0;
  for (const auto &session : m_sessions)
    if (!session->exited)
      ++active;
  if (cwd.isEmpty() || executable.isEmpty() || !m_logs || active >= 5) {
    emit completed(
        requestId,
        error.isEmpty()
            ? QString(
                  "A PowerShell executable, log store and available session "
                  "slot are required (5 active sessions per chat).")
            : error,
        true);
    return;
  }
  if (watcher) {
    const auto timeout = args["timeout_ms"];
    if (!args["command"].isString() || args["command"].toString().isEmpty() ||
        args["command"].toString().size() > 65536 ||
        !args["title"].isString() || args["title"].toString().isEmpty() ||
        args["title"].toString().size() > 120 ||
        (args.contains("description") &&
         (!args["description"].isString() ||
          args["description"].toString().size() > 1000)) ||
        !timeout.isDouble() ||
        timeout.toDouble() != double(timeout.toInteger()) ||
        timeout.toInteger() < 10000 || timeout.toInteger() > 604800000) {
      emit completed(requestId,
                     "Watcher arguments are invalid or outside their bounds.",
                     true);
      return;
    }
  }
  auto session = std::make_shared<Session>();
  session->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  session->token = QUuid::createUuid().toString(QUuid::Id128);
  session->protocol = ShellProtocol(session->token);
  session->generation = m_generation;
  session->cwd = cwd;
  session->cwdPrefix = args["cwd"].toString();
  session->created = QDateTime::currentMSecsSinceEpoch();
  session->createRequest = requestId;
  const auto root = m_mounts.value(args["cwd"].toString().section('/', 0, 0));
  bool truncated = false;
  if (!readScopedDirectory(cwd, root, &session->cwdPin, &error, 0, nullptr,
                           false, &truncated) ||
      !m_logs->create(session->id, &error)) {
    emit completed(requestId, error, true);
    return;
  }
  session->process = new ConPtyProcess(this);
  session->model = new TerminalModel({120, 24}, this);
  session->timeout = new QTimer(this);
  session->idle = new QTimer(this);
  session->initialization = new QTimer(this);
  session->watcherTimeout = new QTimer(this);
  session->stream = new QTimer(this);
  session->inputPump = new QTimer(this);
  for (auto timer :
       {session->timeout, session->idle, session->initialization,
        session->watcherTimeout, session->stream, session->inputPump})
    timer->setSingleShot(true);
  m_sessions[session->id] = session;
  m_requests[requestId] = session->id;
  if (watcher) {
    session->watcher = {
        {"title", args["title"]},
        {"description", args["description"].toString()},
        {"command", args["command"]},
        {"startedAt", session->created},
        {"expiresAt", session->created + args["timeout_ms"].toInteger()}};
    session->watcherActive = true;
    session->command = args["command"].toString();
  }
  connect(
      session->process, &ConPtyProcess::outputReady, this,
      [this, session](const QByteArray &bytes) { receive(session, bytes); });
  connect(session->process, &ConPtyProcess::exited, this,
          [this, session](quint32 code) { processExited(session, int(code)); });
  connect(session->model, &TerminalModel::outputReady, session->process,
          [process = session->process](const QByteArray &bytes) {
            if (process)
              process->write(bytes);
          });
  connect(session->timeout, &QTimer::timeout, this, [this, session] {
    if (owns(session))
      resolve(session, "timeout");
  });
  connect(session->idle, &QTimer::timeout, this, [this, session] {
    if (owns(session))
      resolve(session, "idle");
  });
  connect(session->initialization, &QTimer::timeout, this, [this, session] {
    if (owns(session) && !session->ready)
      kill(session, "abort");
  });
  connect(session->watcherTimeout, &QTimer::timeout, this, [this, session] {
    if (!owns(session) || !session->watcherActive)
      return;
    notifyWatcher(session, "timed_out", -1);
  });
  connect(session->stream, &QTimer::timeout, this, [this, session] {
    if (!owns(session) || session->requestId.isEmpty() || !session->model)
      return;
    const auto request = session->requestId;
    auto view = utf8Tail(session->model->plainText(), 16 * 1024);
    emit outputSnapshot(request, view);
  });
  connect(session->inputPump, &QTimer::timeout, this,
          [this, session] { pumpInput(session); });
  const auto integration = ShellProtocol::powerShellIntegration(session->token);
  const auto utf16 =
      QByteArray(reinterpret_cast<const char *>(integration.utf16()),
                 integration.size() * 2);
  const QStringList commandLine{executable, "-NoLogo", "-NoExit",
                                "-EncodedCommand",
                                QString::fromLatin1(utf16.toBase64())};
  const QPointer<ShellService> self(this);
  const bool started = session->process->start(
      session->cwdPin.canonicalPath, commandLine, {120, 24}, &error,
      ShellProtocol::agentEnvironment(
          QProcessEnvironment::systemEnvironment()));
  if (!self || !owns(session))
    return;
  if (!started) {
    session->exited = true;
    session->createRequest.clear();
    m_requests.remove(requestId);
    m_logs->close(session->id);
    emit completed(requestId, error, true);
    return;
  }
  session->initialization->start(10000);
  if (watcher)
    session->watcherTimeout->start(int(args["timeout_ms"].toInteger()));
  emit manifestChanged();
}
void ShellService::command(const QString &requestId, const QJsonObject &args) {
  const auto session = m_sessions.value(args["session_id"].toString());
  auto reject = [&](const QString &message) {
    emit completed(requestId, message, true);
  };
  if (!session) {
    reject("This shell session does not belong to this chat.");
    return;
  }
  if (!args["explanation"].isString() ||
      args["explanation"].toString().isEmpty() ||
      args["explanation"].toString().size() > 512 ||
      (args.contains("command") && !args["command"].isString()) ||
      (args.contains("stdin") && !args["stdin"].isString()) ||
      (args.contains("kill") && !args["kill"].isBool())) {
    reject("Shell action arguments are invalid.");
    return;
  }
  const bool hasCommand = !args["command"].toString().isEmpty(),
             raw = args.contains("stdin"), stop = args["kill"].toBool();
  if ((raw && (hasCommand || stop)) || (stop && hasCommand) ||
      args["command"].toString().toUtf8().size() > 65536 ||
      args["stdin"].toString().toUtf8().size() > 65536) {
    reject("Choose one bounded command, raw stdin, poll or kill action.");
    return;
  }
  ShellWaitPolicy policy;
  QString error;
  if (!ShellWaitPolicy::parse(args, &policy, &error)) {
    reject(error);
    return;
  }
  const auto priorSerial = session->waitSerial;
  const QPointer<ShellService> self(this);
  if (!session->requestId.isEmpty()) {
    resolve(session, "abort");
    if (!self || !owns(session))
      return;
    if (session->waitSerial != priorSerial) {
      reject("Shell request was superseded by a newer request.");
      return;
    }
  }
  if (stop) {
    kill(session, "abort");
    if (!self || !owns(session))
      return;
    emit completed(requestId,
                   json({{"session_id", session->id},
                         {"output", "Session killed."},
                         {"exit_code", QJsonValue()},
                         {"session_exited", true},
                         {"timed_out", false},
                         {"resolved_by", "abort"}}),
                   false);
    return;
  }
  if (session->exited) {
    emit completed(requestId,
                   json({{"session_id", session->id},
                         {"output", "Session has exited."},
                         {"exit_code", session->exitCode < 0
                                           ? QJsonValue()
                                           : QJsonValue(session->exitCode)},
                         {"session_exited", true},
                         {"timed_out", false},
                         {"resolved_by", "session_exited"}}),
                   false);
    return;
  }
  if (session->watcherActive) {
    reject("A watcher owns this session; kill it to cancel the watcher.");
    return;
  }
  ++session->waitSerial;
  session->requestId = requestId;
  session->command =
      raw ? args["stdin"].toString() : args["command"].toString();
  session->raw = raw;
  session->policy = policy;
  session->dispatched = false;
  session->hasOutput = false;
  session->matchText.clear();
  m_requests[requestId] = session->id;
  if (session->ready)
    dispatch(session);
}
void ShellService::dispatch(const SessionPtr &session) {
  if (!owns(session) || session->exited || session->dispatched)
    return;
  session->dispatched = true;
  session->markLine =
      session->model->historyLines() + session->model->cursor().y();
  if (!session->requestId.isEmpty())
    session->timeout->start(session->policy.timeoutMs);
  if (session->command.isEmpty())
    return;
  if (!session->raw) {
    session->protocol.beginCommand();
    session->commandActive = true;
  }
  const auto bytes = session->command.toUtf8() +
                     (session->raw ? QByteArray() : QByteArray("\r"));
  // Input ownership belongs to the session, not to a wait result. A timeout
  // snapshot must not discard the unsubmitted tail of a long command.
  if (session->inputPending.size() + bytes.size() > 256 * 1024) {
    kill(session, "abort");
    return;
  }
  session->inputPending += bytes;
  if (!session->inputPump->isActive())
    pumpInput(session);
}
void ShellService::pumpInput(const SessionPtr &session) {
  if (!owns(session) || session->exited || session->inputPending.isEmpty())
    return;
  const auto part = session->inputPending.left(256);
  session->inputPending.remove(0, part.size());
  if (!session->process->write(part)) {
    kill(session, "abort");
    return;
  }
  if (!session->inputPending.isEmpty())
    session->inputPump->start(5);
}
void ShellService::receive(const SessionPtr &session, const QByteArray &bytes) {
  if (!owns(session) || session->exited)
    return;
  const QPointer<ShellService> self(this);
  const auto serial = session->waitSerial;
  const auto events = session->protocol.feed(bytes);
  for (const auto &event : events) {
    if (!self || !owns(session) || session->exited ||
        session->waitSerial != serial)
      return;
    if (event.kind == ShellProtocolEvent::Output) {
      const auto historyRevision = session->model->historyRevision();
      session->model->feed(event.bytes);
      if (!self || !owns(session))
        return;
      const bool sameWait = session->waitSerial == serial;
      if (sameWait)
        session->markLine =
            qMax(0, session->markLine -
                        int(qMin<quint64>(session->model->historyRevision() -
                                              historyRevision,
                                          INT_MAX)));
      if (!session->ready)
        continue;
      const auto clean = stripControls(&session->ansiPending, event.bytes);
      const QString decoded = session->decoder.decode(clean);
      QString error;
      if (!decoded.isEmpty() &&
          !m_logs->append(session->id, decoded.toUtf8(), &error)) {
        kill(session, "abort");
        return;
      }
      // TerminalModel publishes changes synchronously. A replacement wait can
      // be installed there; log these captured bytes, but never assign them to
      // the new wait's matching/idle/output state.
      if (!sameWait)
        return;
      if (!session->requestId.isEmpty() && !decoded.isEmpty()) {
        session->hasOutput = true;
        session->matchText =
            utf8Tail(session->matchText + decoded, 1024 * 1024);
        if (session->policy.idleMs > 0)
          session->idle->start(session->policy.idleMs);
        if (!session->stream->isActive())
          session->stream->start(100);
        if (session->policy.hasPattern &&
            session->policy.pattern.match(session->matchText).hasMatch()) {
          resolve(session, "pattern");
          if (!self || !owns(session))
            return;
        }
      }
    } else if (event.kind == ShellProtocolEvent::CommandStart) {
      session->commandActive = true;
      session->dispatched = true;
      if (!session->requestId.isEmpty())
        session->markLine =
            session->model->historyLines() + session->model->cursor().y();
      emit manifestChanged();
    } else if (event.kind == ShellProtocolEvent::WorkingDirectory) {
      session->cwd = event.text;
      emit manifestChanged();
    } else if (event.kind == ShellProtocolEvent::Ready) {
      session->ready = true;
      session->initialization->stop();
      const auto request = session->createRequest;
      session->createRequest.clear();
      m_requests.remove(request);
      if (session->watcherActive)
        dispatch(session);
      if (!self || !owns(session))
        return;
      QJsonObject result{
          {"session_id", session->id},
          {"message", "Session created in " + session->cwdPrefix + "."}};
      if (session->watcherActive)
        result["expires_at"] = session->watcher["expiresAt"];
      if (!request.isEmpty())
        emit completed(request, json(result), false);
      if (!self || !owns(session))
        return;
      if (!session->requestId.isEmpty())
        dispatch(session);
    } else if (event.kind == ShellProtocolEvent::CommandDone) {
      session->commandActive = false;
      session->dispatched = false;
      if (session->watcherActive) {
        notifyWatcher(session, event.exitCode == 0 ? "triggered" : "failed",
                      event.exitCode);
        return;
      }
      resolve(session, "exit", event.exitCode);
      if (self && owns(session) && session->waitSerial == serial)
        emit manifestChanged();
    }
  }
}
void ShellService::resolve(const SessionPtr &session, const QString &reason,
                           int exitCode) {
  if (!owns(session) || session->requestId.isEmpty())
    return;
  const auto request = session->requestId;
  session->requestId.clear();
  m_requests.remove(request);
  session->timeout->stop();
  session->idle->stop();
  session->stream->stop();
  session->dispatched = false;
  const auto all = session->model ? session->model->plainText() : QString();
  const auto lines = all.split('\n');
  const auto output =
      headTail(
          lines.mid(qBound(0, session->markLine, int(lines.size()))).join('\n'))
          .left(65536);
  QString logError;
  m_logs->flush(session->id, &logError);
  QJsonObject result{
      {"session_id", session->id},
      {"output", output},
      {"recent_output", all.right(12000)},
      {"exit_code", exitCode < 0 ? QJsonValue() : QJsonValue(exitCode)},
      {"session_exited", session->exited},
      {"timed_out", reason == "timeout"},
      {"resolved_by", reason}};
  if (!logError.isEmpty()) {
    result["output"] = output + "\n[Log flush failed: " + logError + "]";
  }
  emit completed(request, json(result), false);
}
void ShellService::kill(const SessionPtr &session, const QString &reason) {
  if (!owns(session))
    return;
  const QPointer<ShellService> self(this);
  session->watcherActive = false;
  session->watcherTimeout->stop();
  session->initialization->stop();
  session->inputPump->stop();
  session->inputPending.clear();
  session->exited = true;
  session->commandActive = false;
  session->exitCode = -1;
  if (session->process) {
    session->process->disconnect(this);
    session->process->stop();
  }
  const auto create = session->createRequest;
  session->createRequest.clear();
  m_requests.remove(create);
  m_logs->close(session->id);
  session->cwdPin = {};
  if (!create.isEmpty()) {
    emit completed(
        create, "Shell initialization or watcher creation was canceled.", true);
    if (!self || !owns(session))
      return;
  }
  resolve(session, reason);
  if (!self || !owns(session))
    return;
  if (!retire(session))
    return;
  emit manifestChanged();
}
void ShellService::processExited(const SessionPtr &session, int code) {
  if (!owns(session) || session->exited)
    return;
  if (session->watcherActive) {
    notifyWatcher(session, code == 0 ? "triggered" : "failed", code);
    return;
  }
  session->exited = true;
  session->exitCode = code;
  session->initialization->stop();
  session->cwdPin = {};
  m_logs->close(session->id);
  const auto create = session->createRequest;
  session->createRequest.clear();
  m_requests.remove(create);
  const QPointer<ShellService> self(this);
  if (!create.isEmpty()) {
    emit completed(create, "Shell exited during initialization.", true);
    if (!self || !owns(session))
      return;
  }
  resolve(session, "session_exited", code);
  if (!self || !owns(session))
    return;
  if (!retire(session))
    return;
  emit manifestChanged();
}
bool ShellService::retire(const SessionPtr &session) {
  if (!owns(session))
    return false;
  if (!session->exitSerial)
    session->exitSerial = ++m_exitSerial;
  const QPointer<ShellService> self(this);
  if (session->model)
    session->model->trimHistory(200);
  return self && pruneExited();
}
bool ShellService::pruneExited() {
  QList<SessionPtr> exited;
  for (const auto &session : m_sessions)
    if (session->exited && session->createRequest.isEmpty() &&
        session->requestId.isEmpty())
      exited.append(session);
  std::sort(exited.begin(), exited.end(), [](const auto &a, const auto &b) {
    return a->exitSerial < b->exitSerial;
  });
  const QPointer<ShellService> self(this);
  while (exited.size() > 32) {
    const auto session = exited.takeFirst();
    m_sessions.remove(session->id);
    // File logs are retained. Only this host's exited in-memory view and idle
    // transport/timers are retired; borrowed views observe QPointer
    // invalidation.
    const QList<QPointer<QObject>> objects{
        session->model.data(),          session->process.data(),
        session->timeout.data(),        session->idle.data(),
        session->initialization.data(), session->watcherTimeout.data(),
        session->stream.data(),         session->inputPump.data()};
    for (const auto &object : objects) {
      if (object)
        delete object.data();
      if (!self)
        return false;
    }
  }
  return true;
}
void ShellService::cancel(const QString &requestId) {
  const auto session = m_sessions.value(m_requests.value(requestId));
  if (session)
    kill(session, "abort");
}
void ShellService::cancelAll() {
  const auto sessions = m_sessions.values();
  const QPointer<ShellService> self(this);
  for (const auto &session : sessions) {
    if (!session->requestId.isEmpty() || !session->createRequest.isEmpty())
      kill(session, "abort");
    if (!self)
      return;
  }
}
void ShellService::closeAll() {
  const auto sessions = m_sessions.values();
  const QPointer<ShellService> self(this);
  for (const auto &session : sessions) {
    if (!session->exited)
      kill(session, "abort");
    if (!self)
      return;
  }
}
void ShellService::notifyWatcher(const SessionPtr &session,
                                 const QString &outcome, int code) {
  if (!owns(session) || !session->watcherActive)
    return;
  auto result = session->watcher;
  result["agentId"] = m_agentId;
  result["sessionId"] = session->id;
  result["outcome"] = outcome;
  result["finishedAt"] = QDateTime::currentMSecsSinceEpoch();
  result["exitCode"] = code < 0 ? QJsonValue() : QJsonValue(code);
  result["output"] = headTail(session->model->plainText()).right(65536);
  const QPointer<ShellService> self(this);
  kill(session, "abort");
  if (self && owns(session)) {
    session->exitCode = code;
    emit watcherCompleted(result);
  }
}
QJsonObject ShellService::snapshot() const {
  QJsonArray sessions;
  for (const auto &session : m_sessions) {
    auto stats = m_logs ? m_logs->stats(session->id) : QJsonObject{};
    QJsonObject row{
        {"id", session->id},
        {"exited", session->exited},
        {"running",
         !session->exited && session->process && session->process->isRunning()},
        {"commandActive",
         !session->exited &&
             (session->commandActive || !session->inputPending.isEmpty() ||
              session->watcherActive)},
        {"exitCode",
         session->exitCode < 0 ? QJsonValue() : QJsonValue(session->exitCode)},
        {"cwd", session->cwd},
        {"createdAt", session->created},
        {"lineCount", stats["lineCount"]},
        {"logPath", "shells/" + session->id + ".shell.log"},
        {"tailContent",
         session->model ? session->model->plainText().right(12000) : QString()},
        {"truncated", stats["truncated"]}};
    if (!session->watcher.isEmpty())
      row["watcher"] = session->watcher;
    sessions.append(row);
  }
  return {{"shellType", "powershell"}, {"sessions", sessions}};
}
bool ShellService::ownsSession(const QString &id) const {
  return owns(m_sessions.value(id));
}
TerminalModel *ShellService::sessionModel(const QString &id) const {
  const auto session = m_sessions.value(id);
  return owns(session) ? session->model.data() : nullptr;
}
ConPtyProcess *ShellService::sessionProcess(const QString &id) const {
  const auto session = m_sessions.value(id);
  return owns(session) ? session->process.data() : nullptr;
}
QString ShellService::manifest() const {
  QString text = "<shell>\nPlatform: Windows\nShell: PowerShell (" +
                 shellExecutable().toHtmlEscaped() +
                 ")\n</shell>\n<shell-sessions>\n";
  for (const auto &session : m_sessions)
    if (!session->exited) {
      text += "  <session id=\"" + session->id.toHtmlEscaped() + "\" cwd=\"" +
              session->cwd.toHtmlEscaped() + "\"";
      if (session->watcherActive) {
        text += " watcher-title=\"" +
                session->watcher["title"].toString().toHtmlEscaped() + "\"";
        const auto description = session->watcher["description"].toString();
        if (!description.isEmpty())
          text +=
              " watcher-description=\"" + description.toHtmlEscaped() + "\"";
        text += " watcher-expires-at=\"" +
                QString::number(session->watcher["expiresAt"].toInteger()) +
                "\"";
      }
      text += " />\n";
    }
  return text + "</shell-sessions>";
}
} // namespace ade
