#include "CodexHarnessSession.h"
#include "QuestionMcpProtocol.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>

namespace ade {
namespace {
constexpr qsizetype MaximumOutput = 4 * 1024 * 1024;
bool samePath(const QString &a, const QString &b) {
#ifdef Q_OS_WIN
  return a.compare(b, Qt::CaseInsensitive) == 0;
#else
  return a == b;
#endif
}
QString safeId(const QJsonValue &value) {
  const auto id = value.toString();
  return !id.isEmpty() && id.size() <= 512 && !id.contains(QChar::Null)
             ? id
             : QString();
}
} // namespace
CodexHarnessSession::CodexHarnessSession(QObject *parent)
    : QObject(parent), m_questionMcp(this), m_server(this) {
  connect(&m_questionMcp, &HarnessQuestionMcp::pendingChanged, this,
          [this](bool pending) {
            if (pending)
              pauseForDecision();
            else
              resumeAfterDecision();
          });
  connect(&m_questionMcp, &HarnessQuestionMcp::event, this,
          [this](const QJsonObject &value) {
            if (m_busy)
              emit event(value);
          });
  connect(&m_questionMcp, &HarnessQuestionMcp::required, this,
          &CodexHarnessSession::stagewiseQuestionRequired);
  connect(&m_questionMcp, &HarnessQuestionMcp::cancelled, this,
          &CodexHarnessSession::stagewiseQuestionCancelled);
  m_timeout.setSingleShot(true);
  connect(&m_timeout, &QTimer::timeout, this, [this] {
    if (!m_busy)
      return;
    m_server.stop();
    finish(m_interrupted ? "interrupted" : "failed",
           m_interrupted ? QString() : "Codex stopped responding.");
  });
  connect(&m_server, &CodexAppServer::ready, this, [this] {
    if (m_busy)
      m_server.readAccount();
  });
  connect(&m_server, &CodexAppServer::accountChanged, this,
          [this](bool connected, const QString &) {
            if (!m_busy || m_turnSent || !m_requests.isEmpty())
              return;
            if (!connected) {
              finish("failed", "Connect a ChatGPT subscription in Models & "
                               "Providers before using Codex.");
              return;
            }
            beginThread();
          });
  connect(&m_server, &CodexAppServer::reply, this,
          &CodexHarnessSession::receiveReply);
  connect(&m_server, &CodexAppServer::notification, this,
          &CodexHarnessSession::receiveNotification);
  connect(&m_server, &CodexAppServer::serverRequest, this,
          &CodexHarnessSession::receiveRequest);
  connect(&m_server, &CodexAppServer::failed, this,
          [this](const QString &error) {
            if (m_busy)
              finish(m_interrupted ? "interrupted" : "failed",
                     m_interrupted ? QString() : error);
          });
}
CodexHarnessSession::~CodexHarnessSession() { shutdown(); }
bool CodexHarnessSession::startTurn(const CodexHarnessOptions &options,
                                    const QJsonArray &input,
                                    const QJsonObject &savedState) {
  if (m_busy)
    return false;
  const auto executable = CodexAppServer::findExecutable(options.executable);
  const auto binding = HarnessWorkspace::resolve(options.workspace);
  const auto workspace = binding.path;
  if (executable.isEmpty() || workspace.isEmpty() ||
      !QFileInfo(workspace).isDir() || options.providerId.isEmpty() ||
      options.model.isEmpty() || input.isEmpty() ||
      options.turnTimeoutMs < 100 || options.turnTimeoutMs > 600000 ||
      QJsonDocument(input).toJson(QJsonDocument::Compact).size() >
          4 * 1024 * 1024) {
    emit failed("Codex requires an installed executable, a workspace, a "
                "selected model, and bounded input.");
    return false;
  }
  // Only host-produced text and inline images enter this seam. Arbitrary
  // model/user supplied absolute filesystem paths are never promoted to a
  // localImage/mention/skill authority by the harness adapter.
  for (const auto &value : input) {
    const auto object = value.toObject();
    const auto type = object.value("type").toString();
    if ((type != "text" && type != "image") ||
        (type == "text" && !object.value("text").isString()) ||
        (type == "image" &&
         !object.value("url").toString().startsWith("data:image/"))) {
      emit failed("Codex input contains an unsupported attachment type.");
      return false;
    }
  }
  const bool reuse = m_server.isReady() &&
                     samePath(m_options.executable, executable) &&
                     samePath(m_options.workspace, workspace) &&
                     m_workspaceBinding.identity == binding.identity &&
                     m_options.providerId == options.providerId;
  ++m_generation;
  m_previouslyConfiguredQuestions =
      !m_options.questionMcpExecutable.isEmpty() ||
      savedState.value("questionMcp").toBool();
  m_options = options;
  m_workspaceBinding=binding;
  m_options.executable = executable;
  m_options.workspace = workspace;
  m_input = input;
  m_saved = savedState;
  m_items.clear();
  m_order = {};
  m_text.clear();
  m_reasoning.clear();
  m_requests.clear();
  m_pending.clear();
  m_questionMcp.setBlocked(false);
  m_usage = {};
  m_outputChars = 0;
  m_itemBytes = 0;
  m_earlyEvents = {};
  m_earlyBytes = 0;
  m_busy = true;
  m_interrupted = false;
  m_turnSent = false;
  m_turnId.clear();
  if (!reuse)
    m_threadId.clear();
  // A restored mapping is accepted only for this actual configured owner and
  // physical workspace. Unknown/foreign mappings are not resumed.
  if (!savedState.isEmpty() &&
      (savedState.value("version").toInt() != 1 ||
       savedState.value("kind").toString() != "codex" ||
       savedState.value("providerId").toString() != options.providerId ||
       !samePath(savedState.value("executable").toString(), executable) ||
       !samePath(savedState.value("workspace").toString(), workspace) ||
       savedState.value("workspaceIdentity").toString()!=binding.identity)) {
    m_saved = {};
    m_threadId.clear();
  }
  m_waitRemaining = -1;
  m_timeout.start(options.turnTimeoutMs);
  if (reuse) {
    m_server.readAccount();
    return true;
  }
  QPointer<CodexHarnessSession> self(this);
  const auto generation = m_generation;
  const bool started = m_server.start(executable, workspace);
  if (!self)
    return true;
  if (!started && generation == m_generation && m_busy)
    finish("failed", "Codex could not start.");
  return true;
}
QJsonObject CodexHarnessSession::sessionState() const {
  if (m_threadId.isEmpty())
    return {};
  return {{"version", 1},
          {"kind", "codex"},
          {"providerId", m_options.providerId},
          {"executable", m_options.executable},
          {"workspace", m_options.workspace},
          {"workspaceIdentity", m_workspaceBinding.identity},
          {"threadId", m_threadId},
          {"questionMcp", !m_options.questionMcpExecutable.isEmpty()}};
}
QJsonObject CodexHarnessSession::forkState(QJsonObject source) {
  if (source.value("version").toInt() != 1 ||
      source.value("kind").toString() != "codex")
    return {};
  source["fork"] = true;
  return source;
}
void CodexHarnessSession::beginThread() {
  if (!m_busy || m_interrupted)
    return;
  QJsonObject params{
      {"model", m_options.model},
      {"cwd", m_options.workspace},
      {"approvalPolicy", m_options.alwaysAllow ? "never" : "on-request"},
      {"sandbox", m_options.alwaysAllow ? "workspace-write" : "read-only"}};
  if (!m_options.instructions.isEmpty())
    params["developerInstructions"] = m_options.instructions;
  if (!m_options.questionMcpExecutable.isEmpty()) {
    QString error;
    auto config =
        m_questionMcp.configure(m_options.questionMcpExecutable, &error);
    if (config.isEmpty()) {
      finish("failed", error);
      return;
    }
    config["required"] = true;
    config["tool_timeout_sec"] = 43200;
    params["config"] = QJsonObject{{"mcp_servers.stagewise", config}};
  } else {
    m_questionMcp.revoke();
    if (m_previouslyConfiguredQuestions)
      params["config"] =
          QJsonObject{{"mcp_servers.stagewise",
                       QJsonObject{{"command", m_options.executable},
                                   {"args", QJsonArray{"--version"}},
                                   {"enabled", false}}}};
  }
  QString method = "thread/start";
  const auto savedId = safeId(m_saved.value("threadId"));
  if (!savedId.isEmpty()) {
    method = m_saved.value("fork").toBool() ? "thread/fork" : "thread/resume";
    params["threadId"] = savedId;
    params["excludeTurns"] = true;
  } else if (!m_threadId.isEmpty()) {
    method = "thread/resume";
    params["threadId"] = m_threadId;
    params["excludeTurns"] = true;
  }
  const auto id = m_server.call(method, params);
  if (id.isEmpty()) {
    finish("failed", "Codex could not create or resume the conversation.");
    return;
  }
  m_sessionCreated = method == "thread/start";
  m_requests.insert(id, method);
}
void CodexHarnessSession::sendTurn() {
  if (!m_busy || m_interrupted)
    return;
  QString attachmentError;
  auto input = mapHarnessAttachments(m_input,m_options.attachments,false,true,m_sessionCreated,&attachmentError);
  if(!attachmentError.isEmpty()){finish("failed",attachmentError);return;}
  if (!m_options.questionMcpExecutable.isEmpty()) {
    m_questionMcp.beginTurn(QUuid::createUuid().toString(QUuid::WithoutBraces));
    // Some installed Codex model interfaces defer MCP declarations into the
    // exec registry. Keep the configured question preference and describe the
    // discovery path conditionally; never change the selected model or claim
    // that every CLI/model supplies this interface.
    const auto preference = HarnessQuestionMcp::preference() +
        "\n<stagewise-tool-discovery>\nIf your runtime exposes `functions.exec` and `ALL_TOOLS`, "
        "MCP tools may be deferred rather than listed directly. Before concluding the "
        "question tool is unavailable, find the exact registered name "
        "`mcp__stagewise__stagewise_request_user_input` in `ALL_TOOLS` and invoke it "
        "through `tools[name]` inside `functions.exec`. Use the registered tool schema. "
        "Do not invent a tool, call a similarly named server, or substitute a "
        "different model. If this interface is absent, use your runtime's normal "
        "configured MCP tool interface.\n</stagewise-tool-discovery>";
    input.prepend(QJsonObject{{"type", "text"}, {"text", preference}});
  }
  if (m_sessionCreated && !m_options.historyContext.isEmpty())
    input.prepend(
        QJsonObject{{"type", "text"}, {"text", m_options.historyContext}});
  if (QJsonDocument(input).toJson(QJsonDocument::Compact).size() >
      MaximumOutput) {
    finish("failed", "Codex conversation input exceeded its limit.");
    return;
  }
  QJsonObject params{{"threadId", m_threadId},
                     {"input", input},
                     {"model", m_options.model},
                     {"summary", "auto"}};
  if (!m_options.reasoningEffort.isEmpty())
    params["effort"] = m_options.reasoningEffort;
  m_turnSent = true;
  const auto id = m_server.call("turn/start", params);
  if (id.isEmpty()) {
    finish("failed", "Codex could not start the turn.");
    return;
  }
  m_requests.insert(id, "turn/start");
}
void CodexHarnessSession::receiveReply(const QString &id, const QString &method,
                                       const QJsonValue &result,
                                       const QJsonObject &error) {
  if (!m_busy || !m_requests.remove(id))
    return;
  if (!error.isEmpty()) {
    if (method == "thread/resume" && !m_interrupted && !m_turnSent) {
      // Pinned ACP runtime resumes optimistically and creates a fresh session
      // when that mapping is no longer available. Clearing both candidates
      // makes the next request thread/start: its failure is terminal, so this
      // cannot loop. sendTurn adds canonical history only for that new thread.
      m_saved = {};
      m_threadId.clear();
      beginThread();
      return;
    }
    finish(m_interrupted ? "interrupted" : "failed",
           m_interrupted ? QString()
                         : "Codex rejected the conversation request. Existing "
                           "ADE history was preserved.");
    return;
  }
  const auto object = result.toObject();
  if (method.startsWith("thread/")) {
    const auto thread = safeId(object.value("thread").toObject().value("id"));
    if (thread.isEmpty()) {
      finish("failed", "Codex returned an invalid conversation identifier.");
      return;
    }
    m_threadId = thread;
    QPointer<CodexHarnessSession> self(this);
    const auto generation = m_generation;
    emit sessionStateChanged(sessionState());
    if (self && generation == m_generation && m_busy)
      sendTurn();
  } else if (method == "turn/start") {
    const auto turn = safeId(object.value("turn").toObject().value("id"));
    if (turn.isEmpty() || (!m_turnId.isEmpty() && turn != m_turnId)) {
      finish("failed", "Codex returned an invalid turn identifier.");
      return;
    }
    m_turnId = turn;
    if (m_interrupted) {
      interrupt();
      return;
    }
    const auto events = m_earlyEvents;
    m_earlyEvents = {};
    m_earlyBytes = 0;
    const auto generation = m_generation;
    QPointer<CodexHarnessSession> self(this);
    for (const auto &value : events) {
      const auto event = value.toObject();
      if (event.contains("id"))
        receiveRequest(event.value("id"), event.value("method").toString(),
                       event.value("params").toObject());
      else
        receiveNotification(event.value("method").toString(),
                            event.value("params").toObject());
      if (!self || generation != m_generation || !m_busy)
        return;
    }
  }
}
bool CodexHarnessSession::belongs(const QJsonObject &params,
                                  bool permitNewTurn) {
  Q_UNUSED(permitNewTurn)
  if (!m_busy || params.value("threadId").toString() != m_threadId)
    return false;
  const auto turn = safeId(params.value("turnId"));
  return !m_turnId.isEmpty() && turn == m_turnId;
}
bool CodexHarnessSession::publish(QJsonObject value) {
  if (!m_busy)
    return false;
  value["turnId"] = m_turnId;
  QPointer<CodexHarnessSession> self(this);
  const auto generation = m_generation;
  emit event(value);
  return self && generation == m_generation && m_busy && !m_interrupted;
}
void CodexHarnessSession::publishItem(const QJsonObject &item, bool completed) {
  const auto id = safeId(item.value("id")),
             type = item.value("type").toString();
  if (id.isEmpty() || type.isEmpty())
    return;
  if (!m_items.contains(id)) {
    if (m_items.size() >= 20000) {
      finish("failed", "Codex exceeded the item limit.");
      return;
    }
    m_order.append(id);
  }
  // Canonical final output is independently bounded, even for items without
  // streaming deltas. Keep native payload separate from ADE provider replay.
  auto bounded = item;
  const auto bytes =
      QJsonDocument(bounded).toJson(QJsonDocument::Compact).size();
  m_itemBytes +=
      bytes - (m_items.contains(id) ? QJsonDocument(m_items.value(id))
                                          .toJson(QJsonDocument::Compact)
                                          .size()
                                    : 0);
  if (m_itemBytes > MaximumOutput) {
    finish("failed", "Codex item exceeded the output limit.");
    return;
  }
  m_items[id] = bounded;
  if (type == "agentMessage" || type == "plan") {
    const auto text = item.value("text").toString();
    if (completed && text != m_text.value(id)) {
      m_outputChars += text.size();
      if (m_outputChars > MaximumOutput) {
        finish("failed", "Codex exceeded the output limit.");
        return;
      }
      m_text[id] = text;
      if (!publish({{"type", "textSnapshot"}, {"itemId", id}, {"text", text}}))
        return;
    }
    publish({{"type", "partState"},
             {"itemId", id},
             {"state", completed ? "done" : "streaming"}});
    return;
  }
  if (type == "reasoning") {
    if (completed) {
      QStringList parts;
      for (const auto &value : item.value("summary").toArray())
        if (value.isString())
          parts.append(value.toString());
      if (!parts.isEmpty() && !publish({{"type", "reasoningSnapshot"},
                                        {"itemId", id},
                                        {"text", parts.join('\n')}}))
        return;
    }
    publish({{"type", "partState"},
             {"itemId", id},
             {"state", completed ? "done" : "streaming"}});
    return;
  }
  if (type == "userMessage" || type == "hookPrompt")
    return;
  QJsonObject arguments;
  QString output;
  if (type == "commandExecution") {
    arguments = {{"command", item.value("command")},
                 {"cwd", item.value("cwd")}};
    output = item.value("aggregatedOutput").toString();
  } else if (type == "fileChange")
    arguments = {{"changes", item.value("changes")}};
  else if (type == "mcpToolCall" || type == "dynamicToolCall") {
    arguments = item.value("arguments").isObject()
                    ? item.value("arguments").toObject()
                    : QJsonObject{{"arguments", item.value("arguments")}};
    if (completed)
      output = QString::fromUtf8(
          QJsonDocument(
              item.value("result").isObject()
                  ? item.value("result").toObject()
                  : QJsonObject{{"content", item.value("contentItems")}})
              .toJson(QJsonDocument::Compact));
  } else if (type == "webSearch")
    arguments = {{"query", item.value("query")}};
  else
    arguments = {{"kind", type}};
  const auto status = item.value("status").toString();
  QJsonObject event{{"type", "tool"},
                    {"itemId", id},
                    {"name", item.value("tool").toString(type)},
                    {"arguments", arguments},
                    {"output", output.left(65536)},
                    {"state", completed
                                  ? (status == "failed" || status == "declined"
                                         ? "output-error"
                                         : "output-available")
                                  : "input-available"}};
  const bool ownedWrapper =
      !m_options.questionMcpExecutable.isEmpty() && type == "mcpToolCall" &&
      item.value("server") == "stagewise" &&
      item.value("tool") == "stagewise_request_user_input";
  event["acpPresentation"] = ownedWrapper
                                 ? HarnessQuestionMcp::wrapperPresentation(
                                       event.value("name").toString())
                                 : QJsonObject{};
  publish(event);
}
void CodexHarnessSession::receiveNotification(const QString &method,
                                              const QJsonObject &params) {
  if (!m_busy)
    return;
  if (m_turnSent && m_turnId.isEmpty() &&
      params.value("threadId").toString() == m_threadId) {
    QJsonObject event{{"method", method}, {"params", params}};
    m_earlyBytes += QJsonDocument(event).toJson(QJsonDocument::Compact).size();
    if (m_earlyBytes > MaximumOutput || m_earlyEvents.size() >= 20000) {
      finish("failed", "Codex exceeded the pending event limit.");
      return;
    }
    m_earlyEvents.append(event);
    return;
  }
  if (method == "turn/started") {
    auto current = params;
    current["turnId"] = params.value("turn").toObject().value("id");
    belongs(current, true);
    return;
  }
  if (method == "turn/completed") {
    auto current = params;
    const auto turn = params.value("turn").toObject();
    current["turnId"] = turn.value("id");
    if (!belongs(current, true))
      return;
    const auto status = turn.value("status").toString();
    finish(m_interrupted ? "interrupted" : status,
           status == "failed" ? "Codex could not complete the turn."
                              : QString());
    return;
  }
  if (m_interrupted)
    return;
  if (!belongs(params, true))
    return;
  if (!m_interrupted && m_pending.isEmpty())
    m_timeout.start(300000);
  if (method == "item/started" || method == "item/completed") {
    publishItem(params.value("item").toObject(), method == "item/completed");
    return;
  }
  if (method == "thread/tokenUsage/updated") {
    m_usage = params.value("tokenUsage").toObject();
    publish({{"type", "usage"}, {"usage", m_usage}});
    return;
  }
  const auto id = safeId(params.value("itemId"));
  if (id.isEmpty())
    return;
  if (!m_items.contains(id)) {
    if (m_items.size() >= 20000) {
      finish("failed", "Codex exceeded the item limit.");
      return;
    }
    const QJsonObject placeholder{{"id", id}};
    m_itemBytes +=
        QJsonDocument(placeholder).toJson(QJsonDocument::Compact).size();
    m_items.insert(id, placeholder);
    m_order.append(id);
  }
  const auto delta = params.value("delta").toString();
  m_outputChars += delta.size();
  if (m_outputChars > MaximumOutput) {
    finish("failed", "Codex exceeded the output limit.");
    return;
  }
  if (method == "item/agentMessage/delta" || method == "item/plan/delta") {
    m_text[id] += delta;
    publish({{"type", "text"}, {"itemId", id}, {"text", delta}});
  } else if (method == "item/reasoning/summaryTextDelta") {
    m_reasoning[id] += delta;
    publish({{"type", "reasoning"}, {"itemId", id}, {"text", delta}});
  } else if (method == "item/reasoning/summaryPartAdded") {
    publish({{"type", "reasoning"}, {"itemId", id}, {"text", ""}});
  } else if (method == "item/commandExecution/outputDelta" ||
             method == "item/fileChange/outputDelta") {
    publish({{"type", "toolOutput"}, {"itemId", id}, {"text", delta}});
  }
}
void CodexHarnessSession::receiveRequest(const QJsonValue &id,
                                         const QString &method,
                                         const QJsonObject &params) {
  if (m_busy && m_turnSent && m_turnId.isEmpty() &&
      params.value("threadId").toString() == m_threadId) {
    QJsonObject event{{"id", id}, {"method", method}, {"params", params}};
    m_earlyBytes += QJsonDocument(event).toJson(QJsonDocument::Compact).size();
    if (m_earlyBytes > MaximumOutput || m_earlyEvents.size() >= 20000) {
      m_server.reject(id, -32000, "Too many pending events.");
      finish("failed", "Codex exceeded the pending event limit.");
      return;
    }
    m_earlyEvents.append(event);
    return;
  }
  if (!belongs(params, true) || m_interrupted) {
    m_server.reject(id, -32000, "This turn is no longer active.");
    return;
  }
  if (m_pending.size() >= 32) {
    m_server.reject(id, -32000, "Too many pending user requests.");
    return;
  }
  const auto itemId = safeId(params.value("itemId"));
  const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (method == "item/commandExecution/requestApproval" ||
      method == "item/fileChange/requestApproval") {
    m_pending.insert(token, {id, method, itemId, params});
    pauseForDecision();
    emit approvalRequired(token,
                          method == "item/fileChange/requestApproval"
                              ? "Codex file change"
                              : "Codex command",
                          params);
    return;
  }
  if (method == "item/tool/requestUserInput") {
    if (m_questionMcp.pending()) {
      m_server.respond(id, QJsonObject{{"answers", QJsonObject{}}});
      return;
    }
    const auto questions = params.value("questions").toArray();
    if (questions.isEmpty() || questions.size() > 20 ||
        QJsonDocument(questions).toJson(QJsonDocument::Compact).size() >
            65536) {
      m_server.reject(id, -32602, "Unsupported user question form.");
      return;
    }
    m_pending.insert(token, {id, method, itemId, params});
    refreshQuestionBlock();
    pauseForDecision();
    emit questionRequired(token, questions);
    return;
  }
  if (method == "item/permissions/requestApproval") {
    // New generalized filesystem/network grants cannot be represented by a
    // boolean command dialog. Deny explicitly until a typed grant UI exists.
    m_server.respond(
        id, QJsonObject{{"permissions", QJsonObject{}}, {"scope", "turn"}});
    return;
  }
  m_server.reject(id, -32601,
                  "This Codex interaction is not supported by ADE yet.");
}
bool CodexHarnessSession::answerApproval(const QString &token, bool allow) {
  if (!m_busy || m_interrupted || !m_pending.contains(token))
    return false;
  const auto pending = m_pending.value(token);
  if (!pending.method.endsWith("/requestApproval"))
    return false;
  m_pending.remove(token);
  const bool sent = m_server.respond(
      pending.id, QJsonObject{{"decision", allow ? "accept" : "decline"}});
  resumeAfterDecision();
  return sent;
}
bool CodexHarnessSession::answerQuestion(const QString &token,
                                         const QJsonObject &answers) {
  if (!m_busy || m_interrupted || !m_pending.contains(token))
    return false;
  const auto pending = m_pending.value(token);
  if (pending.method != "item/tool/requestUserInput" ||
      QJsonDocument(answers).toJson(QJsonDocument::Compact).size() > 65536)
    return false;
  QJsonObject response;
  for (const auto &value : pending.parameters.value("questions").toArray()) {
    const auto question = value.toObject();
    const auto id = question.value("id").toString();
    if (id.isEmpty() || !answers.contains(id))
      return false;
    QJsonArray strings;
    const auto answer = answers.value(id);
    if (answer.isString())
      strings.append(answer);
    else if (answer.isArray()) {
      for (const auto &entry : answer.toArray()) {
        if (!entry.isString())
          return false;
        strings.append(entry);
      }
    } else
      return false;
    response[id] = QJsonObject{{"answers", strings}};
  }
  m_pending.remove(token);
  const bool sent =
      m_server.respond(pending.id, QJsonObject{{"answers", response}});
  refreshQuestionBlock();
  resumeAfterDecision();
  return sent;
}
bool CodexHarnessSession::cancelQuestion(const QString &token) {
  if (m_questionMcp.owns(token))
    return m_questionMcp.complete(token,
                                  QuestionMcpProtocol::cancelledResult());
  if (!m_pending.contains(token) ||
      m_pending.value(token).method != "item/tool/requestUserInput")
    return false;
  const auto pending = m_pending.take(token);
  const bool sent =
      m_server.respond(pending.id, QJsonObject{{"answers", QJsonObject{}}});
  refreshQuestionBlock();
  resumeAfterDecision();
  return sent;
}
void CodexHarnessSession::refreshQuestionBlock() {
  bool blocked = false;
  for (const auto &request : m_pending)
    blocked |= request.method == "item/tool/requestUserInput";
  m_questionMcp.setBlocked(blocked);
}
void CodexHarnessSession::pauseForDecision() {
  if (m_waitRemaining < 0) {
    m_waitRemaining = qMax(1, m_timeout.remainingTime());
    m_timeout.stop();
  }
  for (auto it = m_requests.cbegin(); it != m_requests.cend(); ++it)
    if (it.value() == "turn/start")
      m_server.suspendDeadline(it.key());
}
void CodexHarnessSession::resumeAfterDecision() {
  if (!m_busy || m_interrupted || !m_pending.isEmpty() ||
      m_questionMcp.pending())
    return;
  if (m_waitRemaining >= 0) {
    const auto remaining = m_waitRemaining;
    m_waitRemaining = -1;
    m_timeout.start(remaining);
  }
  for (auto it = m_requests.cbegin(); it != m_requests.cend(); ++it)
    if (it.value() == "turn/start")
      m_server.resumeDeadline(it.key());
}
void CodexHarnessSession::settleInputs() {
  const auto pending = m_pending;
  m_pending.clear();
  for (const auto &value : pending) {
    if (value.method == "item/tool/requestUserInput")
      m_server.respond(value.id, QJsonObject{{"answers", QJsonObject{}}});
    else
      m_server.respond(value.id, QJsonObject{{"decision", "cancel"}});
  }
}
void CodexHarnessSession::interrupt() {
  if (!m_busy)
    return;
  m_interrupted = true;
  const auto generation = m_generation;
  const QPointer<CodexHarnessSession> self(this);
  m_questionMcp.endTurn();
  if (!self || generation != m_generation || !m_busy)
    return;
  settleInputs();
  if (m_turnId.isEmpty()) {
    m_server.stop();
    finish("interrupted");
    return;
  }
  const auto id =
      m_server.call("turn/interrupt",
                    {{"threadId", m_threadId}, {"turnId", m_turnId}}, 10000);
  if (!id.isEmpty())
    m_requests.insert(id, "turn/interrupt");
  m_timeout.start(10000);
}
void CodexHarnessSession::shutdown() {
  ++m_generation;
  m_busy = false;
  m_timeout.stop();
  m_pending.clear();
  m_requests.clear();
  m_questionMcp.revoke();
  m_server.stop();
  m_options.attachments={};
  m_workspaceBinding={};m_threadId.clear();m_turnId.clear();
}
void CodexHarnessSession::finish(const QString &status, const QString &error) {
  if (!m_busy)
    return;
  const auto generation = m_generation;
  const QPointer<CodexHarnessSession> self(this);
  m_questionMcp.endTurn();
  if (!self || generation != m_generation || !m_busy)
    return;
  settleInputs();
  m_timeout.stop();
  m_busy = false;
  m_requests.clear();
  ++m_generation;
  if (status != "completed" && status != "interrupted")
    m_server.stop();
  QJsonArray items;
  for (const auto &value : m_order)
    items.append(m_items.value(value.toString()));
  QJsonObject result{{"status", status},
                     {"items", items},
                     {"usage", m_usage},
                     {"sessionState", sessionState()},
                     {"model", m_options.model}};
  // Partial text/reasoning is retained even when no completed item arrived.
  QJsonObject text, reasoning;
  for (auto it = m_text.cbegin(); it != m_text.cend(); ++it)
    text[it.key()] = it.value();
  for (auto it = m_reasoning.cbegin(); it != m_reasoning.cend(); ++it)
    reasoning[it.key()] = it.value();
  result["partialText"] = text;
  result["partialReasoning"] = reasoning;
  if (!error.isEmpty())
    result["error"] = error;
  emit finished(result);
}
} // namespace ade
