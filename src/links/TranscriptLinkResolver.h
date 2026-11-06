#pragma once
#include "attachments/AttachmentStore.h"
#include "attachments/FileReadSnapshotStore.h"
#include <QJsonObject>
#include <QSet>
#include <QUrl>
#include <atomic>
#include <memory>

namespace ade {
struct TranscriptLinkProvenance {
    // Host-created per-message values. Missing historic bindings never authorize
    // an alias such as ws1 to open a different current workspace.
    QJsonObject mountBindings;
    QString browserSessionId;
    QMap<QString, QUrl> historicalTabUrls;
    QMap<QString, QString> attachmentNames;
};
struct TranscriptLinkOptions {
    QString agentId, dataDirectory, browserSessionId;
    FileReadSnapshotStore::Lease lease;
    AttachmentMounts mounts;
    QSet<QString> liveTabIds, liveShellIds;
    QSet<QString> readOnlyMounts;
    qint64 maxFileBytes = 20 * 1024 * 1024;
};
class TranscriptLinkResolver {
    struct State;

  public:
    class Scope {
      public:
        bool isCurrent(const QString& expectedAgentId) const;
        QJsonObject mountBindings() const;
        void revoke() const;

      private:
        std::shared_ptr<State> state;
        friend class TranscriptLinkResolver;
    };
    enum class Kind { Invalid, NoOp, ExternalUrl, ActivateTab, ActivateShell, CopyColor, File };
    struct Parsed {
        Kind kind = Kind::Invalid;
        QString path, lineNumber, tabId, shellId, color;
        QJsonObject params;
        QUrl url;
        bool incomplete = false, directory = false;
    };
    struct Action {
        Kind kind = Kind::Invalid;
        QString mountedPath, canonicalPath, displayName, mimeType, tabId, shellId, color, lineNumber;
        QJsonObject params;
        QUrl url;
        QByteArray bytes;
        bool readOnly = true;
        bool isCurrent(const QString& expectedAgentId) const {
            return owner.isCurrent(expectedAgentId);
        }

      private:
        Scope owner;
        // Short-lived loan: keep alive through editor loading, then discard.
        // Holding it while saving would intentionally prevent atomic replacement.
        std::shared_ptr<void> pins;
        friend class TranscriptLinkResolver;
    };
    static Scope create(const TranscriptLinkOptions&, QString* error = nullptr);
    static Parsed parse(const QString& href, QString* error = nullptr);
    static Action resolve(const Scope&, const QString& expectedAgentId, const QString& href,
                          const TranscriptLinkProvenance& provenance = {},
                          const std::atomic_bool* canceled = nullptr, QString* error = nullptr);
};
} // namespace ade
