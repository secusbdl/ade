#pragma once

#include "ProviderConfig.h"
#include <QList>

namespace ade {

class ProviderStore {
public:
    static QList<ProviderConfig> load(const QString &path, QString *error = nullptr);
    static bool save(const QString &path, const QList<ProviderConfig> &providers,
                     QString *error = nullptr);
    // Imports dotenv-style PROVIDER_API_KEY=value entries. Unknown providers are
    // retained with an empty endpoint, so no credential is sent to a guessed host.
    static QList<ProviderConfig> importKeyFile(const QString &path, QString *error = nullptr);
    // Replacing an imported instance's credential/configuration keeps its model
    // visibility preference; newly imported instances have the default visibility.
    static QList<ProviderConfig> mergeImported(const QList<ProviderConfig>& existing,
                                              const QList<ProviderConfig>& imported);
};

} // namespace ade
