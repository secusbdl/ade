#pragma once
#include "core/ProviderConfig.h"
#include <QByteArray>
#include <QList>
namespace ade {
// Guards the encrypted provider file against corrupt and concurrent overwrites.
class ProviderRepository {
public:
    explicit ProviderRepository(QString path):m_path(std::move(path)){}
    QList<ProviderConfig> load(QString* error=nullptr);
    bool save(const QList<ProviderConfig>& providers,QString* error=nullptr);
private:
    QString m_path;
    QByteArray m_revision;
    bool m_loaded=false,m_valid=false,m_existed=false;
};
}
