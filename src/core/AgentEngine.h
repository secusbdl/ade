#pragma once

#include "ProviderConfig.h"
#include "ModelFallback.h"
#include "SseParser.h"
#include "ToolExecutor.h"
#include "AgentPresentation.h"
#include <QJsonArray>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>

namespace ade {

class AgentEngine final : public QObject {
    Q_OBJECT
public:
    explicit AgentEngine(QObject *parent = nullptr);
    ~AgentEngine() override;
    void setProvider(const ProviderConfig &provider);
    void setModelChain(const QList<ModelConfig> &models, const QString &presetId);
    bool applyCompression(int boundaryIndex, const QString &briefing);
    void setWorkspace(const QString &path);
    bool setAppsDirectory(const QString &path);
    void setSystemContext(const QString &context);
    void submit(const QString &prompt);
    // True once the user message is accepted into history, even if that turn
    // then fails or is cancelled synchronously by an observer.
    bool submitWithAttachments(const QString &prompt, const QJsonArray &references, const QJsonObject &pathReferences = {}, const QJsonObject &pathMetadata = {});
    using MessagePreparer = std::function<QJsonArray(const QJsonArray &, const ProviderConfig &, QString *)>;
    void setMessagePreparer(MessagePreparer preparer);
    void setFileReadCommitter(FileReadCommitter committer);
    void setAttachmentReadSource(FileReadSource source);
    void cancel();
    void approveTool(const QString &id, bool allow);
    void setAlwaysAllow(bool enabled) { m_alwaysAllow = enabled; }
    void setMessages(const QJsonArray &messages);
    QJsonArray messages() const { return m_messages; }
    QJsonArray displayMessages() const;
    static QJsonArray displayHistory(const QJsonArray& history);
    bool isBusy() const { return m_busy; }
    bool lastTurnSucceeded() const { return m_lastTurnSucceeded; }
    QString workspace() const { return m_tools.workspace(); }
    ProviderConfig provider() const { return m_provider; }
    QString actualModel() const { return m_actualModel; }
    void setExternalTools(const QJsonArray &definitions);
    void completeExternalTool(const QString &requestId, const QString &output, bool error);
    // UI-only bounded preview, accepted only for the currently running opaque
    // external request token. The final canonical tool result replaces it.
    bool appendExternalToolOutput(const QString &requestId,const QString &text);
    void completeExternalToolWithAttachments(const QString &requestId, const QString &output, bool error,
                                             const QJsonArray &references, const QJsonObject &pathReferences = {}, const QJsonObject &pathMetadata = {});

signals:
    void textDelta(QString text);
    void toolStarted(QString id, QString name, QJsonObject arguments);
    void toolFinished(QString id, QString output, bool error);
    void approvalRequired(QString id, QString name, QJsonObject arguments);
    void finished();
    void failed(QString error);
    void busyChanged(bool busy);
    void fileChanged(QString path, QByteArray before, QByteArray after);
    void reasoningDelta(QString text);
    void displayChanged();
    void externalToolRequested(QString requestId, QString name, QJsonObject arguments);
    void externalToolCancellationRequested(QString requestId);
    void effectiveProviderChanged(QString providerId, QString modelId, int index);

private:
    struct ToolCall { QString id, name, arguments; };
    void requestModel();
    void readReply();
    void finishReply();
    void consumeEvent(const QByteArray &event);
    void consumeCompletion(const QJsonObject &object, bool streaming);
    void consumeAnthropic(const QJsonObject &object, bool streaming);
    void consumeGoogle(const QJsonObject &object);
    void consumeResponses(const QJsonObject &object, bool streaming);
    void dispatchTool(const ToolCall &call, const QJsonObject &arguments);
    void runNextTool();
    void recordToolResult(const QString &id, const QString &output, bool error, const QJsonObject &metadata = {}, const QString &displayState = {});
    void syncDisplayMetadata();
    void retainPartialResponse();
    void settleOutstandingTools(const QString &reason);
    void complete();
    void fail(const QString &message);
    QString redact(QString value) const;
    void stopNetwork();

    ProviderConfig m_provider;
    QList<ModelConfig> m_models;
    QString m_presetId;
    QString m_systemContext;
    MessagePreparer m_messagePreparer;
    ModelFallback m_fallback;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    ToolExecutor m_tools;
    QTimer m_deadline;
    QTimer m_externalDeadline;
    SseParser m_parser;
    QJsonArray m_messages;
    AgentPresentation m_display;
    int m_displayHistoryIndex = -1;
    QMap<int, ToolCall> m_streamCalls;
    QMap<int, QJsonObject> m_nativeBlocks;
    QJsonArray m_googleParts;
    QJsonArray m_externalDefinitions;
    QStringList m_externalNames;
    QString m_externalRequestId;
    bool m_externalOutputTruncated = false;
    QList<ToolCall> m_pendingTools;
    int m_toolIndex = 0;
    QString m_waitingApproval;
    QString m_streamText;
    QString m_streamReasoning;
    QString m_actualModel;
    QString m_streamError;
    QString m_finishReason;
    QByteArray m_responseBody;
    bool m_sse = true;
    bool m_done = false;
    bool m_busy = false;
    bool m_lastTurnSucceeded = false;
    bool m_alwaysAllow = false;
    bool m_responseRetained = false;
    int m_round = 0;
    int m_retry = 0;
    quint64 m_generation = 0;
};

} // namespace ade
