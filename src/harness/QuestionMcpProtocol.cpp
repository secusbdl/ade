#include "QuestionMcpProtocol.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>
namespace ade {
namespace {
bool validId(const QJsonValue &id) {
  return (id.isString() && !id.toString().isEmpty() &&
          id.toString().size() <= 512) ||
         (id.isDouble() && std::isfinite(id.toDouble()) &&
          std::floor(id.toDouble()) == id.toDouble() &&
          std::abs(id.toDouble()) <= 9007199254740991.);
}
QJsonObject toolResult(const QJsonObject &value) {
  return {
      {"content", QJsonArray{QJsonObject{
                      {"type", "text"},
                      {"text", QString::fromUtf8(QJsonDocument(value).toJson(
                                   QJsonDocument::Compact))}}}},
      {"structuredContent", value}};
}
} // namespace
QuestionMcpProtocol::QuestionMcpProtocol(QJsonObject schema, QObject *parent)
    : QObject(parent), m_schema(std::move(schema)) {}
QJsonObject QuestionMcpProtocol::cancelledResult() {
  return {{"completed", false},
          {"cancelled", true},
          {"answers", QJsonObject{}},
          {"completedSteps", 0}};
}
void QuestionMcpProtocol::result(const QJsonValue &id,
                                 const QJsonValue &value) {
  emit output({{"jsonrpc", "2.0"}, {"id", id}, {"result", value}});
}
void QuestionMcpProtocol::error(const QJsonValue &id, int code,
                                const QString &message) {
  emit output({{"jsonrpc", "2.0"},
               {"id", id},
               {"error", QJsonObject{{"code", code}, {"message", message}}}});
}
void QuestionMcpProtocol::receive(const QJsonObject &message) {
  const QJsonValue id = message.value("id");
  const auto method = message.value("method").toString();
  if (message.value("jsonrpc") != "2.0" || method.isEmpty() ||
      (!id.isUndefined() && !validId(id))) {
    error(id.isUndefined() ? QJsonValue(QJsonValue::Null) : id, -32600,
          "Invalid MCP request.");
    return;
  }
  const auto params = message.value("params").toObject();
  if (id.isUndefined()) {
    if (method == "notifications/initialized" && m_initialized) {
      m_ready = true;
      return;
    }
    if (method == "notifications/cancelled" && m_ready && hasPending() &&
        params.value("requestId") == m_pending)
      cancelPending();
    return;
  }
  if (method == "ping") {
    result(id, QJsonObject{});
    return;
  }
  if (method == "initialize") {
    if (m_initialized) {
      error(id, -32600, "MCP is already initialized.");
      return;
    }
    if (!params.value("protocolVersion").isString()) {
      error(id, -32602, "A protocol version is required.");
      return;
    }
    const auto requested = params.value("protocolVersion").toString();
    const QString version =
        QStringList{"2025-03-26", "2025-06-18", "2025-11-25"}.contains(
            requested)
            ? requested
            : QString("2025-11-25");
    m_initialized = true;
    result(id,
           QJsonObject{{"protocolVersion", version},
                       {"capabilities", QJsonObject{{"tools", QJsonObject{}}}},
                       {"serverInfo", QJsonObject{{"name", "stagewise"},
                                                  {"version", "1.0.0"}}}});
    return;
  }
  if (!m_ready) {
    error(id, -32002, "MCP initialization is incomplete.");
    return;
  }
  if (method == "tools/list") {
    if (params.contains("cursor")) {
      error(id, -32602, "Unknown tool-list cursor.");
      return;
    }
    const QJsonObject tool{
        {"name", "stagewise_request_user_input"},
        {"title", "Ask the user"},
        {"description",
         "Show a native form and wait for the response. Use this only for "
         "structured product questions, never for command or tool approval. "
         "Invoke commands and tools directly because the host owns their "
         "approval UI. Keep forms short and never call it concurrently."},
        {"inputSchema", m_schema},
        {"annotations", QJsonObject{{"readOnlyHint", true},
                                    {"destructiveHint", false},
                                    {"idempotentHint", false},
                                    {"openWorldHint", false}}}};
    result(id, QJsonObject{{"tools", QJsonArray{tool}}});
    return;
  }
  if (method != "tools/call") {
    error(id, -32601, "Unknown MCP method.");
    return;
  }
  if (params.value("name") != "stagewise_request_user_input" ||
      !params.value("arguments").isObject()) {
    error(id, -32602, "Unknown tool or invalid question arguments.");
    return;
  }
  const auto form = params.value("arguments").toObject();
  if (QJsonDocument(form).toJson(QJsonDocument::Compact).size() > 256 * 1024) {
    error(id, -32602, "Question input exceeds its limit.");
    return;
  }
  if (hasPending()) {
    if (id == m_pending)
      error(id, -32600, "Duplicate pending MCP request identity.");
    else
      result(id, toolResult(cancelledResult()));
    return;
  }
  m_pending = id;
  emit questionRequested(id, form);
}
bool QuestionMcpProtocol::complete(const QJsonValue &id,
                                   const QJsonObject &value,
                                   const QString &issue) {
  if (!hasPending() || id != m_pending)
    return false;
  m_pending = QJsonValue::Undefined;
  if (!issue.isEmpty())
    result(id,
           QJsonObject{{"isError", true},
                       {"content",
                        QJsonArray{QJsonObject{{"type", "text"},
                                               {"text", issue.left(8192)}}}}});
  else
    result(id, toolResult(value));
  return true;
}
void QuestionMcpProtocol::cancelPending() {
  if (!hasPending())
    return;
  const auto id = m_pending;
  m_pending = QJsonValue::Undefined;
  emit questionCancelled(id);
}
} // namespace ade
