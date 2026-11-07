#include "ClaudeHarnessSession.h"
#include "ClaudeQuestion.h"
#include "QuestionMcpProtocol.h"
#include <QJsonDocument>
#include <QStandardPaths>
#include <QUuid>
#include <cmath>

namespace ade {
namespace {
bool uuid(const QString &value) {
  const QUuid parsed(value);
  return !parsed.isNull() &&
         parsed.toString(QUuid::WithoutBraces) == value.toLower();
}
QJsonObject usage(const QJsonObject &value) {
  QJsonObject output;
  for (const auto *key :
       {"input_tokens", "output_tokens", "cache_read_input_tokens",
        "cache_creation_input_tokens"}) {
    const auto v = value.value(key);
    const auto n = v.toDouble(-1);
    if (v.isDouble() && std::isfinite(n) && n >= 0 && n <= 1e12 &&
        std::floor(n) == n)
      output[key] = v;
  }
  return output;
}
} // namespace
ClaudeHarnessSession::ClaudeHarnessSession(QObject *parent)
    : QObject(parent), m_questionMcp(this), m_process(this) {
  connect(&m_questionMcp, &HarnessQuestionMcp::pendingChanged, this,
          [this](bool pending) {
            if (pending) {
              m_resumeActivity = true;
              pauseForDecision();
            } else
              resumeAfterDecision();
          });
  connect(&m_questionMcp, &HarnessQuestionMcp::event, this,
          [this](const QJsonObject &value) {
            if (m_busy)
              emit event(value);
          });
  connect(&m_questionMcp, &HarnessQuestionMcp::required, this,
          &ClaudeHarnessSession::stagewiseQuestionRequired);
  connect(&m_questionMcp, &HarnessQuestionMcp::cancelled, this,
          &ClaudeHarnessSession::stagewiseQuestionCancelled);
  m_deadline.setSingleShot(true);
  m_cancelDeadline.setSingleShot(true);
  connect(&m_deadline, &QTimer::timeout, this,
          [this] { finish("failed", "Claude turn timed out."); });
  connect(&m_cancelDeadline, &QTimer::timeout, this,
          [this] { finish("interrupted"); });
  connect(&m_process, &ClaudeControlProcess::started, this, [this] {
    if (!m_busy)
      return;
    m_initializeId =
        m_process.control("initialize", {{"hooks", QJsonValue::Null}}, 60000);
    if (m_initializeId.isEmpty())
      finish("failed", "Could not initialize Claude.");
  });
  connect(&m_process, &ClaudeControlProcess::controlResponse, this,
          [this](const QString &id, const QJsonObject &, const QString &error) {
            if (!m_busy || id != m_initializeId)
              return;
            m_initializeId.clear();
            if (!error.isEmpty()) {
              finish("failed", error);
              return;
            }
            m_initialized = true;
            sendPrompt();
          });
  connect(&m_process, &ClaudeControlProcess::controlRequest, this,
          &ClaudeHarnessSession::request);
  connect(&m_process, &ClaudeControlProcess::controlCanceled, this,
          [this](const QString &id) {
            QString token;
            for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
              if (it->requestId == id) {
                token = it.key();
                m_pending.erase(it);
                break;
              }
            if (!token.isEmpty()) {
              resumeAfterDecision();
              emit decisionCanceled(token);
            }
          });
  connect(&m_process, &ClaudeControlProcess::frame, this,
          &ClaudeHarnessSession::consume);
  connect(&m_process, &ClaudeControlProcess::failed, this,
          [this](const QString &error) {
            m_initialized = false;
            if (m_busy)
              finish("failed", error);
          });
}
ClaudeHarnessSession::~ClaudeHarnessSession() { shutdown(); }
bool ClaudeHarnessSession::startTurn(const ClaudeHarnessOptions &options,
                                     const QJsonArray &input,
                                     const QJsonObject &saved) {
  if (m_busy)
    return false;
  const auto executable = QStandardPaths::findExecutable(
      options.executable.isEmpty() ? QStringLiteral("claude")
                                   : options.executable);
  const auto workspace = HarnessWorkspace::resolve(options.workspace);
  if (executable.isEmpty() || workspace.path.isEmpty() ||
      options.providerId.isEmpty() || input.isEmpty() ||
      options.turnTimeoutMs < 100 || options.turnTimeoutMs > 600000 ||
      QJsonDocument(input).toJson(QJsonDocument::Compact).size() >
          4 * 1024 * 1024) {
    emit failed("Choose an installed Claude executable and an owned workspace "
                "with bounded input.");
    return false;
  }
  if (!options.effort.isEmpty() &&
      !QStringList{"low", "medium", "high", "xhigh", "max"}.contains(
          options.effort)) {
    emit failed("Claude effort is not supported by this adapter.");
    return false;
  }
  if (!QStringList{"", "enabled", "disabled", "adaptive"}.contains(
          options.thinkingMode) ||
      options.thinkingBudget < 0 || options.thinkingBudget > 200000) {
    emit failed("Claude thinking settings are outside the supported range.");
    return false;
  }
  const bool sameIdentity =
      m_options.executable == executable &&
      m_options.providerId == options.providerId &&
      m_workspace.identity == workspace.identity &&
      m_options.alwaysAllow == options.alwaysAllow &&
      m_options.model == options.model && m_options.effort == options.effort &&
      m_options.thinkingMode == options.thinkingMode &&
      m_options.thinkingBudget == options.thinkingBudget &&
      m_options.questionMcpExecutable == options.questionMcpExecutable;
  const bool reuse = sameIdentity && m_initialized && m_process.isRunning() &&
                     !saved.value("fork").toBool();
  ++m_generation;
  m_busy = true;
  m_interrupted = false;
  m_promptSent = false;
  m_resumeId.clear();
  m_resumeRecoveryUsed = m_resumeActivity = m_recoveryQueued = false;
  m_usage = {};
  m_eventCount = 0;
  m_waitRemaining = -1;
  m_pending.clear();
  m_questionMcp.setBlocked(false);
  m_input = input;
  m_options = options;
  m_options.executable = executable;
  m_workspace = workspace;
  m_turnId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  m_stream.reset(m_turnId);
  m_created = false;
  m_deadline.start(options.turnTimeoutMs);
  if (reuse) {
    sendPrompt();
    return true;
  }
  m_process.stop();
  m_initialized = false;
  m_tasks.clear();
  const bool resume =
      saved.value("kind") == "claude-code" &&
      saved.value("providerId") == options.providerId &&
      saved.value("executable") == executable &&
      saved.value("workspaceIdentity") == workspace.identity &&
      saved.value("alwaysAllow").toBool() == options.alwaysAllow &&
      uuid(saved.value("sessionId").toString());
  m_fork = resume && saved.value("fork").toBool();
  m_created = !resume;
  m_sessionId = resume && !m_fork
                    ? saved.value("sessionId").toString()
                    : QUuid::createUuid().toString(QUuid::WithoutBraces);
  m_resumeId = resume ? saved.value("sessionId").toString() : QString{};
  return launchProcess(m_resumeId, m_fork);
}
bool ClaudeHarnessSession::launchProcess(const QString &resumeId, bool fork) {
  const auto &options = m_options;
  QJsonObject mcpConfig;
  if (!options.questionMcpExecutable.isEmpty()) {
    QString error;
    const auto config =
        m_questionMcp.configure(options.questionMcpExecutable, &error);
    if (config.isEmpty()) {
      finish("failed", error);
      return false;
    }
    mcpConfig = QJsonObject{{"mcpServers", QJsonObject{{"stagewise", config}}}};
  } else
    m_questionMcp.revoke();
  QStringList args{"--print",
                   "--input-format",
                   "stream-json",
                   "--output-format",
                   "stream-json",
                   "--verbose",
                   "--include-partial-messages",
                   "--permission-prompt-tool",
                   "stdio",
                   "--permission-mode",
                   "default"};
  if (!resumeId.isEmpty()) {
    args << "--resume=" + resumeId;
    if (fork)
      args << "--fork-session";
  } else
    args << "--session-id=" + m_sessionId;
  if (!mcpConfig.isEmpty())
    args << "--mcp-config"
         << QString::fromUtf8(
                QJsonDocument(mcpConfig).toJson(QJsonDocument::Compact));
  if (!options.model.isEmpty() && options.model != "default")
    args << "--model" << options.model;
  if (!options.effort.isEmpty())
    args << "--effort" << options.effort;
  if (options.thinkingMode == "disabled")
    args << "--thinking" << "disabled";
  else if (options.thinkingMode == "adaptive" ||
           (options.thinkingMode == "enabled" && options.thinkingBudget == 0))
    args << "--thinking" << "adaptive";
  else if (options.thinkingBudget > 0)
    args << "--max-thinking-tokens" << QString::number(options.thinkingBudget);
  if (!options.alwaysAllow)
    args << "--settings"
         << QString::fromUtf8(
                QJsonDocument(
                    QJsonObject{{"permissions",
                                 QJsonObject{{"ask", QJsonArray{"Bash(*)"}},
                                             {"defaultMode", "default"}}}})
                    .toJson(QJsonDocument::Compact));
  auto environment = ClaudeControlProcess::managedEnvironment();
  environment.insert("CLAUDE_CODE_SDK_READS_SESSION_STATE", "1");
  const auto generation = m_generation;
  const QPointer<ClaudeHarnessSession> self(this);
  const bool started =
      m_process.start(options.executable, args, m_workspace.path, environment);
  if (self && generation == m_generation && !started && m_busy)
    finish("failed", "Could not start Claude.");
  return started;
}
bool ClaudeHarnessSession::recoverMissingSession(const QJsonObject &frame) {
  // Only a typed, exact diagnostic for this attempted resume is replayable.
  // Generic exits, prose substrings and account/network failures are not proof
  // that a prompt or tool had no side effects.
  const auto errors = frame.value("errors");
  const auto parent = frame.value("parent_tool_use_id");
  const auto origin = frame.value("origin");
  const auto result = frame.value("result");
  if (!m_busy || m_interrupted || m_resumeId.isEmpty() ||
      m_resumeRecoveryUsed || m_resumeActivity || !m_pending.isEmpty() ||
      !m_tasks.isEmpty() || frame.value("type") != "result" ||
      frame.value("subtype") != "error_during_execution" ||
      frame.value("is_error") != true || !errors.isArray() ||
      errors.toArray().size() != 1 ||
      errors.toArray().first() !=
          "No conversation found with session ID: " + m_resumeId ||
      (!parent.isUndefined() && !parent.isNull()) ||
      (!origin.isUndefined() &&
       (!origin.isObject() || origin.toObject().value("kind") != "human")) ||
      (!result.isUndefined() && result != ""))
    return false;
  m_resumeRecoveryUsed = true;
  m_recoveryQueued = true;
  m_initialized = false;
  m_promptSent = false;
  m_initializeId.clear();
  m_process.stop();
  const auto generation = m_generation;
  QTimer::singleShot(0, this, [this, generation] {
    if (generation != m_generation || !m_busy || m_interrupted ||
        !m_recoveryQueued)
      return;
    m_recoveryQueued = false;
    const auto current = HarnessWorkspace::resolve(m_workspace.path);
    if (current.identity.isEmpty() ||
        current.identity != m_workspace.identity) {
      finish("failed", "The Claude workspace changed before session recovery.");
      return;
    }
    m_resumeId.clear();
    m_created = true;
    m_fork = false;
    m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // Keep the original turn ID and remaining deadline. sendPrompt adds the
    // caller's canonical history exactly once for this new session.
    launchProcess();
  });
  return true;
}
void ClaudeHarnessSession::sendPrompt() {
  if (!m_busy || m_interrupted || !m_initialized)
    return;
  QString attachmentError;
  const auto prepared=mapHarnessAttachments(m_input,m_options.attachments,false,true,m_created,&attachmentError);
  if(!attachmentError.isEmpty()){finish("failed",attachmentError);return;}
  QJsonArray content;
  if (!m_options.questionMcpExecutable.isEmpty()) {
    m_questionMcp.beginTurn(m_turnId);
    content.append(QJsonObject{{"type", "text"},
                               {"text", HarnessQuestionMcp::preference()}});
  }
  // Per-turn context avoids relying on a stale resumed system-prompt snapshot.
  if (!m_options.instructions.isEmpty())
    content.append(QJsonObject{
        {"type", "text"},
        {"text",
         "<ade-current-context>\nCurrent task context; labels are not "
         "filesystem grants. Use only actual runtime tools and permissions.\n" +
             m_options.instructions.left(1024 * 1024) +
             "\n</ade-current-context>"}});
  if (m_created && !m_options.historyContext.isEmpty())
    content.append(
        QJsonObject{{"type", "text"},
                    {"text", m_options.historyContext.left(1024 * 1024)}});
  for (const auto &part : prepared)
    content.append(part);
  m_promptSent = true;
  if (!m_process.sendUser(content, m_fork ? QString{} : m_sessionId))
    finish("failed", "Could not send the bounded Claude prompt.");
}
bool ClaudeHarnessSession::publish(const QJsonObject &frame) {
  QJsonArray updates;
  QString error;
  if (!m_stream.consume(frame, &updates, &error)) {
    finish("failed", error);
    return false;
  }
  return publishUpdates(updates);
}
bool ClaudeHarnessSession::publishUpdates(const QJsonArray &updates) {
  const auto generation = m_generation;
  const QPointer<ClaudeHarnessSession> self(this);
  for (const auto &value : updates) {
    if (++m_eventCount > 200000) {
      finish("failed", "Claude turn output exceeded its event limit.");
      return false;
    }
    auto display = value.toObject();
    if (display.value("type") == "tool") {
      const bool ownedWrapper =
          !m_options.questionMcpExecutable.isEmpty() &&
          display.value("name") ==
              "claude.mcp__stagewise__stagewise_request_user_input";
      display["acpPresentation"] =
          ownedWrapper ? HarnessQuestionMcp::wrapperPresentation(
                             display.value("name").toString())
                       : QJsonObject{};
    }
    emit event(display);
    if (!self || generation != m_generation || !m_busy || m_interrupted)
      return false;
  }
  return true;
}
void ClaudeHarnessSession::consume(const QJsonObject &frame) {
  if (m_recoveryQueued)
    return;
  const auto session = frame.value("session_id").toString();
  if (!session.isEmpty()) {
    if (m_fork && uuid(session)) {
      m_sessionId = session;
      m_fork = false;
    } else if (session != m_sessionId)
      return;
  }
  const auto type = frame.value("type").toString(),
             subtype = frame.value("subtype").toString();
  if (recoverMissingSession(frame))
    return;
  if (m_busy && (type != "system" || subtype != "init"))
    m_resumeActivity = true;
  if (type == "system") {
    const auto taskId = frame.value("task_id").toString();
    if (!taskId.isEmpty() && taskId.size() <= 512) {
      if (subtype == "task_started" && m_tasks.size() < 128)
        m_tasks.insert(taskId);
      else if (subtype == "task_notification")
        m_tasks.remove(taskId);
      else if (subtype == "task_updated" &&
               QStringList{"completed", "failed", "stopped", "killed"}.contains(
                   frame.value("patch").toObject().value("status").toString()))
        m_tasks.remove(taskId);
    }
  }
  if (!m_busy || !m_promptSent)
    return;
  if (type == "assistant" && frame.value("error").isString()) {
    const auto code = frame.value("error").toString();
    if (code == "authentication_failed") {
      finish("failed", "Claude authentication failed. Sign in using the "
                       "installed Claude Code CLI.");
      return;
    }
    if (code == "billing_error") {
      finish("failed",
             "Claude reported an account billing or subscription error.");
      return;
    }
    if (code == "rate_limit") {
      finish("failed", "Claude reported a usage limit. Try again when the "
                       "account limit permits.");
      return;
    }
  }
  if (type == "system" && subtype == "init") {
    const auto generation = m_generation;
    const QPointer<ClaudeHarnessSession> self(this);
    emit sessionStateChanged(sessionState());
    if (!self || generation != m_generation || !m_busy || m_interrupted)
      return;
  }
  if (type == "result") {
    const auto origin =
        frame.value("origin").toObject().value("kind").toString();
    if (!origin.isEmpty() && origin != "human")
      return;
    if (!m_interrupted && !publish(frame))
      return;
    m_usage = usage(frame.value("usage").toObject());
    const auto generation = m_generation;
    const QPointer<ClaudeHarnessSession> self(this);
    emit sessionStateChanged(sessionState());
    if (!self || generation != m_generation || !m_busy)
      return;
    if (m_interrupted)
      finish("interrupted");
    else if (frame.value("is_error").toBool() || subtype != "success")
      finish("failed", "Claude could not complete this turn.");
    else
      finish("completed");
    return;
  }
  if (!m_interrupted)
    publish(frame);
}
void ClaudeHarnessSession::request(const QString &id, const QString &subtype,
                                   const QJsonObject &request) {
  if (m_busy)
    m_resumeActivity = true;
  if (!m_busy || !m_promptSent || m_interrupted || m_recoveryQueued) {
    m_process.reject(id, "No active user turn owns this request.");
    return;
  }
  if (subtype != "can_use_tool" || !request.value("input").isObject()) {
    m_process.reject(
        id, "This native client does not support that control request.");
    return;
  }
  const auto name = request.value("tool_name").toString(),
             toolId = request.value("tool_use_id").toString();
  if (name.isEmpty() || name.size() > 256 || toolId.isEmpty() ||
      toolId.size() > 512 || m_pending.size() >= 50 ||
      QJsonDocument(request).toJson(QJsonDocument::Compact).size() >
          256 * 1024) {
    m_process.reject(id, "Invalid or oversized permission request.");
    return;
  }
  for (const auto &pending : m_pending)
    if (pending.toolId == toolId) {
      m_process.reject(id, "A decision is already pending for this tool.");
      return;
    }
  const auto input = request.value("input").toObject();
  if (name == "AskUserQuestion") {
    const auto form = claudeQuestionForm(input);
    bool otherQuestion = m_questionMcp.pending();
    for (const auto &pending : m_pending)
      otherQuestion |= pending.question;
    if (!m_options.questionsEnabled || form.isEmpty() || otherQuestion) {
      m_process.respond(id,
                        {{"behavior", "deny"},
                         {"message", "Native questions are disabled, invalid, "
                                     "or already awaiting an answer."}});
      return;
    }
    const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_pending.insert(token, {id, toolId, input, true});
    m_questionMcp.setBlocked(true);
    pauseForDecision();
    emit questionRequired(token, request, form);
    return;
  }
  if (m_options.alwaysAllow) {
    m_process.respond(id, {{"behavior", "allow"}, {"updatedInput", input}});
    return;
  }
  const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
  m_pending.insert(token, {id, toolId, input});
  pauseForDecision();
  emit approvalRequired(token, request);
}
bool ClaudeHarnessSession::answerApproval(const QString &token, bool allow) {
  const auto it = m_pending.constFind(token);
  if (!m_busy || m_interrupted || it == m_pending.cend() || it->question)
    return false;
  const auto pending = *it;
  m_pending.remove(token);
  resumeAfterDecision();
  return m_process.respond(
      pending.requestId,
      allow
          ? QJsonObject{{"behavior", "allow"}, {"updatedInput", pending.input}}
          : QJsonObject{{"behavior", "deny"},
                        {"message", "The user denied this tool request."}});
}
bool ClaudeHarnessSession::validQuestionAnswer(
    const QString &token, const QJsonObject &answers) const {
  if (m_questionMcp.owns(token))
    return m_busy && !m_interrupted;
  const auto it = m_pending.constFind(token);
  return m_busy && !m_interrupted && it != m_pending.cend() && it->question &&
         claudeQuestionResponse(it->input, answers, nullptr);
}
bool ClaudeHarnessSession::answerQuestion(const QString &token,
                                          const QJsonObject &answers) {
  // Shared MCP answers require the full completed/cancelled/steps envelope.
  // validQuestionAnswer also serves Engine preflight, but this API is
  // native-only.
  if (m_questionMcp.owns(token) || !validQuestionAnswer(token, answers))
    return false;
  const auto pending = m_pending.value(token);
  QJsonObject updated;
  claudeQuestionResponse(pending.input, answers, &updated);
  m_pending.remove(token);
  resumeAfterDecision();
  return m_process.respond(pending.requestId,
                           {{"behavior", "allow"}, {"updatedInput", updated}});
}
bool ClaudeHarnessSession::cancelQuestion(const QString &token) {
  if (m_questionMcp.owns(token))
    return m_questionMcp.complete(token,
                                  QuestionMcpProtocol::cancelledResult());
  const auto it = m_pending.constFind(token);
  if (!m_busy || it == m_pending.cend() || !it->question)
    return false;
  const auto pending = *it;
  m_pending.remove(token);
  resumeAfterDecision();
  return m_process.respond(
      pending.requestId,
      {{"behavior", "deny"}, {"message", "The user canceled this question."}});
}
QJsonObject ClaudeHarnessSession::sessionState() const {
  if (!m_initialized || m_sessionId.isEmpty())
    return {};
  return {{"version", 1},
          {"kind", "claude-code"},
          {"providerId", m_options.providerId},
          {"executable", m_options.executable},
          {"workspace", m_workspace.path},
          {"workspaceIdentity", m_workspace.identity},
          {"alwaysAllow", m_options.alwaysAllow},
          {"sessionId", m_sessionId}};
}
void ClaudeHarnessSession::pauseForDecision() {
  if (m_waitRemaining < 0) {
    m_waitRemaining = qMax(1, m_deadline.remainingTime());
    m_deadline.stop();
  }
}
void ClaudeHarnessSession::resumeAfterDecision() {
  bool blocked = false;
  for (const auto &request : m_pending)
    blocked |= request.question;
  m_questionMcp.setBlocked(blocked);
  if (m_busy && !m_interrupted && m_pending.isEmpty() &&
      !m_questionMcp.pending() && m_waitRemaining >= 0) {
    const auto remaining = m_waitRemaining;
    m_waitRemaining = -1;
    m_deadline.start(remaining);
  }
}
void ClaudeHarnessSession::interrupt() {
  if (!m_busy)
    return;
  m_interrupted = true;
  const auto generation = m_generation;
  const QPointer<ClaudeHarnessSession> self(this);
  m_questionMcp.endTurn();
  if (!self || generation != m_generation || !m_busy)
    return;
  const auto pending = m_pending;
  m_pending.clear();
  for (const auto &item : pending)
    m_process.respond(item.requestId,
                      {{"behavior", "deny"},
                       {"message", "The user stopped this turn."},
                       {"interrupt", true}});
  if (!m_promptSent) {
    finish("interrupted");
    return;
  }
  m_process.control("interrupt", {}, 2000);
  m_cancelDeadline.start(2000);
}
void ClaudeHarnessSession::finish(const QString &status, const QString &error) {
  if (!m_busy)
    return;
  const auto originalGeneration = m_generation;
  const QPointer<ClaudeHarnessSession> originalSelf(this);
  m_questionMcp.endTurn();
  if (!originalSelf || originalGeneration != m_generation || !m_busy)
    return;
  // Normalize terminal state before observers see completion. Emit snapshots
  // only here, keeping growing streamed tool inputs out of retained history.
  const auto closing = m_stream.finish(status);
  const auto generation = m_generation;
  const QPointer<ClaudeHarnessSession> self(this);
  for (const auto &value : closing) {
    auto display = value.toObject();
    if (display.value("type") == "tool") {
      const bool ownedWrapper =
          !m_options.questionMcpExecutable.isEmpty() &&
          display.value("name") ==
              "claude.mcp__stagewise__stagewise_request_user_input";
      display["acpPresentation"] =
          ownedWrapper ? HarnessQuestionMcp::wrapperPresentation(
                             display.value("name").toString())
                       : QJsonObject{};
    }
    emit event(display);
    if (!self || generation != m_generation || !m_busy)
      return;
  }
  m_busy = false;
  m_deadline.stop();
  m_cancelDeadline.stop();
  const auto pending = m_pending;
  m_pending.clear();
  for (const auto &item : pending)
    m_process.respond(item.requestId, {{"behavior", "deny"},
                                       {"message", "The owning turn ended."}});
  const QString finalStatus = m_interrupted ? "interrupted" : status;
  const QJsonObject result{
      {"status", finalStatus},          {"error", error},
      {"events", m_stream.snapshot()},  {"usage", m_usage},
      {"sessionState", sessionState()}, {"turnId", m_turnId}};
  if (finalStatus != "completed") {
    m_process.stop();
    m_initialized = false;
    m_tasks.clear();
  }
  if (!error.isEmpty()) {
    emit failed(error);
    if (!self || generation != m_generation)
      return;
  }
  emit finished(result);
}
void ClaudeHarnessSession::shutdown() {
  ++m_generation;
  m_busy = false;
  m_initialized = false;
  m_promptSent = false;
  m_recoveryQueued = false;
  m_resumeId.clear();
  m_deadline.stop();
  m_cancelDeadline.stop();
  m_pending.clear();
  m_tasks.clear();
  m_questionMcp.revoke();
  m_process.stop();
  m_options.attachments={};
}
} // namespace ade
