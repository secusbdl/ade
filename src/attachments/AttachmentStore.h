#pragma once
#include <QJsonObject>
#include <QDateTime>
#include <QMap>
#include <QString>

namespace ade {
struct AttachmentRef {
    QString path, originalFileName;
    QJsonObject toJson() const;
    static AttachmentRef fromJson(const QJsonObject &object, QString *error = nullptr);
    bool isEmpty() const { return path.isEmpty(); }
};
using AttachmentMounts = QMap<QString, QString>;
class AttachmentStore {
public:
    explicit AttachmentStore(QString dataDirectory);
    AttachmentRef importFile(const QString &agentId, const QString &absoluteFile,
                             const AttachmentMounts &mounts, QString *error = nullptr) const;
    AttachmentRef write(const QString &agentId, const QString &originalName,
                        const QByteArray &bytes, QString *error = nullptr) const;
    AttachmentRef existingBlob(const QString &agentId, const QString &fileName,
                               const QString &originalName = {}, QString *error = nullptr) const;
    QByteArray read(const QString &agentId, const AttachmentRef &reference, const AttachmentMounts &mounts,
                    QString *error = nullptr, qint64 maxBytes = 64 * 1024 * 1024, QDateTime *modified = nullptr) const;
    QString resolve(const QString &agentId, const AttachmentRef &reference, const AttachmentMounts &mounts,
                    QString *error = nullptr) const;
    bool cloneAgent(const QString &sourceAgentId, const QString &targetAgentId, QString *error = nullptr) const;
    bool removeAgent(const QString &agentId, QString *error = nullptr) const;
    QString agentDirectory(const QString &agentId, QString *error = nullptr, bool create = false) const;
    bool saveSnapshot(const QString &agentId, const AttachmentRef &reference, const QByteArray &bytes, QString *error = nullptr) const;
    QByteArray readSnapshot(const QString &agentId, const AttachmentRef &reference, const QString &hash,
                            QString *error = nullptr) const;
private:
    QString ownedDirectory(const QString &agentId, const QString &category, QString *error, bool create) const;
    QString m_dataDirectory;
};
}
