#pragma once
#include "core/ProviderConfig.h"
#include <QList>
#include <QStringList>
namespace ade {
struct ProviderTemplate {
    QString id, group, name, description, baseUrl, protocol, keyUrl, helpText;
    QString kind = QStringLiteral("api");
    QString subscribeUrl{};
    bool configurableEndpoint = false;
};
// Frozen source-derived setup choices. The catalog never contains API keys or
// model availability claims; discovery and connection checks remain explicit.
class ProviderCatalog {
public:
    static const QList<ProviderTemplate>& all();
    static const ProviderTemplate* find(const QString& id);
    static QStringList groups();
    static ProviderConfig instantiate(const QString& id);
    static bool matches(const ProviderConfig& provider, const ProviderTemplate& entry);
    static void clearChangedProvenance(ProviderConfig& provider);
};
}
