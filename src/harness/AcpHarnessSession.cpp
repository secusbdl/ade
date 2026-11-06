#include "AcpHarnessSession.h"
#include "AcpForm.h"
#include "QuestionMcpProtocol.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QUuid>
#include <cmath>
namespace ade {
namespace {
constexpr qsizetype Bound = 4 * 1024 * 1024;
QString id(const QJsonValue &v) {
  const auto s = v.toString();
  return !s.isEmpty() && s.size() <= 512 && !s.contains(QChar::Null)
             ? s
             : QString{};
}
bool same(const QString &a, const QString &b) {
#ifdef Q_OS_WIN
  return a.compare(b, Qt::CaseInsensitive) == 0;
#else
  return a == b;
#endif
}
QJsonArray choices(const QJsonObject &option) {
  QJsonArray result;
  for (const auto &v : option["options"].toArray()) {
    const auto row = v.toObject();
    if (row["options"].isArray())
      for (const auto &entry : row["options"].toArray())
        result.append(entry);
    else
      result.append(row);
    if (result.size() > 10000)
      return {};
  }
  return result;
}
QJsonObject permission(const QJsonArray &options, bool allow) {
  const QStringList kinds = allow ? QStringList{"allow_once", "allow_always"}
                                  : QStringList{"reject_once", "reject_always"};
  for (const auto &kind : kinds)
    for (const auto &v : options) {
      const auto o = v.toObject();
      if (o["kind"] == kind && !id(o["optionId"]).isEmpty())
        return {{"outcome", QJsonObject{{"outcome", "selected"},
                                        {"optionId", o["optionId"]}}}};
    }
  return {{"outcome", QJsonObject{{"outcome", "cancelled"}}}};
}
} // namespace
AcpHarnessSession::AcpHarnessSession(QObject *parent)
    : QObject(parent), m_process(this), m_questionMcp(this) {
  connect(&m_questionMcp, &QuestionMcpBridge::questionRequested, this,
          [this](const QString &token, const QString &turn,
                 const QJsonObject &form) {
            bool otherQuestion = !m_stagewiseQuestion.isEmpty();
            for (const auto &pending : m_pending)
              otherQuestion |= pending.question;
            if (!m_busy || m_interrupted || !m_promptSent || turn != m_turnId ||
                otherQuestion) {
              m_questionMcp.complete(token,
                                     QuestionMcpProtocol::cancelledResult());
              return;
            }
            m_stagewiseQuestion = token;
            m_stagewiseForm = form;
            refreshPromptDeadline();
            const QPointer<AcpHarnessSession> self(this);
            const auto generation = m_generation;
            if (!publishStagewiseQuestion(token, form) || !self ||
                !active(generation) || m_stagewiseQuestion != token)
              return;
            emit stagewiseQuestionRequired(token, form);
          });
  connect(&m_questionMcp, &QuestionMcpBridge::questionCancelled, this,
          [this](const QString &token) {
            if (token != m_stagewiseQuestion)
              return;
            const auto form = m_stagewiseForm;
            m_stagewiseQuestion.clear();
            m_stagewiseForm = {};
            refreshPromptDeadline();
            const QPointer<AcpHarnessSession> self(this);
            const auto generation = m_generation;
            if (m_busy)
              publishStagewiseQuestion(token, form,
                                       QuestionMcpProtocol::cancelledResult());
            if (self && generation == m_generation)
              emit stagewiseQuestionCancelled(token);
          });
  m_cancelTimer.setSingleShot(true);
  connect(&m_cancelTimer, &QTimer::timeout, this, [this] {
    if (m_busy)
      finish("interrupted");
  });
  connect(&m_process, &JsonRpcProcess::started, this,
          &AcpHarnessSession::initialize);
  connect(&m_process, &JsonRpcProcess::response, this,
          &AcpHarnessSession::response);
  connect(&m_process, &JsonRpcProcess::notification, this,
          &AcpHarnessSession::notification);
  connect(&m_process, &JsonRpcProcess::serverRequest, this,
          &AcpHarnessSession::request);
  connect(&m_process, &JsonRpcProcess::failed, this, [this](const QString &) {
    if (m_busy)
      finish(m_interrupted ? "interrupted" : "failed",
             m_interrupted ? QString{}
                           : "The OpenCode process became unavailable.");
  });
}
AcpHarnessSession::~AcpHarnessSession() {
  disconnect(&m_questionMcp, nullptr, this, nullptr);
  shutdown();
}
QProcessEnvironment AcpHarnessSession::environment(bool alwaysAllow) {
  auto env = JsonRpcProcess::managedEnvironment();
  auto config =
      QJsonDocument::fromJson(env.value("OPENCODE_CONFIG_CONTENT").toUtf8())
          .object();
  auto permissions = config["permission"].isString()
                         ? QJsonObject{{"*", config["permission"]}}
                         : config["permission"].toObject();
  permissions["bash"] = alwaysAllow ? "allow" : "ask";
  config["permission"] = permissions;
  auto experimental = config["experimental"].toObject();
  experimental["mcp_timeout"] =
      qMax(43200000., experimental["mcp_timeout"].toDouble());
  config["experimental"] = experimental;
  env.insert(
      "OPENCODE_CONFIG_CONTENT",
      QString::fromUtf8(QJsonDocument(config).toJson(QJsonDocument::Compact)));
  return env;
}
bool AcpHarnessSession::startTurn(const AcpHarnessOptions &options,
                                  const QJsonArray &input,
                                  const QJsonObject &saved) {
  if (m_busy)
    return false;
  const auto exe = QStandardPaths::findExecutable(
      options.executable.isEmpty() ? "opencode" : options.executable);
  const auto root = HarnessWorkspace::resolve(options.workspace);
  if (options.promptTimeoutMs < 100 || options.promptTimeoutMs > 600000 ||
      exe.isEmpty() || root.path.isEmpty() || options.providerId.isEmpty() ||
      input.isEmpty() ||
      QJsonDocument(input).toJson(QJsonDocument::Compact).size() > Bound) {
    emit failed("OpenCode requires its installed CLI, an owned workspace, and "
                "bounded input.");
    return false;
  }
  for (const auto &v : input) {
    const auto p = v.toObject();
    if ((p["type"] != "text" || !p["text"].isString()) &&
        (p["type"] != "image" || !p["data"].isString() ||
         !p["mimeType"].toString().startsWith("image/"))) {
      emit failed("Unsupported ACP input content.");
      return false;
    }
  }
  const bool reuse =
      m_initialized && m_process.isRunning() &&
      same(m_options.executable, exe) &&
      m_workspace.identity == root.identity &&
      m_options.providerId == options.providerId &&
      m_options.alwaysAllow == options.alwaysAllow &&
      m_options.questionMcpExecutable == options.questionMcpExecutable &&
      !saved["fork"].toBool() &&
      (id(saved["sessionId"]).isEmpty() || saved["sessionId"] == m_sessionId);
  if (!reuse) {
    m_questionMcp.revoke();
    m_process.stop();
    m_initialized = false;
    m_sessionId.clear();
    m_config = {};
    m_capabilities = {};
  }
  ++m_generation;
  m_options = options;
  m_options.executable = exe;
  m_options.workspace = root.path;
  m_workspace = root;
  m_input = input;
  m_saved = saved;
  m_calls.clear();
  m_pending.clear();
  m_promptRequestId.clear();
  m_stagewiseQuestion.clear();
  m_stagewiseForm = {};
  m_configured.clear();
  m_parts = {};
  m_toolStates.clear();
  m_turnId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  m_usage = {};
  m_outputSize = 0;
  m_partSequence = 0;
  m_currentPart.clear();
  m_currentType.clear();
  m_busy = true;
  m_promptSent = false;
  m_interrupted = false;
  m_created = false;
  if (saved["version"].toInt() != 1 || saved["kind"] != "opencode" ||
      saved["providerId"] != options.providerId ||
      !same(saved["executable"].toString(), exe) ||
      saved["workspaceIdentity"].toString() != root.identity ||
      saved["alwaysAllow"].toBool() != options.alwaysAllow ||
      saved["fork"].toBool())
    m_saved = {};
  if (reuse) {
    beginSession();
    return true;
  }
  const QPointer<AcpHarnessSession> self(this);
  const auto generation = m_generation;
  const bool started = m_process.start(exe, {"acp"}, root.path,
                                       environment(options.alwaysAllow));
  if (self && active(generation) && !started)
    finish("failed", "OpenCode could not start.");
  return true;
}
bool AcpHarnessSession::active(quint64 generation) const {
  return m_busy && generation == m_generation;
}
bool AcpHarnessSession::call(const QString &method, const QJsonObject &params,
                             int timeout) {
  const auto requestId = m_process.request(method, params, timeout);
  if (requestId.isEmpty()) {
    finish("failed", "The ACP request could not be sent.");
    return false;
  }
  m_calls.insert(requestId, method);
  if (method == "session/prompt")
    m_promptRequestId = requestId;
  return true;
}
void AcpHarnessSession::initialize() {
  if (!m_busy)
    return;
  call("initialize",
       {{"protocolVersion", 1},
        {"clientInfo",
         QJsonObject{{"name", "ade"}, {"title", "ADE"}, {"version", "0.1.0"}}},
        {"clientCapabilities",
         QJsonObject{{"plan", QJsonObject{}},
                     {"session",
                      QJsonObject{{"configOptions",
                                   QJsonObject{{"boolean", QJsonObject{}}}}}},
                     {"elicitation", QJsonObject{{"form", QJsonObject{}}}}}}});
}
void AcpHarnessSession::beginSession() {
  if (!m_busy || m_interrupted)
    return;
  if (!m_sessionId.isEmpty()) {
    configure();
    return;
  }
  QJsonArray servers;
  if (!m_options.questionMcpExecutable.isEmpty()) {
    QString error;
    const auto config =
        m_questionMcp.start(m_options.questionMcpExecutable, &error);
    if (config.isEmpty()) {
      finish("failed", error);
      return;
    }
    servers.append(config);
  }
  QJsonObject params{{"cwd", m_workspace.path}, {"mcpServers", servers}};
  const auto previous = id(m_saved["sessionId"]);
  if (!previous.isEmpty() &&
      m_capabilities["sessionCapabilities"].toObject()["resume"].isObject()) {
    params["sessionId"] = previous;
    call("session/resume", params);
  } else {
    m_created = true;
    call("session/new", params);
  }
}
QJsonObject AcpHarnessSession::sessionState() const {
  if (m_sessionId.isEmpty())
    return {};
  return {{"version", 1},
          {"kind", "opencode"},
          {"providerId", m_options.providerId},
          {"executable", m_options.executable},
          {"workspace", m_workspace.path},
          {"workspaceIdentity", m_workspace.identity},
          {"alwaysAllow", m_options.alwaysAllow},
          {"sessionId", m_sessionId}};
}
void AcpHarnessSession::configure() {
  if (!m_busy || m_interrupted)
    return;
  for (const auto &v : m_config) {
    const auto option = v.toObject();
    const auto key = id(option["id"]);
    if (option["type"] != "select" || key.isEmpty() ||
        m_configured.contains(key))
      continue;
    const auto category = option["category"].toString();
    QString wanted;
    if (key == "model" || category == "model")
      wanted = m_options.model;
    else if (QStringList{"thought_level", "reasoning_effort", "effort"}
                 .contains(key) ||
             QStringList{"thought_level", "reasoning_effort", "effort"}
                 .contains(category)) {
      wanted = m_options.reasoningEffort;
      if (m_options.thinkingDisabled) {
        wanted.clear();
        for (const auto &candidate : choices(option)) {
          const auto value = candidate.toObject()["value"].toString();
          if (QStringList{"off", "none", "disabled"}.contains(
                  value.toLower())) {
            wanted = value;
            break;
          }
        }
      }
    }
    if (wanted.isEmpty() || wanted == "default")
      continue;
    bool found = false;
    for (const auto &choice : choices(option))
      found |= choice.toObject()["value"] == wanted;
    if (!found || option["currentValue"] == wanted)
      continue;
    m_configured.insert(key);
    call("session/set_config_option",
         {{"sessionId", m_sessionId}, {"configId", key}, {"value", wanted}});
    return;
  }
  prompt();
}
void AcpHarnessSession::prompt() {
  if (!m_busy || m_interrupted || m_promptSent)
    return;
  auto content = m_input;
  for (const auto &v : content)
    if (v.toObject()["type"] == "image" &&
        !m_capabilities["promptCapabilities"].toObject()["image"].toBool()) {
      finish("failed", "This ACP agent does not advertise image input.");
      return;
    }
  if (m_created && !m_options.historyContext.isEmpty())
    content.prepend(
        QJsonObject{{"type", "text"}, {"text", m_options.historyContext}});
  if (!m_options.instructions.isEmpty())
    content.prepend(
        QJsonObject{{"type", "text"}, {"text", m_options.instructions}});
  if (QJsonDocument(content).toJson(QJsonDocument::Compact).size() > Bound) {
    finish("failed", "ACP prompt exceeded its limit.");
    return;
  }
  m_promptSent = true;
  const QPointer<AcpHarnessSession> self(this);
  const auto generation = m_generation;
  m_questionMcp.beginTurn(m_turnId);
  if (!self || !active(generation))
    return;
  call("session/prompt", {{"sessionId", m_sessionId}, {"prompt", content}},
       m_options.promptTimeoutMs);
}
void AcpHarnessSession::response(const QString &requestId,
                                 const QJsonValue &value,
                                 const QJsonObject &error) {
  if (!m_busy)
    return;
  const auto method = m_calls.take(requestId);
  if (requestId == m_promptRequestId)
    m_promptRequestId.clear();
  if (method.isEmpty())
    return;
  if (!error.isEmpty()) {
    if (method == "session/resume" && !m_interrupted) {
      m_saved = {};
      beginSession();
      return;
    }
    if (error["code"].toInt() == -32000) {
      const QPointer<AcpHarnessSession> self(this);
      const auto generation = m_generation;
      emit authenticationRequired(m_authMethods);
      if (!self || !active(generation))
        return;
    }
    finish(
        m_interrupted ? "interrupted" : "failed",
        m_interrupted ? QString{}
        : error["code"].toInt() == -32000
            ? QString("OpenCode requires authentication. Run opencode auth "
                      "login, then retry; existing ADE history is preserved.")
            : QString("OpenCode rejected the ACP request; existing ADE history "
                      "is preserved."));
    return;
  }
  const auto object = value.toObject();
  if (method == "initialize") {
    if (object["protocolVersion"].toInt() != 1) {
      finish("failed", "The agent did not negotiate ACP v1.");
      return;
    }
    m_initialized = true;
    m_capabilities = object["agentCapabilities"].toObject();
    m_authMethods = object["authMethods"].toArray();
    beginSession();
  } else if (method == "session/new" || method == "session/resume") {
    m_sessionId = method == "session/new" ? id(object["sessionId"])
                                          : id(m_saved["sessionId"]);
    if (m_sessionId.isEmpty()) {
      finish("failed", "Invalid ACP session identifier.");
      return;
    }
    m_config = object["configOptions"].toArray();
    const QPointer<AcpHarnessSession> self(this);
    const auto generation = m_generation;
    emit sessionStateChanged(sessionState());
    if (self && active(generation))
      configure();
  } else if (method == "session/set_config_option") {
    m_config = object["configOptions"].toArray();
    configure();
  } else if (method == "session/prompt") {
    const auto reason = object["stopReason"].toString();
    if (m_interrupted || reason == "cancelled")
      finish("interrupted");
    else if (reason == "end_turn" || reason == "max_tokens" ||
             reason == "max_turn_requests" || reason == "refusal")
      finish("completed");
    else
      finish("failed", "The ACP prompt returned an invalid stop reason.");
  }
}
bool AcpHarnessSession::publish(QJsonObject value) {
  value["turnId"] = m_turnId;
  const auto size = QJsonDocument(value).toJson(QJsonDocument::Compact).size();
  if (size > Bound - m_outputSize || m_parts.size() >= 20000) {
    finish("failed", "ACP output exceeded its limit.");
    return false;
  }
  m_outputSize += size;
  m_parts.append(value);
  const QPointer<AcpHarnessSession> self(this);
  const auto generation = m_generation;
  emit event(value);
  return self && active(generation) && !m_interrupted;
}
void AcpHarnessSession::notification(const QString &method,
                                     const QJsonObject &params) {
  if (!m_busy || !m_promptSent || m_interrupted || method != "session/update" ||
      params["sessionId"] != m_sessionId)
    return;
  const auto update = params["update"].toObject();
  const auto kind = update["sessionUpdate"].toString();
  if (kind == "agent_message_chunk" || kind == "agent_thought_chunk") {
    const auto content = update["content"].toObject();
    const auto type =
        kind == "agent_message_chunk" ? QString("text") : QString("reasoning");
    if (content["type"] == "text") {
      const auto message = id(update["messageId"]);
      if (!message.isEmpty()) {
        m_currentType = type;
        m_currentPart = type + ":" + message;
      } else if (m_currentType != type) {
        m_currentType = type;
        m_currentPart = "acp-part-" + QString::number(++m_partSequence);
      }
      publish({{"type", type},
               {"id", m_currentPart},
               {"text", content["text"].toString()}});
    } else if (content["type"] == "image") {
      m_currentType.clear();
      publish({{"type", "image"},
               {"id", "acp-part-" + QString::number(++m_partSequence)},
               {"content", content}});
    }
  } else if (kind == "tool_call" || kind == "tool_call_update") {
    const auto key = id(update["toolCallId"]);
    if (key.isEmpty())
      return;
    m_currentType.clear();
    auto merged = m_toolStates.value(key);
    auto meta = merged["_meta"].toObject();
    const auto updateMeta = update["_meta"].toObject();
    for (auto it = updateMeta.begin(); it != updateMeta.end(); ++it)
      meta[it.key()] = it.value();
    for (auto it = update.begin(); it != update.end(); ++it)
      merged[it.key()] = it.value();
    merged["_meta"] = meta;
    const QJsonValue exit = meta["terminal_exit"].toObject().value("exit_code");
    if (merged["kind"] == "execute" && exit.isDouble())
      merged["status"] = exit.toDouble() == 0 ? "completed" : "failed";
    m_toolStates[key] = merged;
    const QJsonValue todos = merged["rawInput"].toObject().value("todos");
    if (todos.isArray()) {
      QJsonArray entries;
      for (const auto &item : todos.toArray()) {
        const auto todo = item.toObject();
        const auto content =
                       todo["content"].toString(todo["description"].toString()),
                   status = todo["status"].toString();
        if (!content.isEmpty() && !status.isEmpty())
          entries.append(QJsonObject{{"content", content}, {"status", status}});
      }
      if (entries.size() == todos.toArray().size()) {
        QJsonObject planUpdate =
            entries.isEmpty()
                ? QJsonObject{{"sessionUpdate", "plan_removed"},
                              {"planId", key}}
                : QJsonObject{{"sessionUpdate", "plan_update"},
                              {"plan", QJsonObject{{"type", "items"},
                                                   {"planId", key},
                                                   {"entries", entries}}}};
        publish({{"type", "plan"}, {"update", planUpdate}});
        return;
      }
    }
    publish({{"type", "tool"}, {"id", key}, {"update", merged}});
  } else if (kind == "plan" || kind == "plan_update" ||
             kind == "plan_removed") {
    m_currentType.clear();
    publish({{"type", "plan"}, {"update", update}});
  } else if (kind == "usage_update") {
    const auto used = update["used"].toDouble(-1),
               size = update["size"].toDouble(-1);
    if (!update["used"].isDouble() || !update["size"].isDouble() || used < 0 ||
        size <= 0 || !std::isfinite(used) || !std::isfinite(size) ||
        std::floor(used) != used || std::floor(size) != size ||
        used > 9007199254740991. || size > 9007199254740991.)
      return;
    m_usage = {{"used", used}, {"size", size}};
    publish({{"type", "usage"}, {"usage", m_usage}});
  } else if (kind == "config_option_update") {
    m_config = update["configOptions"].toArray();
  }
}
void AcpHarnessSession::request(const QJsonValue &requestId,
                                const QString &method,
                                const QJsonObject &params) {
  const bool sessionMatches = params.contains("sessionId")
                                  ? params["sessionId"] == m_sessionId
                                  : method == "elicitation/create";
  if (!m_busy || m_interrupted || !m_promptSent || !sessionMatches) {
    m_process.reject(requestId, -32602,
                     "The request has no active owning session.");
    return;
  }
  if (m_pending.size() >= 50 ||
      QJsonDocument(params).toJson(QJsonDocument::Compact).size() >
          256 * 1024) {
    m_process.reject(requestId, -32602, "The request exceeds its limit.");
    return;
  }
  for (const auto &pending : m_pending)
    if (pending.id == requestId) {
      m_process.reject(requestId, -32600,
                       "Duplicate pending ACP request identity.");
      return;
    }
  const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (method == "session/request_permission") {
    if (id(params["toolCall"].toObject()["toolCallId"]).isEmpty() ||
        !params["options"].isArray() ||
        params["options"].toArray().size() > 32) {
      m_process.reject(requestId, -32602, "Invalid permission options.");
      return;
    }
    const QString toolId =
        params["toolCall"].toObject().value("toolCallId").toString();
    for (const auto &pending : m_pending) {
      if (!pending.question &&
          pending.parameters["toolCall"].toObject()["toolCallId"] == toolId) {
        m_process.reject(requestId, -32600,
                         "An approval is already pending for this tool call.");
        return;
      }
    }
    if (m_options.alwaysAllow) {
      m_process.respond(requestId,
                        permission(params["options"].toArray(), true));
      return;
    }
    m_pending.insert(token, {requestId, params, false});
    refreshPromptDeadline();
    emit approvalRequired(token, params);
  } else if (method == "elicitation/create" &&
             !acpQuestionForm(params).isEmpty()) {
    bool otherQuestion = !m_stagewiseQuestion.isEmpty();
    for (const auto &pending : m_pending)
      otherQuestion |= pending.question;
    if (otherQuestion) {
      m_process.respond(requestId, QJsonObject{{"action", "cancel"}});
      return;
    }
    m_pending.insert(token, {requestId, params, true});
    refreshPromptDeadline();
    emit questionRequired(token, params);
  } else
    m_process.reject(requestId, -32601,
                     "This ACP client does not support that request.");
}
bool AcpHarnessSession::answerApproval(const QString &token, bool allow) {
  const auto found = m_pending.constFind(token);
  if (!m_busy || m_interrupted || found == m_pending.cend() || found->question)
    return false;
  const auto pending = *found;
  m_pending.remove(token);
  refreshPromptDeadline();
  return m_process.respond(
      pending.id, permission(pending.parameters["options"].toArray(), allow));
}
bool AcpHarnessSession::validQuestionAnswer(const QString &token,
                                            const QJsonObject &answers) const {
  if (m_busy && !m_interrupted && token == m_stagewiseQuestion &&
      !token.isEmpty())
    return QJsonDocument(answers).toJson(QJsonDocument::Compact).size() <=
           2 * 1024 * 1024;
  const auto found = m_pending.constFind(token);
  return m_busy && !m_interrupted && found != m_pending.cend() &&
         found->question && acpQuestionAnswers(found->parameters, answers);
}
bool AcpHarnessSession::answerQuestion(const QString &token,
                                       const QJsonObject &answers) {
  const auto found = m_pending.constFind(token);
  if (!m_busy || m_interrupted || found == m_pending.cend() ||
      !found->question || !acpQuestionAnswers(found->parameters, answers))
    return false;
  const auto pending = *found;
  m_pending.remove(token);
  refreshPromptDeadline();
  return m_process.respond(
      pending.id, QJsonObject{{"action", "accept"}, {"content", answers}});
}
bool AcpHarnessSession::cancelQuestion(const QString &token) {
  if (!token.isEmpty() && token == m_stagewiseQuestion)
    return completeStagewiseQuestion(token,
                                     QuestionMcpProtocol::cancelledResult());
  const auto found = m_pending.constFind(token);
  if (!m_busy || found == m_pending.cend() || !found->question)
    return false;
  const auto pending = *found;
  m_pending.remove(token);
  refreshPromptDeadline();
  return m_process.respond(pending.id, QJsonObject{{"action", "cancel"}});
}
void AcpHarnessSession::refreshPromptDeadline() {
  if (m_promptRequestId.isEmpty())
    return;
  if (!m_pending.isEmpty() || !m_stagewiseQuestion.isEmpty())
    m_process.suspendDeadline(m_promptRequestId);
  else
    m_process.resumeDeadline(m_promptRequestId);
}
bool AcpHarnessSession::publishStagewiseQuestion(const QString &token,
                                                 const QJsonObject &form,
                                                 const QJsonObject &result,
                                                 const QString &error) {
  return publish(
      {{"type", "stagewiseQuestion"},
       {"id", "stagewise-mcp:" + token},
       {"form", form},
       {"result", result},
       {"error", error},
       {"state", result.isEmpty() && error.isEmpty() ? "input-available"
                 : error.isEmpty()                   ? "output-available"
                                                     : "output-error"}});
}
bool AcpHarnessSession::completeStagewiseQuestion(const QString &token,
                                                  const QJsonObject &result,
                                                  const QString &error) {
  if (!m_busy || token.isEmpty() || token != m_stagewiseQuestion)
    return false;
  if (error.isEmpty()) {
    const auto steps = result.value("completedSteps");
    if (!result.value("completed").isBool() ||
        !result.value("cancelled").isBool() ||
        !result.value("answers").isObject() || !steps.isDouble() ||
        steps.toDouble() < 0 ||
        steps.toDouble() > m_stagewiseForm.value("steps").toArray().size() ||
        std::floor(steps.toDouble()) != steps.toDouble() ||
        (result.value("completed").toBool() &&
         result.value("cancelled").toBool()) ||
        QJsonDocument(result).toJson(QJsonDocument::Compact).size() >
            2 * 1024 * 1024)
      return false;
  }
  const auto form = m_stagewiseForm;
  m_stagewiseQuestion.clear();
  m_stagewiseForm = {};
  refreshPromptDeadline();
  const QPointer<AcpHarnessSession> self(this);
  const auto generation = m_generation;
  const bool accepted = m_questionMcp.complete(token, result, error);
  if (!self || !active(generation))
    return accepted;
  publishStagewiseQuestion(token, form, result, error);
  return accepted;
}
void AcpHarnessSession::interrupt() {
  if (!m_busy)
    return;
  m_interrupted = true;
  const QPointer<AcpHarnessSession> self(this);
  const auto generation = m_generation;
  m_questionMcp.endTurn();
  if (!self || generation != m_generation || !m_busy)
    return;
  const auto pending = m_pending;
  m_pending.clear();
  refreshPromptDeadline();
  for (const auto &p : pending) {
    m_process.respond(p.id, p.question ? QJsonObject{{"action", "cancel"}}
                                       : permission({}, false));
    if (!self || generation != m_generation || !m_busy)
      return;
  }
  if (m_sessionId.isEmpty() || !m_promptSent) {
    finish("interrupted");
    return;
  }
  m_process.notify("session/cancel", {{"sessionId", m_sessionId}});
  m_cancelTimer.start(2000);
}
void AcpHarnessSession::finish(const QString &status, const QString &error) {
  if (!m_busy)
    return;
  const QPointer<AcpHarnessSession> bridgeGuard(this);
  const auto bridgeGeneration = m_generation;
  m_questionMcp.endTurn();
  if (!bridgeGuard || bridgeGeneration != m_generation || !m_busy)
    return;
  m_cancelTimer.stop();
  m_busy = false;
  m_calls.clear();
  m_pending.clear();
  m_promptRequestId.clear();
  m_stagewiseQuestion.clear();
  m_stagewiseForm = {};
  if (status != "completed") {
    m_questionMcp.revoke();
    m_process.stop();
    m_initialized = false;
    m_sessionId.clear();
  }
  const QJsonObject result{{"status", status},
                           {"events", m_parts},
                           {"usage", m_usage},
                           {"sessionState", sessionState()},
                           {"error", error}};
  const QPointer<AcpHarnessSession> self(this);
  const auto generation = m_generation;
  if (!error.isEmpty()) {
    emit failed(error);
    if (!self || generation != m_generation)
      return;
  }
  emit finished(result);
}
void AcpHarnessSession::shutdown() {
  ++m_generation;
  m_busy = false;
  m_initialized = false;
  m_interrupted = false;
  m_cancelTimer.stop();
  m_calls.clear();
  m_pending.clear();
  m_promptRequestId.clear();
  m_stagewiseQuestion.clear();
  m_stagewiseForm = {};
  m_sessionId.clear();
  m_stagewiseQuestion.clear();
  m_stagewiseForm = {};
  m_promptRequestId.clear();
  m_questionMcp.revoke();
  m_process.stop();
  m_workspace = {};
}
} // namespace ade
