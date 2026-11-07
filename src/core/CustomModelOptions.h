#pragma once
#include "ProviderConfig.h"
#include <QNetworkRequest>
namespace ade {
// Pinned SDK option translations only. No arbitrary body merge or routing
// override; errors name the unsupported field but never echo private values.
class CustomModelOptions {
public:
  static bool validate(const QString &protocol, const QJsonObject &options,
                       const QJsonObject &headers, QString *error = nullptr);
  // Atomic: neither body nor request is changed on validation failure.
  // Empty customRequest is a no-op; ordinary provider behavior is unchanged.
  static bool apply(const ProviderConfig &, QJsonObject *body,
                    QNetworkRequest *request, QString *error = nullptr);
  // Host-only diagnostic redaction. Never use on successful file/tool output.
  static QString redactDiagnostic(const ProviderConfig &, QString text);
  // Exact bound custom capability. Invalid binding fails closed; apply()
  // reports its detailed error before any network request.
  static bool allowsTools(const ProviderConfig &);
};
} // namespace ade
