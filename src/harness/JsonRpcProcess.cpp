#include "JsonRpcProcess.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>
#include <algorithm>
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
constexpr qint64 WriteChunk = 256 * 1024, WriteHighWater = 1024 * 1024;
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
JsonRpcProcess::JsonRpcProcess(QObject *parent) : QObject(parent) {
  m_clock.setInterval(100);
  connect(&m_clock, &QTimer::timeout, this, &JsonRpcProcess::checkDeadlines);
}
JsonRpcProcess::~JsonRpcProcess() { stop(); }
QProcessEnvironment JsonRpcProcess::managedEnvironment() {
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
  return environment;
}
bool JsonRpcProcess::start(const QString &executable,
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
        QPointer<JsonRpcProcess> self(this);
        readOutput();
        if (!self || generation != m_generation || original != m_process)
          return;
        fail("The coding-agent process exited.");
      },
      Qt::QueuedConnection);
  connect(process, &QProcess::bytesWritten, this,
      [this, original, generation](qint64 count) {
        if (original && m_process == original && generation == m_generation)
          written(count);
      }, Qt::QueuedConnection);
  m_clock.start();
  process->start();
  QTimer::singleShot(10000, this, [this, original, generation] {
    if (generation == m_generation && original && m_process == original &&
        original->state() == QProcess::Starting)
      fail("Starting the coding-agent process timed out.");
  });
  return true;
}
bool JsonRpcProcess::isRunning() const {
  return m_process && m_process->state() == QProcess::Running;
}
bool JsonRpcProcess::enqueue(OutboundFrame frame) {
  if (!isRunning() || frame.size <= 0 || m_outbound.size() >= 512)
    return false;
  for (const auto &sent : m_sent)
    if (sent.acp && !sent.prompt.valid()) return false;
  if (!frame.acp && frame.size > 2 * MaxFrame - m_normalOutstanding)
    return false;
  if (frame.acp && (!m_acpWriteId.isEmpty() || frame.size > AcpPromptPayload::MaxFrameBytes))
    return false;
  frame.end = m_totalPlanned + frame.size;
  if (frame.acp) {
    m_acpWriteId = frame.requestId;
    m_acpStart = m_totalPlanned;
    m_acpEnd = frame.end;
  } else m_normalOutstanding += frame.size;
  m_totalPlanned = frame.end;
  m_outbound.enqueue(std::move(frame));
  schedulePump();
  return true;
}
void JsonRpcProcess::schedulePump() {
  if (m_pumpScheduled) return;
  m_pumpScheduled = true;
  const auto generation = m_generation;
  QTimer::singleShot(0, this, [this, generation] {
    if (generation != m_generation) return;
    m_pumpScheduled = false;
    pumpWrites();
  });
}
void JsonRpcProcess::pumpWrites() {
  if (!isRunning()) return;
  while (!m_outbound.isEmpty() && m_process->bytesToWrite() < WriteHighWater) {
    auto &frame = m_outbound.head();
    if (frame.acp && !frame.prompt.valid()) {
      fail("Captured ACP input was revoked before transmission completed."); return;
    }
    while (frame.segment < frame.segments.size() &&
           frame.offset == frame.segments[frame.segment].size()) {
      ++frame.segment; frame.offset = 0;
    }
    if (frame.segment == frame.segments.size()) {
      m_sent.enqueue({frame.end,frame.size,frame.acp,frame.prompt});
      m_outbound.dequeue();
      continue;
    }
    const auto &segment = frame.segments[frame.segment];
    const auto count = std::min<qint64>({WriteChunk, WriteHighWater - m_process->bytesToWrite(),
                                   segment.size() - frame.offset});
    const auto accepted = m_process->write(segment.constData() + frame.offset,count);
    if (accepted <= 0) { fail("The coding-agent input pipe could not continue."); return; }
    frame.offset += accepted;
    m_totalQueued += accepted;
    m_peakQueuedBytes = qMax(m_peakQueuedBytes,m_process->bytesToWrite());
    while (frame.segment < frame.segments.size() &&
           frame.offset == frame.segments[frame.segment].size()) {
      ++frame.segment; frame.offset = 0;
    }
    if (frame.segment == frame.segments.size()) {
      m_sent.enqueue({frame.end,frame.size,frame.acp,frame.prompt});
      m_outbound.dequeue();
    }
  }
}
void JsonRpcProcess::written(qint64 count) {
  Q_UNUSED(count)
  if (!m_process) return;
  m_totalWritten = qMax(m_totalWritten, m_totalQueued - m_process->bytesToWrite());
  if (!m_acpWriteId.isEmpty() && m_totalWritten >= m_acpEnd)
    m_acpWriteId.clear();
  while (!m_sent.isEmpty() && m_totalWritten >= m_sent.head().end) {
    // Observe revocation before retiring the final queued grant, including
    // when Qt drained it between owner cancellation and this callback. The
    // clock reports failure terminally; this bookkeeping emits no callbacks.
    if (m_sent.head().acp && !m_sent.head().prompt.valid()) break;
    const auto frame=m_sent.dequeue();
    if (!frame.acp) m_normalOutstanding -= frame.size;
  }
  schedulePump();
}
bool JsonRpcProcess::send(const QJsonObject &object) {
  if (!isRunning()) return false;
  auto bytes=QJsonDocument(object).toJson(QJsonDocument::Compact);
  if (bytes.size() > MaxFrame) return false;
  bytes += '\n';
  OutboundFrame frame;
  frame.size=bytes.size();frame.segments.append(std::move(bytes));
  return enqueue(std::move(frame));
}
QString JsonRpcProcess::requestAcpPrompt(const AcpPromptPayload &payload, int timeoutMs) {
  if (!payload.valid() || m_deadlines.size() >= 256) return {};
  written(0);
  const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto prefix=QByteArray("{\"jsonrpc\":\"2.0\",\"id\":\"")+id.toLatin1()+
      "\",\"method\":\"session/prompt\",\"params\":";
  const QByteArray suffix("}\n");
  OutboundFrame frame;
  frame.acp=true;frame.requestId=id;frame.prompt=payload;
  frame.size=prefix.size()+payload.m_parameters.size()+suffix.size();
  frame.segments={prefix,payload.m_parameters,suffix};
  if (!enqueue(std::move(frame))) return {};
  m_deadlines.insert(id,QDateTime::currentMSecsSinceEpoch()+qBound(100,timeoutMs,600000));
  return id;
}
JsonRpcProcess::PromptWriteCancel JsonRpcProcess::cancelAcpPromptWrite(const QString &id) {
  written(0);
  if (id.isEmpty() || id != m_acpWriteId) return PromptWriteCancel::NotPending;
  if (m_totalQueued > m_acpStart) {
    // A partially transmitted JSON frame cannot be followed by session/cancel.
    // This transport owns the child/job, so stop only that process tree.
    stop();
    return PromptWriteCancel::StoppedPartial;
  }
  const auto size=m_acpEnd-m_acpStart;
  bool removed=false;
  for (auto it=m_outbound.begin();it!=m_outbound.end();) {
    if (it->acp && it->requestId==id) { it=m_outbound.erase(it);removed=true; }
    else { if (removed) it->end-=size; ++it; }
  }
  if (!removed) { stop(); return PromptWriteCancel::StoppedPartial; }
  m_totalPlanned-=size;m_acpWriteId.clear();
  m_deadlines.remove(id);m_pausedDeadlines.remove(id);
  schedulePump();
  return PromptWriteCancel::RemovedBeforeWrite;
}
QString JsonRpcProcess::request(const QString &method,
                                const QJsonObject &parameters, int timeoutMs) {
  if (method.isEmpty() || m_deadlines.size() >= 256)
    return {};
  const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (!send({{"jsonrpc", "2.0"},
             {"id", id},
             {"method", method},
             {"params", parameters}}))
    return {};
  m_deadlines.insert(id, QDateTime::currentMSecsSinceEpoch() +
                             qBound(100, timeoutMs, 600000));
  return id;
}
bool JsonRpcProcess::notify(const QString &method,
                            const QJsonObject &parameters) {
  return send({{"jsonrpc", "2.0"}, {"method", method}, {"params", parameters}});
}
bool JsonRpcProcess::respond(const QJsonValue &id, const QJsonValue &result) {
  if (!id.isString() && !id.isDouble())
    return false;
  return send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}
