#include "CodexAppServer.h"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace ade {
CodexAppServer::CodexAppServer(QObject *parent) : QObject(parent), m_rpc(this) {
  connect(&m_rpc, &JsonRpcProcess::started, this, [this] {
    call(
        "initialize",
        {{"clientInfo",
          QJsonObject{{"name", "ade"}, {"title", "ADE"}, {"version", "0.1.0"}}},
         {"capabilities", QJsonObject{{"experimentalApi", false}}}});
  });
  connect(&m_rpc, &JsonRpcProcess::response, this,
          &CodexAppServer::receiveReply);
  connect(&m_rpc, &JsonRpcProcess::notification, this,
          &CodexAppServer::receiveNotification);
  connect(&m_rpc, &JsonRpcProcess::serverRequest, this,
          &CodexAppServer::serverRequest);
  connect(&m_rpc, &JsonRpcProcess::failed, this, [this](const QString &error) {
    ++m_generation;
    m_methods.clear();
    m_ready = false;
    m_authenticated = false;
    m_discovering = false;
    m_loginId.clear();
    m_loginRequest.clear();
    emit failed(error);
  });
}
CodexAppServer::~CodexAppServer() { stop(); }
QString CodexAppServer::findExecutable(const QString &configured) {
  if (!configured.isEmpty()) {
    const QFileInfo info(configured);
    if (!info.isAbsolute() || !info.isFile() || !info.isExecutable())
      return {};
#ifdef Q_OS_WIN
    if (info.suffix().compare("exe", Qt::CaseInsensitive) != 0)
      return {};
#endif
    return info.canonicalFilePath();
  }
  auto result = QStandardPaths::findExecutable("codex");
#ifdef Q_OS_WIN
  if (result.isEmpty()) {
    const auto local = qEnvironmentVariable("LOCALAPPDATA");
    if (!local.isEmpty())
      result = QDir(local).filePath("Programs/OpenAI/Codex/bin/codex.exe");
  }
#endif
  return result.isEmpty() ? QString() : findExecutable(result);
}
bool CodexAppServer::start(const QString &executable,
                           const QString &directory) {
  stop();
  m_executable = findExecutable(executable);
  return m_rpc.start(m_executable, {"app-server", "--stdio"}, directory,
                     JsonRpcProcess::managedEnvironment());
}
void CodexAppServer::stop() {
  ++m_generation;
  m_rpc.stop();
  m_methods.clear();
  m_ready = false;
  m_authenticated = false;
  m_discovering = false;
  m_loginId.clear();
  m_loginRequest.clear();
  m_models = {};
  m_modelIds.clear();
  m_cursors.clear();
}
bool CodexAppServer::validLoginUrl(const QUrl &url) {
  return url.isValid() && url.scheme() == "https" && url.userInfo().isEmpty() &&
         (url.port(-1) == -1 || url.port() == 443) &&
         (url.host().compare("auth.openai.com", Qt::CaseInsensitive) == 0 ||
          url.host().compare("auth0.openai.com", Qt::CaseInsensitive) == 0);
}
QString CodexAppServer::call(const QString &method, const QJsonObject &params,
                             int timeoutMs) {
  if (method != "initialize" && !m_ready)
    return {};
  const auto id = m_rpc.request(method, params, timeoutMs);
  if (!id.isEmpty())
    m_methods.insert(id, method);
  return id;
}
bool CodexAppServer::respond(const QJsonValue &id, const QJsonValue &result) {
  return m_rpc.respond(id, result);
}
bool CodexAppServer::reject(const QJsonValue &id, int code,
                            const QString &message) {
  return m_rpc.reject(id, code, message);
}
void CodexAppServer::readAccount() {
  call("account/read", {{"refreshToken", false}});
}
void CodexAppServer::startLogin() {
  if (!m_ready || !m_loginId.isEmpty() || !m_loginRequest.isEmpty())
    return;
  m_loginRequest = call("account/login/start", {{"type", "chatgpt"}}, 60000);
}
void CodexAppServer::cancelLogin() {
  // A pending start is canceled once its loginId arrives; never invalidate a
  // replacement login or call account/logout on the user's shared CLI.
  if (!m_loginRequest.isEmpty()) {
    if (!m_loginRequest.startsWith("cancel:"))
      m_loginRequest.prepend("cancel:");
    return;
  }
  if (m_loginId.isEmpty())
    return;
  const auto id = m_loginId;
  m_loginId.clear();
  call("account/login/cancel", {{"loginId", id}});
  emit loginFinished(false, "Sign-in canceled.");
}
void CodexAppServer::discoverModels() {
  if (!m_ready || m_discovering)
    return;
  m_models = {};
  m_modelIds.clear();
  m_cursors.clear();
  m_pages = 0;
  m_discovering = true;
  modelPage();
}
void CodexAppServer::modelPage(const QString &cursor) {
  if (++m_pages > 50) {
    m_discovering = false;
    emit failed("Codex model discovery exceeded its page limit.");
    return;
  }
  QJsonObject args{{"limit", 100}, {"includeHidden", false}};
  if (!cursor.isEmpty())
    args["cursor"] = cursor;
  if (call("model/list", args).isEmpty()) {
    m_discovering = false;
    emit failed("Codex model discovery could not start.");
  }
}
void CodexAppServer::readRateLimits() { call("account/rateLimits/read", {}); }
void CodexAppServer::receiveReply(const QString &id, const QJsonValue &result,
                                  const QJsonObject &error) {
  const auto method = m_methods.take(id);
  if (method.isEmpty())
    return;
  const auto generation = m_generation;
  QPointer<CodexAppServer> self(this);
  if (!error.isEmpty()) {
    if (method == "account/login/start") {
      m_loginRequest.clear();
      emit loginFinished(false, "Codex sign-in could not start.");
    } else if (method == "initialize") {
      stop();
      emit failed(
          "Codex initialization failed. Check the installed CLI version.");
      return;
    } else if (method == "model/list") {
      m_discovering = false;
      emit failed("Codex model discovery failed. Connect your ChatGPT account "
                  "and retry.");
    } else if (method == "account/read") {
      m_authenticated = false;
      emit failed("Codex could not read its managed account. Reconnect in "
                  "Models & Providers.");
    }
    if (!self || generation != m_generation)
      return;
    // Do not forward raw CLI error messages: they may contain login URLs,
    // account identifiers, prompt fragments or environment values.
    emit reply(
        id, method, {},
        {{"code", error.value("code")}, {"message", "Codex request failed."}});
    return;
  }
  const auto object = result.toObject();
  if (method == "initialize") {
    if (!result.isObject() || !m_rpc.notify("initialized")) {
      stop();
      emit failed("Codex initialization returned an invalid response.");
      return;
    }
    m_ready = true;
    emit ready();
    return;
  }
  if (method == "account/read") {
    const auto account = object.value("account").toObject();
    m_authenticated = account.value("type").toString() == "chatgpt";
    emit accountChanged(m_authenticated,
                        m_authenticated
                            ? account.value("planType").toString().left(100)
                            : QString());
  } else if (method == "account/login/start") {
    const bool canceled = m_loginRequest == "cancel:" + id;
    m_loginRequest.clear();
    const auto loginId = object.value("loginId").toString();
    const QUrl url(object.value("authUrl").toString());
    if (loginId.isEmpty() || loginId.size() > 256 || !validLoginUrl(url)) {
      emit loginFinished(false,
                         "Codex returned an unsupported sign-in response.");
      return;
    }
    m_loginId = loginId;
    if (canceled) {
      cancelLogin();
      return;
    }
    emit loginUrlReady(url);
  } else if (method == "model/list") {
    if (!m_discovering)
      return;
    if (!object.value("data").isArray()) {
      m_discovering = false;
      emit failed("Codex returned an invalid model catalog.");
      return;
    }
    for (const auto &value : object.value("data").toArray()) {
      const auto model = value.toObject();
      const auto modelId =
          model.value("model").toString(model.value("id").toString());
      if (modelId.isEmpty() || modelId.size() > 512 ||
          m_modelIds.contains(modelId))
        continue;
      QJsonArray efforts;
      for (const auto &option :
           model.value("supportedReasoningEfforts").toArray()) {
        const auto effort =
            option.toObject().value("reasoningEffort").toString();
        if (!effort.isEmpty() && effort.size() <= 32 &&
            !efforts.contains(effort))
          efforts.append(effort);
      }
      m_modelIds.insert(modelId);
      m_models.append(QJsonObject{
          {"id", modelId},
          {"name", model.value("displayName").toString(modelId).left(512)},
          {"reasoningEfforts", efforts},
          {"defaultReasoningEffort",
           model.value("defaultReasoningEffort").toString().left(32)},
          {"isDefault", model.value("isDefault").toBool()},
          {"inputModalities", model.value("inputModalities").toArray()}});
      if (m_models.size() >= 10000) {
        m_discovering = false;
        emit failed("Codex model catalog exceeded its entry limit.");
        return;
      }
    }
    const auto cursor = object.value("nextCursor").toString();
    if (!cursor.isEmpty()) {
      if (cursor.size() > 1024 || m_cursors.contains(cursor)) {
        m_discovering = false;
        emit failed("Codex model catalog repeated a cursor.");
        return;
      }
      m_cursors.insert(cursor);
      modelPage(cursor);
      return;
    }
    m_discovering = false;
    emit modelsReady(m_models);
  } else if (method == "account/rateLimits/read") {
    // Rate limits are retained only as this bounded public result, not raw
    // authentication/account responses.
    emit rateLimitsReady(object.value("rateLimits").toObject());
  }
  if (!self || generation != m_generation)
    return;
  if (!method.startsWith("account/") && method != "model/list")
    emit reply(id, method, result, {});
}
void CodexAppServer::receiveNotification(const QString &method,
                                         const QJsonObject &params) {
  if (method == "account/login/completed") {
    if (m_loginId.isEmpty() || params.value("loginId").toString() != m_loginId)
      return;
    m_loginId.clear();
    const bool success = params.value("success").toBool();
    QPointer<CodexAppServer> self(this);
    const auto generation = m_generation;
    emit loginFinished(success, success ? "Codex sign-in completed."
                                        : "Codex sign-in did not complete.");
    if (self && generation == m_generation && success)
      readAccount();
  } else if (method == "account/updated")
    readAccount();
  else if (method == "account/rateLimits/updated")
    emit rateLimitsReady(params.value("rateLimits").toObject());
  else if (!method.startsWith("account/"))
    emit notification(method, params);
}
} // namespace ade
