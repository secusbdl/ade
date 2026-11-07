#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
namespace ade {
// Durable definition owned by one configured HTTP provider instance. Options
// and headers are private data; only ProviderStore encrypts/persists them.
struct CustomModelConfig {
    QString modelId, displayName, description, providerInstanceId;
    int contextWindowSize = 128000;
    bool thinkingEnabled = false;
    QJsonObject capabilities;
    QJsonObject providerOptions;
    QJsonObject headers;
    CustomModelConfig();
    QJsonObject publicJson() const;
    QJsonObject privateJson() const;
    static CustomModelConfig fromPublicJson(const QJsonObject&, QString* error = nullptr);
    bool setPrivateJson(const QJsonObject&, QString* error = nullptr);
    bool validate(const QString& expectedProviderId, QString* error = nullptr) const;
};
QJsonObject defaultCustomModelCapabilities();
bool validCustomModelCapabilities(const QJsonObject&, QString* error = nullptr);
bool isReservedCustomModelId(const QString&);
}
