#pragma once

#include "ProviderConfig.h"
#include "ModelFallback.h"
#include "SseParser.h"
#include "ToolExecutor.h"
#include "AgentPresentation.h"
#include "QuestionForm.h"
#include "ProviderUsage.h"
#include "harness/HarnessPreparedTurn.h"
#include <QJsonArray>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <optional>

namespace ade {
class UtilityService;
class CodexHarnessSession;
class AcpHarnessSession;
class CursorHarnessSession;
class ClaudeHarnessSession;

class AgentEngine final : public QObject {
    Q_OBJECT
public:
    explicit AgentEngine(QObject *parent = nullptr);
    ~AgentEngine() override;
    void setProvider(const ProviderConfig &provider);
    void setHarnessContext(const QString& ownerId,const QString& fallbackWorkspace);
    void setModelChain(const QList<ModelConfig> &models, const QString &presetId);
    bool applyCompression(int boundaryIndex, const QString &briefing);
    // Automatic compression uses the configured model context window only.
    // Unknown limits retain the manual path. Utility models precede main models.
    void setCompressionModels(const QList<ModelConfig>& models);
    QJsonObject latestUsage() const;
    bool isCompressing() const { return !m_compressor.isNull(); }
    void setWorkspace(const QString &path);
    bool setAppsDirectory(const QString &path);
    void setSystemContext(const QString &context);
    void setExternalHarnessContext(const QString& context);
    void setDynamicSystemContext(std::function<QString()> context);
    void submit(const QString &prompt);
    // True once the user message is accepted into history, even if that turn
    // then fails or is cancelled synchronously by an observer.
    bool submitWithAttachments(const QString &prompt, const QJsonArray &references, const QJsonObject &pathReferences = {}, const QJsonObject &pathMetadata = {});
    // Durable UI send: accepts into the queue while working. A question is
    // interrupted by a normal send; Stop and final failures retain the queue.
    bool sendUserMessage(const QString& text,const QJsonArray& attachments={},const QJsonObject& references={},const QJsonObject& pathMetadata={},bool queueIfBlocked=false);
    // Failed-turn Retry. The record is set only for a turn whose user row was
    // accepted, and cleared by any later accepted, completed or cancelled turn.
    QJsonObject turnFailure() const { return m_turnFailure; }
    static bool validTurnFailureShape(const QJsonObject& record);
    static bool validTurnFailure(const QJsonArray& history,const QJsonObject& record);
    // Idle only; validated against the current history. An empty record clears.
    bool restoreTurnFailure(const QJsonObject& record);
    // Replaces the failed user row and its suffix by a rebuilt copy of that row
    // and dispatches it. History and queue clearing commit atomically.
    bool retryFailedTurn(const QString& userRowId,QString* error=nullptr);
    enum class CommitKind { Update, AcceptUser, ReplaceAndAcceptUser, ReplaceHistory };
    // Meaningful only inside the QueueCommitter callback.
    CommitKind commitKind() const { return m_commitKind; }
    QJsonArray queuedMessages() const {return m_queuedMessages;}
    bool setQueuedMessages(const QJsonArray& messages,QString* error=nullptr);
    bool editQueuedMessage(const QString& id,const QString& text);
    bool removeQueuedMessage(const QString& id);
    bool moveQueuedMessage(const QString& id,int index);
    bool flushQueuedMessage(const QString& id = {});
    bool resumeQueue();
    // Called synchronously before publishing queue changes or dispatching a
    // dequeued message. The host atomically saves both arrays or returns false.
    // This callback must not pump events or mutate the engine.
    using QueueCommitter=std::function<bool(const QJsonArray& history,const QJsonArray& queue,QString* error)>;
    void setQueueCommitter(QueueCommitter committer);
    QJsonObject pendingQuestion() const;
    // Hosts with a question presenter opt in. Validation/headless engines must
    // neither advertise questions nor wait for an unavailable UI.
    void setQuestionToolEnabled(bool enabled) { if (!m_busy) m_questionToolEnabled = enabled; }
    bool submitQuestionStep(const QString& token,int expectedStep,const QJsonObject& answers);
    bool updateQuestionDraft(const QString& token,int expectedStep,const QJsonObject& answers);
    bool goBackQuestion(const QString& token,int expectedStep);
    bool cancelQuestion(const QString& token,const QJsonObject& draft={});
    using MessagePreparer = std::function<QJsonArray(const QJsonArray &, const ProviderConfig &, QString *)>;
    void setMessagePreparer(MessagePreparer preparer);
    // Factory executes on the owner thread; returned job contains only values
    // and runs off-thread. Transient paths/leases never enter saved history.
    using HarnessInputPreparer=std::function<HarnessInputJob(const QJsonArray&,const ProviderConfig&,QString*)>;
    void setHarnessInputPreparer(HarnessInputPreparer preparer);
    void setFileReadCommitter(FileReadCommitter committer);
    void setFileMutationExecutor(FileMutationExecutor executor);
    using MutationRecovery=std::function<QJsonArray(const QStringList&,QString*)>;
    void setMutationContext(const QString& agentId,MutationRecovery recovery);
    bool registerExternalMutation(const QString& invocationId,const QString& requestId);
    void settleExternalMutation(const QString& invocationId,const QJsonObject& metadata);
    void setAttachmentReadSource(FileReadSource source);
    void setShellReadSource(FileReadSource source);
    void cancel();
    void approveTool(const QString &id, bool allow);
    void setAlwaysAllow(bool enabled) { m_alwaysAllow = enabled; }
    void setMessages(const QJsonArray &messages);
    QJsonArray messages() const { return m_messages; }
    QJsonArray displayMessages() const;
    static QJsonArray displayHistory(const QJsonArray& history);
    bool isBusy() const { return m_busy; }
    bool lastTurnSucceeded() const { return m_lastTurnSucceeded; }
    // An approval or question is waiting for the user; the agent is idle then.
    // Harness approvals leave the map when answered, cancelled by the harness
    // (Claude), or when the turn ends (complete/fail/cancel).
    bool isAwaitingUser() const { return !m_waitingApproval.isEmpty() || !m_harnessApprovals.isEmpty() || !m_questionToken.isEmpty(); }
    QString workspace() const { return m_tools.workspace(); }
    ProviderConfig provider() const { return m_provider; }
    QString actualModel() const { return m_actualModel; }
    QJsonObject modelTelemetry() const;
    void setExternalTools(const QJsonArray &definitions);
    void setExternalToolTimeout(const QString& name,int milliseconds);
    void completeExternalTool(const QString &requestId, const QString &output, bool error);
    // UI-only bounded preview, accepted only for the currently running opaque
    // external request token. The final canonical tool result replaces it.
    bool appendExternalToolOutput(const QString &requestId,const QString &text);
    bool replaceExternalToolOutput(const QString &requestId,const QString &text);
    QString externalToolCallId(const QString& requestId) const;
    void completeExternalToolWithAttachments(const QString &requestId, const QString &output, bool error,
                                             const QJsonArray &references, const QJsonObject &pathReferences = {}, const QJsonObject &pathMetadata = {}, const QJsonArray &mutations = {});

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
    void queuedMessagesChanged();
    void queueError(QString error);
    void questionChanged();
    void questionError(QString token,QString error);
    void turnAboutToStart(QJsonObject acceptedUserMessage);
    void usageChanged(QJsonObject usage);
    void compressionStarted();
    void compressionFinished(bool applied, QString error);

private:
    struct ToolCall { QString id, name, arguments; };
    bool startUserMessage(const QString&,const QJsonArray&,const QJsonObject&,const QJsonObject&,const QString& queuedId={},const QJsonObject& immediateMessage={},const std::optional<QJsonArray>& replacementPrefix=std::nullopt);
    bool unsettledMutationAfter(int index) const;
    bool commitQueue(const QJsonArray& history,const QJsonArray& queue,QString* error);
    bool replaceQueue(const QJsonArray& queue);
    void scheduleQueue();
    void beginQuestion(const ToolCall&,const QJsonObject&);
    void finishQuestion(const QString& reason);
    void requestModel();
    void requestHarness();
    void dispatchPreparedHarness(const QJsonArray& input,const QStringList& transcript);
    void requestCursorHarness(const QJsonArray& input,const QStringList& transcript);
    void requestAcpHarness(const QJsonArray& input,const QStringList& transcript);
    void requestClaudeHarness(const QJsonArray& input,const QStringList& transcript);
    void stopHarness();
    void retainHarnessResponse(bool completed = false);
    void consumeHarnessEvent(const QJsonObject& event);
    void finishHarnessQuestion(const QString& reason);
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
    bool registerMutation(const QString& invocationId,const QString& callId,bool external);
    void settleMutation(const QString& invocationId,const QString& output,bool error,const QJsonObject& metadata,bool recovering=false);
    QJsonObject mutationToolMessage(const QString& callId,const QJsonObject& fallback) const;
    void storeToolMessage(const QString& callId,const QJsonObject& message);
    void reconcileMutationHistory();
    void retainPartialResponse();
    void settleOutstandingTools(const QString &reason);
    void complete();
    void fail(const QString &message);
    QString redact(QString value) const;
    void stopNetwork();
    bool maybeCompress(bool finishTurn);
    void stopCompression();
    QJsonObject currentUsageRecord() const;

