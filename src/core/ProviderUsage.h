#pragma once
#include <QJsonObject>
#include <QString>

namespace ade {
// Provider counters are cumulative snapshots for one request, never deltas.
// Missing counters stay missing. Billing prices/costs are deliberately absent.
class ProviderUsage {
public:
  void consume(const QString &protocol, const QJsonObject &event);
  QJsonObject counters() const;
  bool empty() const { return m_values.isEmpty(); }
  static bool validCounters(const QJsonObject &counters);
  static constexpr qint64 MaximumTokens = 2000000000LL;

private:
  void take(const QJsonObject &source, const char *key, const char *target);
  QJsonObject m_values;
  QString m_protocol;
};
} // namespace ade
