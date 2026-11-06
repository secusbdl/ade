#include "ClaudeControlProcess.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStringConverter>
#include <QUuid>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <tlhelp32.h>
#endif

namespace ade {
namespace {
constexpr qsizetype MaxFrame = 8 * 1024 * 1024;
#ifdef Q_OS_WIN
bool attachAndResume(HANDLE job, DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE |
                                   PROCESS_QUERY_LIMITED_INFORMATION,
                               FALSE, pid);
  const bool attached = process && AssignProcessToJobObject(job, process);
  if (process)
    CloseHandle(process);
  if (!attached)
    return false;
  bool resumed = false;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return false;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL ok = Thread32First(snapshot, &entry); ok;
       ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != pid)
      continue;
    HANDLE thread =
        OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
    if (thread) {
      resumed = ResumeThread(thread) != DWORD(-1) || resumed;
      CloseHandle(thread);
    }
  }
  CloseHandle(snapshot);
  return resumed;
}
#endif
} // namespace
ClaudeControlProcess::ClaudeControlProcess(QObject *parent) : QObject(parent) {
  m_clock.setInterval(100);
  connect(&m_clock, &QTimer::timeout, this,
          &ClaudeControlProcess::checkDeadlines);
}
ClaudeControlProcess::~ClaudeControlProcess() { stop(); }
QProcessEnvironment ClaudeControlProcess::managedEnvironment() {
  auto environment = QProcessEnvironment::systemEnvironment();
#ifdef Q_OS_WIN
  // A nested launch from the Codex desktop must not impersonate its registered
  // application/runtime or inherit that application's packaged shell.
  if (environment.contains("CODEX_WINDOWS_REGISTERED_CORE") ||
      environment.contains("CODEX_APP_RUNTIME_CLI_PATH") ||
      environment.contains("CODEX_SHELL"))
    environment.remove("SHELL");
#endif
  for (const auto &key : environment.keys()) {
    const auto upper = key.toUpper();
    // Use the CLI's own managed account. Never accidentally forward ADE or
    // inherited API credentials into a subscription-selected harness.
    if ((upper.startsWith("CODEX_") && upper != "CODEX_HOME") ||
        upper.contains("API_KEY") || upper.endsWith("_TOKEN") ||
        upper.endsWith("_SECRET") || upper.endsWith("_PASSWORD") ||
        upper.startsWith("ELECTRON_") || upper == "OPENAI_BASE_URL" ||
        upper == "OPENAI_ORGANIZATION" || upper == "NODE_OPTIONS" ||
        upper == "RUST_LOG")
      environment.remove(key);
  }
  environment.remove("CLAUDECODE");
  return environment;
}
bool ClaudeControlProcess::start(const QString &executable,
                                 const QStringList &arguments,
                                 const QString &directory,
                                 const QProcessEnvironment &environment) {
  stop();
  if (executable.isEmpty() || !QFileInfo(executable).isFile()) {
    emit failed("The selected coding-agent executable was not found.");
    return false;
  }
  auto *process = new QProcess(this);
  m_process = process;
  process->setProgram(executable);
  process->setArguments(arguments);
  process->setProcessEnvironment(environment);
  if (!directory.isEmpty())
    process->setWorkingDirectory(directory);
  const auto generation = m_generation;
#ifdef Q_OS_WIN
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                       &limits, sizeof(limits))) {
    if (job)
      CloseHandle(job);
    fail("Cannot create the coding-agent process job.");
    return false;
  }
  m_job = job;
  process->setCreateProcessArgumentsModifier(
      [](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW | CREATE_SUSPENDED;
      });
