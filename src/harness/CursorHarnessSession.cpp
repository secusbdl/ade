#include "CursorHarnessSession.h"
#include "CursorExtension.h"
#include <QJsonDocument>
#include <QUuid>
#include <QtConcurrentRun>
namespace ade {
namespace {
constexpr qsizetype Maximum = 4 * 1024 * 1024;
QString identity(const QJsonValue &v) {
  const auto s = v.toString();
  return !s.isEmpty() && s.size() <= 512 && !s.contains(QChar::Null)
             ? s
             : QString{};
}
QString token() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QJsonObject cancelled() {
  return {{"outcome", QJsonObject{{"outcome", "cancelled"}}}};
}
QJsonObject permission(const QJsonObject &p, bool allow) {
  for (const auto &kind : allow ? QStringList{"allow_once", "allow_always"}
                                : QStringList{"reject_once", "reject_always"})
    for (const auto &v : p["options"].toArray()) {
      const auto o = v.toObject();
      if (o["kind"] == kind && !identity(o["optionId"]).isEmpty())
        return {{"outcome", QJsonObject{{"outcome", "selected"},
                                        {"optionId", o["optionId"]}}}};
    }
  return cancelled();
}
QJsonArray choices(const QJsonObject &o) {
  QJsonArray r;
  for (const auto &v : o["options"].toArray()) {
    const auto e = v.toObject();
    if (e["options"].isArray())
      for (const auto &c : e["options"].toArray())
        r.append(c);
    else
      r.append(v);
    if (r.size() > 10000)
      return {};
  }
  return r;
}
} // namespace
CursorHarnessSession::CursorHarnessSession(QObject *parent)
    : QObject(parent), m_process(this) {
  m_deadline.setSingleShot(true);
  m_cancel.setSingleShot(true);
  m_settle.setSingleShot(true);
  connect(&m_deadline, &QTimer::timeout, this, [this] {
    finish("failed", "Cursor exceeded the active turn time limit.");
  });
  connect(&m_cancel, &QTimer::timeout, this, [this] { finish("interrupted"); });
  connect(&m_settle, &QTimer::timeout, this, &CursorHarnessSession::settle);
  connect(&m_process, &JsonRpcProcess::started, this, [this] {
    if (m_busy)
      call("initialize",
           {{"protocolVersion", 1},
            {"clientInfo", QJsonObject{{"name", "ade"}, {"version", "0.1.0"}}},
            {"clientCapabilities",
             QJsonObject{{"fs", QJsonObject{{"readTextFile", false},
                                            {"writeTextFile", false}}},
                         {"terminal", false}}}});
  });
  connect(&m_process, &JsonRpcProcess::response, this,
          &CursorHarnessSession::response);
  connect(&m_process, &JsonRpcProcess::notification, this,
          &CursorHarnessSession::notification);
  connect(&m_process, &JsonRpcProcess::serverRequest, this,
          &CursorHarnessSession::request);
  connect(&m_process, &JsonRpcProcess::failed, this, [this](const QString &) {
    if (m_busy)
      finish(m_interrupted ? "interrupted" : "failed",
             m_interrupted ? QString{}
                           : QString("Cursor process became unavailable."));
  });
}
CursorHarnessSession::~CursorHarnessSession() { shutdown(); }
bool CursorHarnessSession::startTurn(const CursorHarnessOptions &o,
                                     const QJsonArray &input,
                                     const QJsonObject &saved) {
  if (m_busy)
    return false;
  QString error;
  const auto exe = CursorExecutable::resolve(o.executable, &error);
  const auto root = HarnessWorkspace::resolve(o.workspace);
  if (!exe.valid() || root.path.isEmpty() || identity(o.providerId).isEmpty() ||
      identity(o.ownerId).isEmpty() ||
      !QStringList{"agent", "plan", "ask"}.contains(o.mode) ||
      o.promptTimeoutMs < 100 || o.promptTimeoutMs > 600000 ||
      input.isEmpty() ||
      QJsonDocument(input).toJson(QJsonDocument::Compact).size() > Maximum) {
    emit failed(error.isEmpty() ? "Cursor requires an owned workspace, "
                                  "provider, conversation and bounded input."
                                : error);
    return false;
  }
  for (const auto &v : input) {
    const auto p = v.toObject();
    if (!v.isObject() || (p["type"] != "text" && p["type"] != "image") ||
        (p["type"] == "text" && !p["text"].isString()) ||
        (p["type"] == "image" &&
         (!p["data"].isString() ||
          !p["mimeType"].toString().startsWith("image/")))) {
      emit failed("Unsupported Cursor input.");
      return false;
    }
  }
  const bool savedMatches =
      saved["version"].toInt() == 1 && saved["kind"] == "cursor" &&
      saved["providerId"] == o.providerId && saved["ownerId"] == o.ownerId &&
      saved["executableIdentity"] == exe.identity &&
      saved["workspaceIdentity"] == root.identity &&
      saved["alwaysAllow"].isBool() &&
      saved["alwaysAllow"].toBool() == o.alwaysAllow &&
      !saved["fork"].toBool() && !identity(saved["sessionId"]).isEmpty();
  const bool reuse =
      savedMatches && m_authenticated && m_process.isRunning() &&
      m_executable.identity == exe.identity &&
      m_workspace.identity == root.identity &&
      m_options.providerId == o.providerId && m_options.ownerId == o.ownerId &&
      m_options.alwaysAllow == o.alwaysAllow && saved["sessionId"] == m_session;
  if (!reuse) {
    m_process.stop();
    m_authenticated = false;
    m_session.clear();
    m_capabilities = {};
    m_config = {};
  }
  ++m_epoch;
  m_options = o;
  m_executable = exe;
  m_workspace = root;
  m_saved = saved;
  m_input = input;
  m_busy = true;
  m_interrupted = false;
  m_promptSent = false;
  m_promptReturned = false;
  m_new = false;
  m_turn = token();
  m_events = {};
  m_usage = {};
  m_tools.clear();
  m_calls.clear();
  m_pending.clear();
  m_configured.clear();
  m_authToken.clear();
  m_promptId.clear();
  m_bytes = 0;
  m_partNumber = 0;
  m_part.clear();
  m_partType.clear();
  m_remaining = o.promptTimeoutMs;
  if (!savedMatches)
    m_saved = {};
  if (reuse) {
    beginSession();
    return true;
  }
  const QPointer<CursorHarnessSession> self(this);
  const auto epoch = m_epoch;
  auto env = JsonRpcProcess::managedEnvironment();
  env.insert("CURSOR_AGENT_DISABLE_DEBUG_LOG", "1");
  const auto started =
      m_process.start(exe.program, exe.arguments, root.path, env);
  if (self && current(epoch) && !started)
    finish("failed", "Cursor could not start.");
  return true;
}
bool CursorHarnessSession::call(const QString &method, const QJsonObject &p,
                                int timeout) {
  const QPointer<CursorHarnessSession> self(this);
  const auto epoch = m_epoch;
  const auto id = m_process.request(method, p, timeout);
  if (!self || !current(epoch))
    return false;
  if (id.isEmpty()) {
    finish("failed", "Cannot send the Cursor request.");
    return false;
  }
  m_calls.insert(id, method);
  if (method == "session/prompt") {
    m_promptId = id;
    refreshDeadline();
  }
  return true;
}
bool CursorHarnessSession::authenticate(const QString &t) {
  if (!m_busy || m_interrupted || t.isEmpty() || t != m_authToken)
    return false;
  m_authToken.clear();
  return call("authenticate", {{"methodId", "cursor_login"}}, 600000);
}
bool CursorHarnessSession::rejectAuthentication(const QString &t) {
  if (!m_busy || t.isEmpty() || t != m_authToken)
    return false;
  finish("interrupted");
  return true;
}
void CursorHarnessSession::beginSession() {
  if (!m_busy || m_interrupted)
    return;
  if (!m_session.isEmpty()) {
    configure();
    return;
  }
  QJsonObject p{{"cwd", m_workspace.path}, {"mcpServers", QJsonArray{}}};
  const auto old = identity(m_saved["sessionId"]);
  if (!old.isEmpty() && m_capabilities["loadSession"].toBool()) {
    p["sessionId"] = old;
    call("session/load", p);
  } else {
    m_new = true;
    call("session/new", p);
  }
}
QJsonObject CursorHarnessSession::sessionState() const {
  if (m_session.isEmpty())
    return {};
  return {{"version", 1},
          {"kind", "cursor"},
          {"providerId", m_options.providerId},
          {"ownerId", m_options.ownerId},
          {"executable", m_executable.program},
          {"executableIdentity", m_executable.identity},
          {"workspace", m_workspace.path},
          {"workspaceIdentity", m_workspace.identity},
          {"alwaysAllow", m_options.alwaysAllow},
          {"sessionId", m_session}};
}
void CursorHarnessSession::configure() {
  if (!m_busy || m_interrupted)
    return;
  bool hasModel = false, hasMode = false;
  for (const auto &v : m_config) {
    const auto o = v.toObject();
    const auto id = identity(o["id"]);
    const auto category = o["category"].toString();
    QString wanted;
    if (category == "model" || id == "model") {
      hasModel = true;
      wanted = m_options.model;
    } else if (category == "mode" || id == "mode") {
      hasMode = true;
      wanted = m_options.mode;
    } else
      continue;
    if (wanted.isEmpty() || wanted == "default" || m_configured.contains(id))
      continue;
    if (o["type"] != "select" || id.isEmpty()) {
      finish("failed", "Cursor supplied an unsupported model or mode control.");
      return;
    }
    bool found = false;
    for (const auto &c : choices(o))
      found |= c.toObject()["value"] == wanted;
    if (!found) {
      finish("failed", "The selected Cursor model or mode is unavailable.");
      return;
    }
    m_configured.insert(id);
    if (o["currentValue"] == wanted)
      continue;
    call("session/set_config_option",
         {{"sessionId", m_session}, {"configId", id}, {"value", wanted}});
    return;
  }
  if (!hasModel && !m_options.model.isEmpty() && m_options.model != "default") {
    finish("failed", "Cursor did not advertise the selected model.");
    return;
  }
  if (!hasMode && m_options.mode != "agent" &&
      !m_configured.contains("@mode")) {
    m_configured.insert("@mode");
    call("session/set_mode",
         {{"sessionId", m_session}, {"modeId", m_options.mode}});
    return;
  }
  prompt();
}
void CursorHarnessSession::cancelPreparation() {
  if (m_cancelPreparation)
    m_cancelPreparation->store(true);
  m_cancelPreparation.reset();
  if (m_preparing) {
    m_preparing->disconnect(this);
    m_preparing->deleteLater();
  }
  m_preparing = nullptr;
}
void CursorHarnessSession::prompt() {
  if (!m_busy || m_interrupted || m_promptSent || m_preparing)
    return;
  QJsonArray prefix;
  if (!m_options.instructions.isEmpty())
    prefix.append(
        QJsonObject{{"type", "text"}, {"text", m_options.instructions}});
  if (m_new && !m_options.historyContext.isEmpty())
    prefix.append(
        QJsonObject{{"type", "text"}, {"text", m_options.historyContext}});
  const auto attachments = m_options.attachments;
  const auto input = m_input;
  const auto session = m_session;
  const bool
      image = m_capabilities["promptCapabilities"].toObject()["image"].toBool(),
      created = m_new;
  if (!image) {
    for (const auto &part : input) {
      if (part.toObject()["type"] == "image") {
        finish("failed", "Cursor did not advertise image input support.");
        return;
      }
    }
  }
  if (!attachments.acpOriginalImages) {
    QString error;
    const auto mapped =
        mapHarnessAttachments(input, attachments, true, image, created, &error);
    if (!error.isEmpty()) {
      finish("failed", error);
      return;
    }
    for (const auto &part : mapped)
      prefix.append(part);
    if (QJsonDocument(prefix).toJson(QJsonDocument::Compact).size() > Maximum) {
      finish("failed", "Cursor prompt exceeded its text and fallback limit.");
      return;
    }
    m_promptSent = true;
    call("session/prompt", {{"sessionId", m_session}, {"prompt", prefix}},
         m_options.promptTimeoutMs);
    return;
  }
  auto cancelled = std::make_shared<std::atomic_bool>(false);
  m_cancelPreparation = cancelled;
  auto *w = new QFutureWatcher<Prepared>(this);
  m_preparing = w;
  const auto epoch = m_epoch;
  m_deadline.start(m_remaining);
  connect(w, &QFutureWatcher<Prepared>::finished, this,
          [this, w, cancelled, epoch] {
            if (m_preparing != w) {
              w->deleteLater();
              return;
            }
            m_preparing = nullptr;
            m_cancelPreparation.reset();
            const auto result = w->result();
            w->deleteLater();
            if (cancelled->load() || !current(epoch) || m_interrupted)
              return;
            if (!result.error.isEmpty()) {
              finish("failed", result.error);
              return;
            }
            sendPrompt(result.payload);
          });
  w->setFuture(QtConcurrent::run([session, prefix, input, attachments, image,
                                  created, cancelled] {
    Prepared p;
    try {
      p.payload = prepareAcpPrompt(session, prefix, input, attachments, image,
                                   created, cancelled.get(), &p.error);
    } catch (...) {
      p.error = "Cursor prompt preparation failed.";
    }
    return p;
  }));
}
void CursorHarnessSession::sendPrompt(const AcpPromptPayload &p) {
  if (!m_busy || m_interrupted || m_promptSent)
    return;
  if (!p.valid()) {
    finish("failed", "Captured Cursor attachments became unavailable.");
    return;
  }
  m_promptSent = true;
  const QPointer<CursorHarnessSession> self(this);
  const auto epoch = m_epoch;
  const auto id = m_process.requestAcpPrompt(p, m_options.promptTimeoutMs);
  if (!self || !current(epoch))
    return;
  if (id.isEmpty()) {
    finish("failed", "Cannot send the Cursor prompt.");
    return;
  }
  m_calls.insert(id, "session/prompt");
  m_promptId = id;
  refreshDeadline();
}
void CursorHarnessSession::response(const QString &id, const QJsonValue &value,
                                    const QJsonObject &error) {
  if (!m_busy)
    return;
  const auto method = m_calls.take(id);
  if (method.isEmpty())
    return;
  if (id == m_promptId)
    m_promptId.clear();
  if (!error.isEmpty()) {
    // Cursor load failures preserve ADE history; no speculative automatic fork.
    finish(
        m_interrupted ? "interrupted" : "failed",
        m_interrupted
            ? QString{}
            : QString("Cursor rejected %1. Existing ADE history was preserved.")
                  .arg(method));
    return;
  }
  const auto o = value.toObject();
  if (method == "initialize") {
    if (o["protocolVersion"].toInt() != 1) {
      finish("failed", "Cursor did not negotiate ACP v1.");
      return;
    }
    m_capabilities = o["agentCapabilities"].toObject();
    bool found = false;
    for (const auto &m : o["authMethods"].toArray())
      found |= m.toObject()["id"] == "cursor_login";
    if (!found) {
      finish("failed",
             "Cursor did not advertise its managed authentication method.");
      return;
    }
    m_authToken = token();
    emit authenticationRequested(m_authToken, o["authMethods"].toArray());
  } else if (method == "authenticate") {
    m_authenticated = true;
    beginSession();
  } else if (method == "session/new" || method == "session/load") {
    m_session = method == "session/new" ? identity(o["sessionId"])
                                        : identity(m_saved["sessionId"]);
    if (m_session.isEmpty()) {
      finish("failed", "Invalid Cursor session identity.");
      return;
    }
    m_config = o["configOptions"].toArray();
    const QPointer<CursorHarnessSession> self(this);
    const auto epoch = m_epoch;
    emit sessionStateChanged(sessionState());
    if (self && current(epoch))
      configure();
  } else if (method == "session/set_config_option") {
    m_config = o["configOptions"].toArray();
    configure();
  } else if (method == "session/set_mode")
    configure();
  else if (method == "session/prompt") {
    const auto reason = o["stopReason"].toString();
    if (m_interrupted || reason == "cancelled") {
      finish("interrupted");
      return;
    }
    if (!QStringList{"end_turn", "max_tokens", "max_turn_requests", "refusal"}
             .contains(reason)) {
      finish("failed", "Invalid Cursor prompt stop reason.");
      return;
    }
    m_promptReturned = true;
    refreshDeadline();
    m_settle.start(100);
  }
}
bool CursorHarnessSession::publish(QJsonObject e) {
  e["turnId"] = m_turn;
  const auto size = QJsonDocument(e).toJson(QJsonDocument::Compact).size();
  if (size > Maximum - m_bytes || m_events.size() >= 20000) {
    finish("failed", "Cursor output exceeded its limit.");
    return false;
  }
  m_bytes += size;
  m_events.append(e);
  const QPointer<CursorHarnessSession> self(this);
  const auto epoch = m_epoch;
  emit event(e);
  return self && current(epoch) && !m_interrupted;
}
void CursorHarnessSession::notification(const QString &method,
                                        const QJsonObject &p) {
  if (!m_busy || m_interrupted || !m_promptSent)
    return;
  if (method.startsWith("cursor/")) {
    if (p.contains("sessionId") && p["sessionId"] != m_session)
      return;
    if (QStringList{"cursor/update_todos", "cursor/task",
                    "cursor/generate_image"}
            .contains(method))
      publish({{"type", "cursorExtension"},
               {"method", method},
               {"id", identity(p["toolCallId"])},
               {"update", p}});
    return;
  }
  if (method != "session/update" || p["sessionId"] != m_session)
    return;
  const auto u = p["update"].toObject();
  const auto kind = u["sessionUpdate"].toString();
  if (kind == "agent_message_chunk" || kind == "agent_thought_chunk") {
    const auto c = u["content"].toObject();
    const auto type =
        kind == "agent_message_chunk" ? QString("text") : QString("reasoning");
    if (c["type"] == "text") {
      if (m_partType != type) {
        m_partType = type;
        m_part = "cursor-part-" + QString::number(++m_partNumber);
      }
      publish({{"type", type}, {"id", m_part}, {"text", c["text"]}});
    } else if (c["type"] == "image")
      publish({{"type", "image"},
               {"id", "cursor-part-" + QString::number(++m_partNumber)},
               {"content", c}});
  } else if (kind == "tool_call" || kind == "tool_call_update") {
    const auto id = identity(u["toolCallId"]);
    if (id.isEmpty())
      return;
    m_partType.clear();
    auto merged = m_tools.value(id);
    for (auto i = u.begin(); i != u.end(); ++i)
      merged[i.key()] = i.value();
    m_tools[id] = merged;
    if (publish({{"type", "tool"}, {"id", id}, {"update", merged}}) &&
        m_promptReturned)
      m_settle.start(100);
  } else if (kind == "plan")
    publish({{"type", "plan"}, {"update", u}});
  else if (kind == "config_option_update")
    m_config = u["configOptions"].toArray();
  else if (kind == "usage_update") {
    m_usage = u;
    publish({{"type", "usage"}, {"usage", u}});
  }
}
void CursorHarnessSession::request(const QJsonValue &id, const QString &method,
                                   const QJsonObject &p) {
  const bool extension =
      method == "cursor/ask_question" || method == "cursor/create_plan";
  if (!m_busy || m_interrupted || !m_promptSent ||
      (p.contains("sessionId") ? p["sessionId"] != m_session : !extension)) {
    m_process.reject(id, -32602, "No active owning Cursor session.");
    return;
  }
  if (m_pending.size() >= 50 ||
      QJsonDocument(p).toJson(QJsonDocument::Compact).size() > 256 * 1024) {
    m_process.reject(id, -32602, "Cursor request exceeded its limit.");
    return;
  }
  for (const auto &v : m_pending)
    if (v.id == id) {
      m_process.reject(id, -32600, "Duplicate pending Cursor request.");
      return;
    }
  const auto t = token();
  if (method == "session/request_permission") {
    const auto toolId = identity(p["toolCall"].toObject()["toolCallId"]);
    if (toolId.isEmpty() || !p["options"].isArray() ||
        p["options"].toArray().isEmpty() ||
        p["options"].toArray().size() > 32) {
      m_process.reject(id, -32602, "Invalid Cursor permission options.");
      return;
    }
    for (const auto &v : m_pending)
      if (v.method == method &&
          v.params["toolCall"].toObject()["toolCallId"] == toolId) {
        m_process.reject(id, -32600, "Permission already pending.");
        return;
      }
    if (m_options.alwaysAllow) {
      m_process.respond(id, permission(p, true));
      return;
    }
    m_pending.insert(t, {id, method, p});
    refreshDeadline();
    emit approvalRequired(t, p);
  } else if (method == "cursor/ask_question") {
    const auto form = cursorQuestionForm(p);
    if (form.isEmpty()) {
      m_process.reject(id, -32602, "Invalid Cursor question.");
      return;
    }
    for (const auto &v : m_pending)
      if (v.method == method) {
        m_process.respond(id, cancelled());
        return;
      }
    m_pending.insert(t, {id, method, p});
    refreshDeadline();
    emit questionRequired(t, form);
  } else if (method == "cursor/create_plan") {
    if (!validCursorPlan(p)) {
      m_process.reject(id, -32602, "Invalid Cursor plan.");
      return;
    }
    for (const auto &v : m_pending)
      if (v.method == method) {
        m_process.respond(id, cancelled());
        return;
      }
    m_pending.insert(t, {id, method, p});
    refreshDeadline();
    emit planRequired(t, p);
  } else
    m_process.reject(id, -32601, "Unsupported Cursor client request.");
}
bool CursorHarnessSession::reply(const QString &t, const QString &method,
                                 const QJsonObject &r) {
  const auto it = m_pending.constFind(t);
  if (!m_busy || m_interrupted || it == m_pending.cend() ||
      it->method != method)
    return false;
  const auto p = *it;
  m_pending.remove(t);
  refreshDeadline();
  const QPointer<CursorHarnessSession> self(this);
  const auto epoch = m_epoch;
  const bool ok = m_process.respond(p.id, r);
  if (self && current(epoch) && m_promptReturned)
    m_settle.start(100);
  return ok;
}
bool CursorHarnessSession::answerApproval(const QString &t, bool allow) {
  const auto p = m_pending.value(t);
  return reply(t, "session/request_permission", permission(p.params, allow));
}
bool CursorHarnessSession::validQuestionAnswer(const QString &t,
                                               const QJsonObject &a) const {
  const auto found = m_pending.constFind(t);
  return m_busy && !m_interrupted && found != m_pending.cend() &&
         found->method == "cursor/ask_question" &&
         !cursorQuestionResponse(found->params, a).isEmpty();
}
bool CursorHarnessSession::answerQuestion(const QString &t,
                                          const QJsonObject &a) {
  const auto p = m_pending.value(t);
  const auto r = cursorQuestionResponse(p.params, a);
  return !r.isEmpty() && reply(t, "cursor/ask_question", r);
}
bool CursorHarnessSession::cancelQuestion(const QString &t) {
  return reply(t, "cursor/ask_question", cancelled());
}
bool CursorHarnessSession::answerPlan(const QString &t, bool accepted) {
  return reply(t, "cursor/create_plan",
               {{"outcome", QJsonObject{{"outcome", accepted ? "accepted"
                                                             : "rejected"}}}});
}
void CursorHarnessSession::refreshDeadline() {
  const bool paused = !m_pending.isEmpty();
  if (paused) {
    if (m_deadline.isActive()) {
      m_remaining = qMax(1, m_deadline.remainingTime());
      m_deadline.stop();
    }
    if (!m_promptId.isEmpty())
      m_process.suspendDeadline(m_promptId);
  } else if (m_promptSent && !m_interrupted) {
    if (!m_deadline.isActive())
      m_deadline.start(m_remaining);
    if (!m_promptId.isEmpty())
      m_process.resumeDeadline(m_promptId);
  }
}
void CursorHarnessSession::settle() {
  if (!m_busy || !m_promptReturned || !m_pending.isEmpty() || m_interrupted)
    return;
  for (const auto &t : m_tools)
    if (t["status"] != "completed" && t["status"] != "failed")
      return;
  finish("completed");
}
void CursorHarnessSession::interrupt() {
  if (!m_busy || m_interrupted)
    return;
  m_interrupted = true;
  cancelPreparation();
  m_deadline.stop();
  m_settle.stop();
  m_authToken.clear();
  const auto pending = m_pending;
  m_pending.clear();
  const QPointer<CursorHarnessSession> self(this);
  const auto epoch = m_epoch;
  for (const auto &p : pending) {
    m_process.respond(p.id, cancelled());
    if (!self || !current(epoch))
      return;
  }
  if (!m_promptSent || m_session.isEmpty()) {
    finish("interrupted");
    return;
  }
  if (m_process.cancelAcpPromptWrite(m_promptId) !=
      JsonRpcProcess::PromptWriteCancel::NotPending) {
    finish("interrupted");
    return;
  }
  m_process.notify("session/cancel", {{"sessionId", m_session}});
  if (self && current(epoch))
    m_cancel.start(2000);
}
void CursorHarnessSession::finish(const QString &status, const QString &error) {
  if (!m_busy)
    return;
  cancelPreparation();
  m_deadline.stop();
  m_settle.stop();
  m_cancel.stop();
  m_busy = false;
  m_calls.clear();
  m_pending.clear();
  m_authToken.clear();
  m_promptId.clear();
  if (status != "completed") {
    m_process.stop();
    m_authenticated = false;
    m_session.clear();
  }
  const QJsonObject result{{"status", status},
                           {"error", error},
                           {"events", m_events},
                           {"usage", m_usage},
                           {"sessionState", sessionState()}};
  m_options.attachments = {};
  const QPointer<CursorHarnessSession> self(this);
  const auto epoch = m_epoch;
  if (!error.isEmpty()) {
    emit failed(error);
    if (!self || m_epoch != epoch)
      return;
  }
  emit finished(result);
}
void CursorHarnessSession::shutdown() {
  ++m_epoch;
  cancelPreparation();
  m_deadline.stop();
  m_cancel.stop();
  m_settle.stop();
  m_busy = false;
  m_authenticated = false;
  m_interrupted = false;
  m_calls.clear();
  m_pending.clear();
  m_session.clear();
  m_authToken.clear();
  m_promptId.clear();
  m_process.stop();
  m_options.attachments = {};
  m_workspace = {};
}
} // namespace ade
