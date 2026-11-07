#pragma once
#include "HarnessAttachmentStore.h"
#include <QJsonArray>
#include <QStringList>
#include <functional>
namespace ade {
// Host-only typed input. Ordinary message JSON cannot supply these leases.
struct HarnessAttachments {
  HarnessAttachmentLease current;
  HarnessAttachmentLease history;
  QStringList inlineReferences;
};
struct HarnessPreparedTurn {
  QJsonArray input;
  HarnessAttachments attachments;
  QString error;
};
using HarnessInputJob =
    std::function<HarnessPreparedTurn(const std::atomic_bool *)>;
// Transport-specific construction occurs after actual runtime capabilities and
// new/resumed session state are known. Permission configuration is untouched.
QJsonArray mapHarnessAttachments(const QJsonArray &input,
                                 const HarnessAttachments &attachments,
                                 bool acp, bool supportsInlineImages,
                                 bool newSession, QString *error);
} // namespace ade
