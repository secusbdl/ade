#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QByteArray>
#include <QStringList>
#include <functional>
namespace ade {
struct Conversation {
    QString id, title, workspace, providerId, model, presetId, draft, sideChatParentId;
    QJsonArray messages, draftAttachments, queuedMessages;
    QJsonObject transcriptLinkProvenance;
    qint64 created = 0, updated = 0, archivedAt = 0, lastMessageAt = 0;
    bool archived = false;
    bool alwaysAllow = false;
    bool titleLocked = false, autoTitleAttempted = false;
};
class SessionStore {
public:
    explicit SessionStore(QString directory);
    QList<Conversation> load(QString* error = nullptr) const;
    bool save(const QList<Conversation>& sessions, QString* error = nullptr) const;
    bool saveWithCleanup(const QList<Conversation>& sessions, const QStringList& pendingCleanup, QString* error = nullptr) const;
    QStringList pendingCleanup() const { return m_pendingCleanup; }
    using Cleanup = std::function<bool(const QString& agentId,const QString& directory,QString* error)>;
    bool processPendingCleanup(const QList<Conversation>& sessions,const Cleanup& cleanup,QString* error=nullptr) const;
    QString directory() const { return m_directory; }
    // Hash of bytes successfully loaded/saved, never speculative chat JSON.
    QByteArray durableRevision() const { return m_loaded&&m_valid?m_revision:QByteArray(); }
private:
    QString m_directory;
    mutable bool m_loaded=false;
    mutable bool m_valid=true;
    mutable bool m_existed=false;
    mutable QByteArray m_revision;
    mutable QStringList m_pendingCleanup;
    mutable QString m_canonicalDirectory;
    bool validateRevision(const QString& directory,QString* error) const;
    bool writeState(const QString& directory,const QList<Conversation>& sessions,const QStringList& pendingCleanup,QString* error) const;
};
}
