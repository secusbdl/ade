#pragma once
#include "AttachmentStore.h"
#include "FileReadSnapshotStore.h"
#include <QJsonArray>
#include <QCache>

namespace ade {
struct ModelMediaCapabilities {
    bool image = false;
    QStringList imageMimeTypes{"image/webp", "image/jpeg", "image/png"};
    qint64 maxImageBytes = 5 * 1024 * 1024;
    int maxWidth = 4096, maxHeight = 4096;
    qint64 maxPixels = 16 * 1024 * 1024;
};
struct AttachmentLimits {
    qint64 maxFileBytes = 64 * 1024 * 1024;
    int maxTextChars = 64 * 1024;
    int previewLines = 30;
    int maxReadChars = 40000; // Source soft line budget; maxTextChars caps the inner text body.
    int maxAttachments = 32;
    qint64 maxRequestImageBytes = 20 * 1024 * 1024;
};
// Produces transient {type:text,text} / {type:image,mediaType,dataBase64} content.
// Saved history keeps references and original user/tool text, never expanded binary data.
class AttachmentPipeline {
public:
    explicit AttachmentPipeline(AttachmentStore store);
    void setFileReadLease(FileReadSnapshotStore::Lease lease) { m_readLease=std::move(lease); }
    QJsonObject capture(const QString &agentId, const QJsonArray &references, const AttachmentMounts &mounts,
                        QString *error = nullptr);
    QJsonArray prepare(const QJsonArray &history, const QString &agentId, const AttachmentMounts &mounts,
                       const ModelMediaCapabilities &capabilities, const AttachmentLimits &limits = {}, QString *error = nullptr);
private:
    QJsonArray transform(const QString &agentId, const AttachmentRef &reference, const QString &expectedHash,
                         const QJsonObject &capturedMetadata,
                         const AttachmentMounts &mounts, const ModelMediaCapabilities &capabilities,
                         const AttachmentLimits &limits, QString *error,const FileReadParams* explicitParams=nullptr);
    AttachmentStore m_store;
    QCache<QString, QByteArray> m_originalCache;
    FileReadSnapshotStore::Lease m_readLease;
};
}