bool JsonRpcProcess::reject(const QJsonValue &id, int code,
                            const QString &message) {
  if (!id.isString() && !id.isDouble())
    return false;
  return send(
      {{"jsonrpc", "2.0"},
       {"id", id},
       {"error", QJsonObject{{"code", code}, {"message", message.left(500)}}}});
}
void JsonRpcProcess::readOutput() {
  if (!m_process)
    return;
  const auto generation = m_generation;
  QPointer<JsonRpcProcess> self(this);
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
      const auto object = document.object();
      const auto method = object.value("method").toString();
      if (!method.isEmpty()) {
        if (!object.value("params").isObject() &&
            !object.value("params").isUndefined() &&
            !object.value("params").isNull()) {
          fail("The coding-agent returned invalid protocol parameters.");
          return;
        }
        if (object.contains("id"))
          emit serverRequest(object.value("id"), method,
                             object.value("params").toObject());
        else
          emit notification(method, object.value("params").toObject());
      } else {
        const auto id = object.value("id").toString();
        if (!id.isEmpty() && m_deadlines.remove(id)) {
          m_pausedDeadlines.remove(id);
          emit response(id, object.value("result"),
                        object.value("error").toObject());
        }
      }
      if (!self || generation != m_generation || !m_process)
        return;
    }
    if (m_input.size() > MaxFrame) {
      fail("Coding-agent protocol frame exceeded its limit.");
      return;
    }
  }
}
void JsonRpcProcess::checkDeadlines() {
  written(0); // Refresh the actual QProcess queue before testing retained grants.
  for (const auto &frame : m_sent)
    if (frame.acp && !frame.prompt.valid()) {
      fail("Captured ACP input was revoked before its queued bytes drained."); return;
    }
  for (const auto &frame : m_outbound)
    if (frame.acp && !frame.prompt.valid()) {
      fail("Captured ACP input was revoked before transmission completed."); return;
    }
  const auto now = QDateTime::currentMSecsSinceEpoch();
  const auto generation = m_generation;
  QPointer<JsonRpcProcess> self(this);
  const auto ids = m_deadlines.keys();
  for (const auto &id : ids) {
    if (!m_deadlines.contains(id) || m_pausedDeadlines.contains(id) ||
        m_deadlines.value(id) > now)
      continue;
    const auto cancelled = cancelAcpPromptWrite(id);
    const auto afterCancel = m_generation;
    m_deadlines.remove(id);
    emit response(
        id, {},
        {{"code", -32001}, {"message", "Coding-agent request timed out."}});
    if (!self) return;
    if (cancelled == PromptWriteCancel::StoppedPartial) {
      if (afterCancel == m_generation)
        emit failed("The coding-agent input timed out during a partial ACP frame.");
      return;
    }
    if (generation != m_generation) return;
  }
}
bool JsonRpcProcess::suspendDeadline(const QString &id) {
  if (!m_deadlines.contains(id))
    return false;
  if (!m_pausedDeadlines.contains(id))
    m_pausedDeadlines.insert(
        id, qMax<qint64>(1, m_deadlines.value(id) -
                                QDateTime::currentMSecsSinceEpoch()));
  return true;
}
bool JsonRpcProcess::resumeDeadline(const QString &id) {
  const auto found = m_pausedDeadlines.find(id);
  if (found == m_pausedDeadlines.end() || !m_deadlines.contains(id))
    return false;
  const auto remaining = *found;
  m_pausedDeadlines.erase(found);
  m_deadlines[id] = QDateTime::currentMSecsSinceEpoch() + remaining;
  return true;
}
void JsonRpcProcess::stop() {
  ++m_generation;
  m_clock.stop();
  m_deadlines.clear();
  m_pausedDeadlines.clear();
  m_input.clear();
  m_outbound.clear(); m_sent.clear(); m_acpWriteId.clear();
  m_totalPlanned = m_totalQueued = m_totalWritten = m_normalOutstanding = 0;
  m_acpStart = m_acpEnd = 0; m_pumpScheduled = false;
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
void JsonRpcProcess::fail(const QString &error) {
  stop();
  emit failed(error);
}
} // namespace ade
