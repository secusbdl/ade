#pragma once
#include "ProviderConfig.h"
#include <QJsonArray>
#include <QNetworkRequest>

namespace ade {
struct ProviderRequest { QNetworkRequest request; QJsonObject body; QString error; };
class ProviderProtocol {
public:
    static QString kind(const ProviderConfig &provider);
    static bool supports(const ProviderConfig &provider);
    static ProviderRequest prepare(const ProviderConfig &provider, const QJsonArray &messages,
                                   const QString &system, const QJsonArray &tools);
    static void authorize(QNetworkRequest &request, const ProviderConfig &provider);
};
}