    QPointer<CodexHarnessSession> m_harness;
    QPointer<AcpHarnessSession> m_acp;
    QPointer<CursorHarnessSession> m_cursor;
    QHash<QString,QString> m_cursorDecisions;
    QPointer<ClaudeHarnessSession> m_claude;
    QString m_harnessOwnerId,m_harnessFallbackWorkspace,m_harnessQuestionToken;
    QJsonObject m_harnessState,m_harnessUsage;
    QMap<QString,QString> m_harnessText,m_harnessReasoning,m_harnessApprovals;
    QMap<QString,int> m_harnessTools;
    QStringList m_harnessOrder;
    ProviderConfig m_provider;
    QList<ModelConfig> m_models;
    QList<ModelConfig> m_compressionModels;
    QPointer<UtilityService> m_compressor;
    QString m_compressionAttempt;
    ProviderUsage m_roundUsage;
    QString m_usageId;
    bool m_usageComplete = false;
    QString m_presetId;
    QString m_systemContext;
    QString m_externalHarnessContext;
    std::function<QString()> m_dynamicSystemContext;
    MessagePreparer m_messagePreparer;
    HarnessInputPreparer m_harnessInputPreparer;
    std::shared_ptr<std::atomic_bool> m_harnessPreparing;
    HarnessAttachments m_harnessAttachments;
    ModelFallback m_fallback;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    ToolExecutor m_tools;
    QTimer m_deadline;
    QTimer m_externalDeadline;
    SseParser m_parser;
    QJsonArray m_messages;
    mutable AgentHistoryProjection m_historyProjection;
    QJsonArray m_queuedMessages;
    QJsonObject m_turnFailure;
    QString m_turnUserRowId;
    CommitKind m_commitKind = CommitKind::Update;
    QueueCommitter m_queueCommitter;
    QString m_mutationAgentId;
    MutationRecovery m_mutationRecovery;
    bool m_mutationHistoryDirty=false;
    bool m_committingQueue=false,m_queuePaused=true;
    bool m_configuringTurn=false;
    quint64 m_queueResumeSerial=0;
    QuestionForm m_question;
    bool m_questionToolEnabled = false;
    QString m_questionToken,m_questionCallId,m_questionStopReason;
    AgentPresentation m_display;
    int m_displayHistoryIndex = -1;
    QMap<int, ToolCall> m_streamCalls;
    QMap<int, QJsonObject> m_nativeBlocks;
    QJsonArray m_googleParts;
    QJsonArray m_externalDefinitions;
    QStringList m_externalNames;
    QMap<QString,int> m_externalTimeouts;
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
