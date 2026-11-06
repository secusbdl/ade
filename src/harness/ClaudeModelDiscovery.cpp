#include "ClaudeModelDiscovery.h"
#include <QJsonDocument>
#include <QStandardPaths>
#include <QUuid>
namespace ade {
ClaudeModelDiscovery::ClaudeModelDiscovery(QObject *parent)
    : QObject(parent), m_process(this) {
  connect(&m_process, &ClaudeControlProcess::started, this, [this] {
    if (m_busy)
      m_initializeId =
          m_process.control("initialize", {{"hooks", QJsonValue::Null}}, 45000);
  });
  connect(&m_process, &ClaudeControlProcess::controlRequest, this,
          [this](const QString &id, const QString &, const QJsonObject &) {
            m_process.reject(
                id, "Model discovery does not authorize tool actions.");
          });
  connect(&m_process, &ClaudeControlProcess::failed, this,
          [this](const QString &error) {
            if (m_busy)
              finish({}, error);
          });
  connect(&m_process, &ClaudeControlProcess::controlResponse, this,
          [this](const QString &id, const QJsonObject &response,
                 const QString &error) {
            if (!m_busy || id != m_initializeId)
              return;
            if (!error.isEmpty()) {
              finish({}, error);
              return;
            }
            QString parseError;
            const auto models = parse(response, &parseError);
            finish(models, parseError);
          });
}
ClaudeModelDiscovery::~ClaudeModelDiscovery() { cancel(); }
bool ClaudeModelDiscovery::start(const QString &executable) {
  cancel();
  m_executable = QStandardPaths::findExecutable(
      executable.isEmpty() ? "claude" : executable);
  if (m_executable.isEmpty()) {
    emit completed({}, "The selected Claude executable was not found.");
    return false;
  }
  m_scratch = std::make_unique<QTemporaryDir>();
  if (!m_scratch->isValid()) {
    m_scratch.reset();
    emit completed({}, "Could not create a discovery working directory.");
    return false;
  }
  m_busy = true;
  // No --bare: managed CLI sign-in remains available. No user prompt is sent.
  return m_process.start(
      m_executable,
      {"--print", "--input-format", "stream-json", "--output-format",
       "stream-json", "--verbose", "--permission-prompt-tool", "stdio",
       "--permission-mode", "default",
       "--session-id=" + QUuid::createUuid().toString(QUuid::WithoutBraces)},
      m_scratch->path(), ClaudeControlProcess::managedEnvironment());
}
void ClaudeModelDiscovery::cancel() {
  m_busy = false;
  m_initializeId.clear();
  m_process.stop();
  m_scratch.reset();
}
void ClaudeModelDiscovery::finish(const QJsonArray &models,
                                  const QString &error) {
  cancel();
  emit completed(models, error);
}
QJsonArray ClaudeModelDiscovery::parse(const QJsonObject &response,
                                       QString *error) {
  if (error)
    error->clear();
  auto fail = [&](const QString &message) {
    if (error)
      *error = message;
    return QJsonArray{};
  };
  if (!response.value("models").isArray() ||
      response.value("models").toArray().size() > 1000 ||
      QJsonDocument(response).toJson(QJsonDocument::Compact).size() >
          8 * 1024 * 1024)
    return fail("Claude returned an invalid or oversized model catalog.");
  QJsonArray result;
  QSet<QString> ids;
  for (const auto &value : response.value("models").toArray()) {
    if (!value.isObject())
      return fail("Claude returned an invalid model entry.");
    const auto model = value.toObject();
    const auto id = model.value("value").toString();
    if (id.isEmpty() || id.size() > 512 || ids.contains(id) ||
        !model.value("displayName").isString() ||
        model.value("displayName").toString().size() > 512)
      return fail("Claude returned an invalid model identity.");
    ids.insert(id);
    QJsonArray efforts;
    if (model.value("supportsEffort").toBool())
      for (const auto &value : model.value("supportedEffortLevels").toArray()) {
        if (value.isString() &&
            QStringList{"low", "medium", "high", "xhigh", "max"}.contains(
                value.toString()) &&
            !efforts.contains(value))
          efforts.append(value);
      }
    QJsonObject safe{{"id", id},
                     {"name", model.value("displayName")},
                     {"reasoningEfforts", efforts},
                     {"isDefault", id == "default"},
                     // Pinned Stagewise external-agent.ts9-24 declares this
                     // transport capability; it is not account/model access.
                     {"inputModalities", QJsonArray{"text", "image", "file"}}};
    if (model.value("resolvedModel").isString() &&
        model.value("resolvedModel").toString().size() <= 512)
      safe["resolvedModel"] = model.value("resolvedModel");
    result.append(safe);
  }
  if (result.isEmpty())
    return fail("Claude returned no model choices.");
  return result;
}
} // namespace ade
