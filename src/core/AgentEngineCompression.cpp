#include "AgentEngine.h"
#include "ProviderProtocol.h"
#include "UtilityService.h"
#include <QJsonDocument>
#include <QUrl>

namespace ade {
void AgentEngine::setCompressionModels(const QList<ModelConfig> &models) {
  if (!m_busy || m_configuringTurn)
    m_compressionModels = models;
}
QJsonObject AgentEngine::currentUsageRecord() const {
  if (m_roundUsage.empty())
    return {};
  return {{"version", 1},
          {"id", m_usageId},
          {"complete", m_usageComplete},
          {"providerId", m_provider.id},
          {"baseUrl", m_provider.baseUrl},
          {"model", m_provider.model},
          {"actualModel", m_actualModel},
          {"protocol", ProviderProtocol::kind(m_provider)},
          {"counters", m_roundUsage.counters()}};
}
QJsonObject AgentEngine::latestUsage() const {
  for (auto i = m_messages.size(); i > 0; --i) {
    const auto message = m_messages[i - 1].toObject();
    if (message["role"] != "assistant")
      continue;
    const auto usage = message["metadata"].toObject()["usage"].toObject();
    // Never re-use an older count after a new unmetered/failed round, nor a
    // model/endpoint replacement. Imported metadata cannot invoke a tool.
    if (usage["version"] != 1 || usage["complete"] != true ||
        !usage["id"].isString() || usage["id"].toString().isEmpty() ||
        usage["id"].toString().size() > 128 ||
        usage["providerId"] != m_provider.id ||
        usage["model"] != m_provider.model ||
        usage["protocol"] != ProviderProtocol::kind(m_provider) ||
        usage["baseUrl"] != m_provider.baseUrl ||
        !ProviderUsage::validCounters(usage["counters"].toObject()))
      return {};
    return usage;
  }
  return {};
}
void AgentEngine::stopCompression() {
  if (!m_compressor)
    return;
  const auto service = m_compressor;
  m_compressor = nullptr;
  service->disconnect(this);
  service->cancel();
  if (service)
    service->deleteLater();
}
bool AgentEngine::maybeCompress(bool finishTurn) {
  if (m_compressor)
    return true;
  const int limit = m_provider.contextWindowTokens;
  if (!m_busy || limit <= 0 || limit > 100000000)
    return false;
  const auto usage = latestUsage();
  const auto count = usage["counters"].toObject().value("totalTokens");
  // ChatAgent uses 0.5, BaseAgent caps the trigger at 100k. usedTokens is the
  // most recent request total, NOT a lifetime sum of streamed snapshots.
  if (!count.isDouble() || count.toInteger() <= qMin<qint64>(limit / 2, 100000))
    return false;
  const auto key = usage["id"].toString();
  if (key == m_compressionAttempt)
    return false;
  m_compressionAttempt = key;
  const auto snapshot = m_messages;
  int previousBoundary = 0;
  for (int i = 1; i < snapshot.size(); ++i) {
    const auto row = snapshot[i].toObject();
    if (row["ade_compressedHistory"].toString().isEmpty())
      continue;
    previousBoundary = i;
    if (row["metadata"].toObject()["compression"].toObject()["usageId"] == key)
      return false;
  }
  const int boundary = UtilityService::compressionBoundary(snapshot, limit, 12);
  if (boundary <= previousBoundary)
    return false;
  QJsonArray prefix;
  for (int i = 0; i < boundary; ++i)
    prefix.append(snapshot[i]);
  auto *service = new UtilityService(this);
  m_compressor = service;
  auto mainModels = m_models;
  if (mainModels.isEmpty())
    mainModels.append(ModelConfig{m_provider, false});
  service->setMainModels(mainModels);
  service->setCompressionModels(m_compressionModels);
  const auto generation = m_generation;
  const QPointer<AgentEngine> self(this);
  const QPointer<UtilityService> expected(service);
  const auto settled = [self, expected, generation, snapshot, boundary, key,
                        finishTurn](const QString &briefing, QString error) {
    if (!self || !expected || self->m_compressor != expected ||
        self->m_generation != generation || !self->m_busy)
      return;
    self->m_compressor = nullptr;
    expected->disconnect(self);
    expected->deleteLater();
    bool applied = false;
    if (error.isEmpty() && self->m_messages == snapshot &&
        briefing.trimmed().size() >= 30 && briefing.size() <= 256 * 1024) {
      auto row = self->m_messages[boundary].toObject();
      row["ade_compressedHistory"] = briefing.trimmed();
      auto metadata = row["metadata"].toObject();
      metadata["compression"] =
          QJsonObject{{"version", 1}, {"kind", "automatic"}, {"usageId", key}};
      row["metadata"] = metadata;
      self->m_messages[boundary] = row;
      applied = true;
    } else if (error.isEmpty())
      error = "The compression result was stale or exceeded its size limit; "
              "original history was retained.";
    emit self->compressionFinished(applied, error);
    if (!self || self->m_generation != generation || !self->m_busy)
      return;
    if (applied) {
      emit self->displayChanged();
      if (!self || self->m_generation != generation || !self->m_busy)
        return;
    }
    // Utility failure is nonfatal. Preserve the complete history and resume
    // exactly the suspended next step; cancellation never enters this path.
    if (finishTurn)
      self->complete();
    else
      self->requestModel();
  };
  connect(service, &UtilityService::compressionReady, this,
          [settled](const QString &briefing) { settled(briefing, {}); });
  connect(service, &UtilityService::failed, this,
          [settled](const QString &error) { settled({}, error); });
  emit compressionStarted();
  if (!self || generation != m_generation || !m_busy ||
      m_compressor != expected)
    return true;
  service->compressHistory(prefix);
  return true;
}
} // namespace ade
