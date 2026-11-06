#include "OpenCodeModelDiscovery.h"
#include "harness/JsonRpcProcess.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStringConverter>
#include <cmath>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <tlhelp32.h>
namespace ade {
namespace {
constexpr qsizetype Maximum = 16 * 1024 * 1024;
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
int contextLimit(const QJsonValue &value) {
  return value.isDouble() && value.toDouble() > 0 &&
                 value.toDouble() <= 100000000 &&
                 value.toDouble() == std::floor(value.toDouble())
             ? value.toInt()
             : 0;
}
} // namespace
OpenCodeModelDiscovery::OpenCodeModelDiscovery(QObject *parent)
    : QObject(parent) {
  m_deadline.setSingleShot(true);
  connect(&m_deadline, &QTimer::timeout, this,
          [this] { finish("OpenCode model discovery timed out."); });
}
OpenCodeModelDiscovery::~OpenCodeModelDiscovery() { cancel(); }
QJsonArray OpenCodeModelDiscovery::parse(const QByteArray &output,
                                         QString *error) {
  if (error)
    error->clear();
  auto fail = [&](const QString &message) {
    if (error)
      *error = message;
    return QJsonArray{};
  };
  if (output.size() > Maximum)
    return fail("OpenCode model output exceeded 16 MiB.");
  QStringDecoder decoder(QStringDecoder::Utf8,
                         QStringConverter::Flag::Stateless);
  const QString text = decoder(output);
  if (decoder.hasError())
    return fail("OpenCode model output was not valid UTF-8.");
  static const QRegularExpression pattern(R"(^(\S+)\r?\n(\{[\s\S]*?^\}))",
                                          QRegularExpression::MultilineOption);
  QJsonArray models;
  QSet<QString> ids;
  auto matches = pattern.globalMatch(text);
  while (matches.hasNext()) {
    const auto match = matches.next();
    const auto id = match.captured(1);
    const auto metadata =
        QJsonDocument::fromJson(match.captured(2).toUtf8()).object();
    const auto provider = metadata["providerID"].toString(),
               name = metadata["name"].toString();
    if (id.isEmpty() || id.size() > 512 || ids.contains(id) ||
        id == "default" || provider.isEmpty() || name.isEmpty())
      continue;
    if (models.size() >= 9999)
      return fail("OpenCode model catalog exceeded its entry limit.");
    ids.insert(id);
    const auto caps = metadata["capabilities"].toObject(),
               input = caps["input"].toObject();
    QJsonArray modalities, efforts;
    if (input["text"].toBool(true))
      modalities.append("text");
    for (const auto *key : {"image", "audio", "video"})
      if (input[key].toBool())
        modalities.append(key);
    if (caps["attachment"].toBool() || input["pdf"].toBool())
      modalities.append("file");
    const auto variants = metadata["variants"].toObject();
    for (auto it = variants.begin();
         it != variants.end() && efforts.size() < 20; ++it)
      if (!it.key().isEmpty() && it.key().size() <= 32)
        efforts.append(it.key());
    const auto providerName =
        provider == "opencode"
            ? QString("OpenCode Zen")
            : (provider == "opencode-go" ? QString("OpenCode Go") : provider);
    QJsonObject item{{"id", id},
                     {"name", (name + " · " + providerName).left(512)},
                     {"inputTokenLimit",
                      contextLimit(metadata["limit"].toObject()["context"])},
                     {"reasoningEfforts", efforts},
                     {"defaultReasoningEffort", efforts.contains("medium")
                                                    ? QString("medium")
                                                    : QString{}},
                     {"thinkingEnabled", caps["reasoning"].toBool()},
                     {"inputModalities", modalities},
                     {"toolCalling", caps["toolcall"].toBool(true)}};
    item["description"] =
        ("Available through " + providerName + ".").left(8192);
    QJsonArray outputs;
    const auto output = caps["output"].toObject();
    if (output["text"].toBool(true))
      outputs.append("text");
    for (const auto *key : {"image", "audio", "video"})
      if (output[key].toBool())
        outputs.append(key);
    if (output["pdf"].toBool())
      outputs.append("file");
    item["outputModalities"] = outputs;
    const auto cost = metadata["cost"].toObject();
    if (cost["input"].isDouble() && cost["output"].isDouble() &&
        std::isfinite(cost["input"].toDouble()) &&
        std::isfinite(cost["output"].toDouble()) &&
        cost["input"].toDouble() >= 0 && cost["output"].toDouble() >= 0)
      item["pricing"] = QJsonObject{{"inputPerMillion", cost["input"]},
                                    {"outputPerMillion", cost["output"]}};
    models.append(item);
  }
  if (models.isEmpty())
    return fail("OpenCode returned no usable models. Install and sign in "
                "through its CLI first.");
  models.prepend(
      QJsonObject{{"id", "default"},
                  {"name", "Default · OpenCode"},
                  {"isDefault", true},
                  {"inputModalities", QJsonArray{"text", "image", "file"}}});
  return models;
}
bool OpenCodeModelDiscovery::start(const QString &requested) {
  if (isBusy())
    return false;
  const auto exe = QStandardPaths::findExecutable(
      requested.isEmpty() ? "opencode" : requested);
  if (exe.isEmpty()) {
    emit completed({}, "OpenCode is not installed. Install it and run opencode "
                       "auth login first.");
    return false;
  }
  ++m_generation;
  m_output.clear();
  m_executable = exe;
  auto *process = new QProcess(this);
  m_process = process;
  process->setProgram(exe);
  process->setArguments({"models", "--verbose"});
  process->setProcessEnvironment(JsonRpcProcess::managedEnvironment());
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                       &limits, sizeof(limits))) {
    if (job)
      CloseHandle(job);
    finish("Cannot isolate OpenCode discovery.");
    return false;
  }
  m_job = job;
  process->setCreateProcessArgumentsModifier(
      [](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW | CREATE_SUSPENDED;
      });
  const QPointer<QProcess> original(process);
  const auto generation = m_generation;
  const auto current = [this, original, generation] {
    return original && m_process == original && m_generation == generation;
  };
  connect(
      process, &QProcess::started, this,
      [this, current, original] {
        if (current() && !attachAndResume(static_cast<HANDLE>(m_job),
                                          DWORD(original->processId())))
          finish("Cannot isolate OpenCode discovery.");
      },
      Qt::QueuedConnection);
  connect(
      process, &QProcess::readyReadStandardOutput, this,
      [this, current] {
        if (current())
          read();
      },
      Qt::QueuedConnection);
  connect(
      process, &QProcess::readyReadStandardError, this,
      [current, original] {
        if (current())
          original->readAllStandardError();
      },
      Qt::QueuedConnection);
  connect(
      process, &QProcess::errorOccurred, this,
      [this, current](QProcess::ProcessError) {
        if (current())
          finish("OpenCode discovery could not continue.");
      },
      Qt::QueuedConnection);
  connect(
      process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
      [this, current](int code, QProcess::ExitStatus status) {
        if (!current())
          return;
        if (!read())
          return;
        finish(code == 0 && status == QProcess::NormalExit
                   ? QString{}
                   : QString("OpenCode discovery failed; existing provider "
                             "settings were preserved."));
      },
      Qt::QueuedConnection);
  m_deadline.start(30000);
  process->start();
  return true;
}
bool OpenCodeModelDiscovery::read() {
  if (!m_process)
    return false;
  while (m_process->bytesAvailable() > 0) {
    const auto chunk =
        m_process->read(qMin<qint64>(65536, m_process->bytesAvailable()));
    if (chunk.isEmpty())
      break;
    if (chunk.size() > Maximum - m_output.size()) {
      finish("OpenCode model output exceeded 16 MiB.");
      return false;
    }
    m_output += chunk;
  }
  return true;
}
void OpenCodeModelDiscovery::cancel() {
  ++m_generation;
  m_deadline.stop();
  const auto process = m_process;
  m_process = nullptr;
  if (process)
    disconnect(process, nullptr, this, nullptr);
  if (m_job) {
    CloseHandle(static_cast<HANDLE>(m_job));
    m_job = nullptr;
  }
  if (process) {
    if (process->state() != QProcess::NotRunning) {
      process->kill();
      process->waitForFinished(1000);
    }
    process->deleteLater();
  }
  m_output.clear();
}
void OpenCodeModelDiscovery::finish(const QString &failure) {
  if (!m_process)
    return;
  QString error = failure;
  QJsonArray models;
  if (error.isEmpty())
    models = parse(m_output, &error);
  cancel();
  emit completed(models, error);
}
} // namespace ade
