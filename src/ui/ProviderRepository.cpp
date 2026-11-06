#include "ProviderRepository.h"
#include "core/ProviderStore.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
namespace ade {
QList<ProviderConfig> ProviderRepository::load(QString* error) {
    QString failure;m_loaded=true;m_valid=false;m_revision.clear();
    QFile file(m_path);m_existed=file.exists();
    if(m_existed && (!file.open(QIODevice::ReadOnly)||file.size()>4*1024*1024))failure="Cannot read provider storage or it exceeds 4 MiB.";
    if(failure.isEmpty()&&m_existed)m_revision=QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256);
    file.close();
    const auto providers=failure.isEmpty()?ProviderStore::load(m_path,&failure):QList<ProviderConfig>{};
    // Detect a change between reading the revision and decrypting its contents.
    if(failure.isEmpty()&&m_existed){if(!file.open(QIODevice::ReadOnly)||QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256)!=m_revision)failure="Provider storage changed while loading. Retry.";}
    if(failure.isEmpty()&&!m_existed&&file.exists())failure="Provider storage appeared while loading. Retry.";
    m_valid=failure.isEmpty();if(error)*error=failure;return m_valid?providers:QList<ProviderConfig>{};
}
bool ProviderRepository::save(const QList<ProviderConfig>& providers,QString* error) {
    QString failure;if(!m_loaded)load(&failure);
    if(!m_valid){if(error)*error=failure.isEmpty()?"Provider storage failed validation; original credentials were preserved.":failure;return false;}
    if(!QDir().mkpath(QFileInfo(m_path).absolutePath())){if(error)*error="Cannot create provider storage directory.";return false;}
    QLockFile lock(m_path+".lock");if(!lock.tryLock(0)){if(error)*error="Another window is saving providers. Retry shortly.";return false;}
    QFile file(m_path);
    if(file.exists()!=m_existed || (m_existed&&(!file.open(QIODevice::ReadOnly)||file.size()>4*1024*1024||QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256)!=m_revision))){if(error)*error="Providers changed in another window. Reopen provider settings before saving.";return false;}
    file.close();
    if(!ProviderStore::save(m_path,providers,error))return false;
    if(!file.open(QIODevice::ReadOnly)){m_valid=false;if(error)*error="Providers were written but could not be verified.";return false;}
    m_revision=QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256);m_existed=true;return true;
}
}