#endif
  const QPointer<QProcess> original(process);
  connect(
      process, &QProcess::started, this,
      [this, original, generation] {
        if (!original || m_process != original || generation != m_generation)
          return;
#ifdef Q_OS_WIN
        if (!attachAndResume(static_cast<HANDLE>(m_job),
                             DWORD(original->processId()))) {
          fail("Cannot isolate the coding-agent process tree.");
          return;
        }
#endif
        emit started();
      },
      Qt::QueuedConnection);
  connect(
      process, &QProcess::readyReadStandardOutput, this,
      [this, original, generation] {
        if (original && m_process == original && generation == m_generation)
          readOutput();
      },
      Qt::QueuedConnection);
  connect(
      process, &QProcess::readyReadStandardError, this,
      [this, original, generation] {
        if (original && m_process == original && generation == m_generation)
          original->readAllStandardError();
      },
      Qt::QueuedConnection);
  connect(
      process, &QProcess::errorOccurred, this,
      [this, original, generation](QProcess::ProcessError) {
        if (original && m_process == original && generation == m_generation)
          fail("The coding-agent process could not continue.");
      },
      Qt::QueuedConnection);
  connect(
      process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
      [this, original, generation](int, QProcess::ExitStatus) {
        if (!original || m_process != original || generation != m_generation)
          return;
        QPointer<ClaudeControlProcess> self(this);
        readOutput();
        if (!self || generation != m_generation || original != m_process)
          return;
        fail("The coding-agent process exited.");
      },
      Qt::QueuedConnection);
  m_clock.start();
  process->start();
  QTimer::singleShot(10000, this, [this, original, generation] {
    if (generation == m_generation && original && m_process == original &&
        original->state() == QProcess::Starting)
      fail("Starting the coding-agent process timed out.");
  });
  return true;
}
bool ClaudeControlProcess::isRunning() const {
  return m_process && m_process->state() == QProcess::Running;
}
bool ClaudeControlProcess::send(const QJsonObject &object) {
  if (!isRunning())
    return false;
  auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  if (bytes.size() > MaxFrame ||
      m_process->bytesToWrite() + bytes.size() > 2 * MaxFrame)
    return false;
  bytes += '\n';
  return m_process->write(bytes) == bytes.size();
}
QString ClaudeControlProcess::control(const QString &subtype,
                                      const QJsonObject &parameters,
                                      int timeoutMs) {
  if (subtype.isEmpty() || subtype.size() > 128 || m_deadlines.size() >= 128)
    return {};
  const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto request = parameters;
  request["subtype"] = subtype;
  if (!send({{"type", "control_request"},
             {"request_id", id},
             {"request", request}}))
    return {};
  m_deadlines.insert(id, QDateTime::currentMSecsSinceEpoch() +
                             qBound(100, timeoutMs, 600000));
  return id;
}
bool ClaudeControlProcess::sendUser(const QJsonArray &content,
                                    const QString &sessionId) {
  if (content.isEmpty() || sessionId.size() > 512 ||
      QJsonDocument(content).toJson(QJsonDocument::Compact).size() >
          4 * 1024 * 1024)
    return false;
  return send({{"type", "user"},
               {"message", QJsonObject{{"role", "user"}, {"content", content}}},
               {"parent_tool_use_id", QJsonValue::Null},
               {"session_id", sessionId}});
}
bool ClaudeControlProcess::respond(const QString &id,
                                   const QJsonObject &response) {
  if (!m_inbound.remove(id))
    return false;
  return send({{"type", "control_response"},
               {"response", QJsonObject{{"subtype", "success"},
                                        {"request_id", id},
                                        {"response", response}}}});
}
bool ClaudeControlProcess::reject(const QString &id, const QString &message) {
  if (!m_inbound.remove(id))
    return false;
  return send({{"type", "control_response"},
               {"response", QJsonObject{{"subtype", "error"},
                                        {"request_id", id},
                                        {"error", message.left(500)}}}});
}
void ClaudeControlProcess::readOutput() {
  if (!m_process)
    return;
  const auto generation = m_generation;
  QPointer<ClaudeControlProcess> self(this);
  while (m_process->bytesAvailable() > 0) {
    // QProcess's current read channel is stdout. Drain in bounded pieces.
    const auto chunk =
        m_process->read(qMin<qint64>(64 * 1024, m_process->bytesAvailable()));
    if (chunk.isEmpty())
      break;
    m_input += chunk;
    for (;;) {
      const auto end = m_input.indexOf('\n');
      if (end < 0)
        break;
      if (end > MaxFrame) {
        fail("Coding-agent protocol frame exceeded its limit.");
        return;
      }
      const auto line = m_input.left(end).trimmed();
      m_input.remove(0, end + 1);
      if (line.isEmpty())
        continue;
      QJsonParseError error;
      const auto document = QJsonDocument::fromJson(line, &error);
      if (error.error != QJsonParseError::NoError || !document.isObject()) {
        fail("The coding-agent returned malformed protocol data.");
        return;
      }
      QStringDecoder decoder(QStringDecoder::Utf8,
                             QStringConverter::Flag::Stateless);
      decoder.decode(line);
      if (decoder.hasError()) {
        fail("Claude returned invalid UTF-8 protocol data.");
        return;
      }
      const auto object = document.object();
      const auto type = object.value("type").toString();
      auto validId = [](const QJsonValue &value) {
        return value.isString() && !value.toString().isEmpty() &&
               value.toString().size() <= 512;
      };
      if (type == "control_response") {
        const auto response = object.value("response").toObject();
        if (!validId(response.value("request_id"))) {
          fail("Claude returned an invalid control response.");
          return;
        }
        const auto id = response.value("request_id").toString();
        if (m_deadlines.remove(id)) {
          const auto subtype = response.value("subtype").toString();
          if (subtype == "success" &&
              (response.value("response").isObject() ||
               response.value("response").isNull() ||
               response.value("response").isUndefined()))
            emit controlResponse(id, response.value("response").toObject(), {});
          else if (subtype == "error")
            emit controlResponse(id, {},
                                 "Claude rejected the control request.");
          else {
            fail("Claude returned an invalid control result.");
            return;
          }
        }
      } else if (type == "control_request") {
        const auto request = object.value("request").toObject();
        const auto id = object.value("request_id").toString();
        const auto subtype = request.value("subtype").toString();
        if (!validId(object.value("request_id")) || subtype.isEmpty() ||
            subtype.size() > 128 || m_inbound.contains(id) ||
            m_inbound.size() >= 128) {
          fail("Claude returned a duplicate or invalid control request.");
          return;
        }
        m_inbound.insert(id);
        emit controlRequest(id, subtype, request);
      } else if (type == "control_cancel_request") {
        if (!validId(object.value("request_id"))) {
          fail("Claude returned invalid cancellation data.");
          return;
        }
        const auto id = object.value("request_id").toString();
        if (m_inbound.remove(id))
          emit controlCanceled(id);
      } else if (type.isEmpty() || type.size() > 128) {
        fail("Claude returned a protocol message without a valid type.");
        return;
      } else
        emit frame(object);
      if (!self || generation != m_generation || !m_process)
        return;
    }
    if (m_input.size() > MaxFrame) {
      fail("Coding-agent protocol frame exceeded its limit.");
      return;
    }
  }
}
void ClaudeControlProcess::checkDeadlines() {
  const auto now = QDateTime::currentMSecsSinceEpoch();
  const auto generation = m_generation;
  QPointer<ClaudeControlProcess> self(this);
  const auto ids = m_deadlines.keys();
  for (const auto &id : ids) {
    if (!m_deadlines.contains(id) || m_deadlines.value(id) > now)
      continue;
    m_deadlines.remove(id);
    emit controlResponse(id, {}, "Claude control request timed out.");
    if (!self || generation != m_generation)
      return;
  }
}
void ClaudeControlProcess::stop() {
  ++m_generation;
  m_clock.stop();
  m_deadlines.clear();
  m_inbound.clear();
  m_input.clear();
  const auto process = m_process;
  m_process = nullptr;
  if (process)
    disconnect(process, nullptr, this, nullptr);
#ifdef Q_OS_WIN
  if (m_job) {
    CloseHandle(static_cast<HANDLE>(m_job));
    m_job = nullptr;
  }
#endif
  if (process) {
    if (process->state() != QProcess::NotRunning) {
      process->kill();
      process->waitForFinished(1000);
    }
    process->deleteLater();
  }
}
void ClaudeControlProcess::fail(const QString &error) {
  stop();
  emit failed(error);
}
} // namespace ade
