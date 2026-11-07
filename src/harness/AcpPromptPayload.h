#pragma once
#include "HarnessPreparedTurn.h"
#include <QByteArray>
namespace ade {
class JsonRpcProcess;
// Only this native ACP builder can create a large outbound payload. The stdio
// transport cannot use this capability for arbitrary methods or inbound data.
class AcpPromptPayload {
public:
  static constexpr qsizetype MaxFrameBytes = 64 * 1024 * 1024;
  static constexpr qsizetype MaxFallbackBytes = 4 * 1024 * 1024;
  bool valid() const {
    if (m_parameters.isEmpty()) return false;
    for (const auto &lease : {m_attachments.current,m_attachments.history})
      if (!lease.empty() && !lease.valid()) return false;
    return true;
  }
  qsizetype byteSize() const { return m_parameters.size(); }
private:
  QByteArray m_parameters;
  HarnessAttachments m_attachments;
  friend class JsonRpcProcess;
  friend AcpPromptPayload prepareAcpPrompt(const QString &, const QJsonArray &,
      const QJsonArray &, const HarnessAttachments &, bool, bool,
      const std::atomic_bool *, QString *);
};
AcpPromptPayload prepareAcpPrompt(const QString &sessionId,
    const QJsonArray &prefix, const QJsonArray &input,
    const HarnessAttachments &attachments, bool supportsImages, bool newSession,
    const std::atomic_bool *cancelled, QString *error);
} // namespace ade
