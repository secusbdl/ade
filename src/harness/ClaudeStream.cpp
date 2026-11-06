#include "ClaudeStream.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <cmath>
namespace ade {
namespace {
constexpr qsizetype MaxChars = 4 * 1024 * 1024;
constexpr qsizetype MaxParts = 20000;
bool fail(QString *error, const QString &text) {
  if (error)
    *error = text;
  return false;
}
bool identifier(const QString &value) {
  return !value.isEmpty() && value.size() <= 512;
}
QString key(const QString &parent, const QString &message, int index) {
  return QString::number(parent.size()) + ":" + parent + message + ":" +
         QString::number(index);
}
QString toolKey(const QString &parent, const QString &id) {
  Q_UNUSED(parent);
  // The native control protocol refers to tool_use_id without a parent lane.
  // Reject duplicate outstanding/observed IDs rather than ambiguously approve.
  return id;
}
QString partId(const QString &key) {
  return QString::fromLatin1(
      QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256)
          .toHex());
}
bool indexValue(const QJsonValue &value, int *index) {
  const double number = value.toDouble(-1);
  if (!value.isDouble() || !std::isfinite(number) || number < 0 ||
      number >= MaxParts || std::floor(number) != number)
    return false;
  *index = int(number);
  return true;
}
QString outputText(const QJsonValue &value, bool *unsupported) {
  if (value.isString())
    return value.toString();
  if (value.isNull() || value.isUndefined())
    return {};
  if (value.isObject())
    return QString::fromUtf8(
        QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
  if (!value.isArray()) {
    *unsupported = true;
    return {};
  }
  QStringList output;
  for (const auto &part : value.toArray()) {
    const auto item = part.toObject();
    if (item.value("type") == "text" && item.value("text").isString())
      output.append(item.value("text").toString());
    else {
      *unsupported = true;
      return {};
    }
  }
  return output.join("\n");
}
} // namespace
void ClaudeStream::reset(const QString &turnId) {
  *this = ClaudeStream{};
  m_turnId = turnId;
}
void ClaudeStream::append(QJsonArray *updates, QJsonObject value) const {
  value["turnId"] = m_turnId;
  updates->append(value);
}
bool ClaudeStream::updateText(Block &block, const QString &text, bool delta,
                              QJsonArray *updates, QString *error) {
  const auto size = delta ? block.text.size() + text.size() : text.size();
  if (m_chars - block.text.size() + size > MaxChars)
    return fail(error, "Claude presentation text exceeded its limit.");
  if (!delta && text == block.text)
    return true;
  m_chars += size - block.text.size();
  if (delta)
    block.text += text;
  else
    block.text = text;
  append(updates, {{"type", block.type + (delta ? "" : "Snapshot")},
                   {"itemId", block.id},
                   {"text", text}});
  return true;
}
void ClaudeStream::tool(Block &block, QJsonArray *updates) {
  if (block.hidden)
    return;
  QJsonObject item{{"type", "tool"},
                   {"itemId", block.id},
                   {"name", "claude." + block.name},
                   {"arguments", block.arguments},
                   {"state", block.state},
                   {"output", block.text}};
  if (block.state == "input-streaming")
    item["argumentText"] = block.argumentText;
  append(updates, item);
}
void ClaudeStream::close(Block &block, const QString &state,
                         QJsonArray *updates) {
  if (block.closed || block.hidden)
    return;
  block.closed = true;
  if (block.type == "text" || block.type == "reasoning") {
    block.state = state;
    append(updates,
           {{"type", "partState"}, {"itemId", block.id}, {"state", state}});
  }
}
bool ClaudeStream::applyBlock(const QString &parent, const QString &message,
                              int index, const QJsonObject &value, bool final,
                              QJsonArray *updates, QString *error) {
  const auto type = value.value("type").toString();
  const auto id = key(parent, message, index);
  if (!final && m_blocks.contains(id))
    return fail(error, "Claude restarted an existing content block.");
  if (!m_blocks.contains(id)) {
    if (m_blocks.size() >= MaxParts)
      return fail(error, "Claude presentation exceeded its part limit.");
    Block block;
    block.parent = parent;
    block.message = message;
    block.type =
        type == "thinking" || type == "redacted_thinking" ? "reasoning" : type;
    block.hidden = type == "redacted_thinking";
    block.id = "part/" + partId(id);
    if (type == "tool_use" || type == "server_tool_use") {
      const auto toolId = value.value("id").toString(),
                 name = value.value("name").toString();
      if (!identifier(toolId) || !identifier(name) ||
          !value.value("input").isObject())
        return fail(error, "Claude returned an invalid tool block.");
      const auto owned = toolKey(parent, toolId);
      if (m_tools.contains(owned))
        return fail(error, "Claude reused a tool ID in this turn.");
      m_tools.insert(owned, id);
      block.type = "tool";
      block.name = name;
      block.id = "tool/" + owned;
      block.hidden =
          QStringList{"EnterPlanMode", "ExitPlanMode", "ToolSearch"}.contains(
              name);
      block.state = final ? "input-available" : "input-streaming";
    } else if (block.type != "text" && block.type != "reasoning") {
      return fail(error, "Claude returned unsupported assistant content.");
    }
    m_blocks.insert(id, block);
    m_order.append(id);
    if (!block.hidden && block.type != "tool")
      append(updates,
             {{"type", block.type}, {"itemId", block.id}, {"text", ""}});
  }
  auto &block = m_blocks[id];
  const auto expected =
      type == "thinking" || type == "redacted_thinking"
          ? "reasoning"
          : (type == "tool_use" || type == "server_tool_use" ? "tool" : type);
  if (block.type != expected)
    return fail(error, "Claude changed a streamed content block type.");
  if (block.type == "tool") {
    if (block.id != "tool/" + toolKey(parent, value.value("id").toString()) ||
        block.name != value.value("name").toString() ||
        !value.value("input").isObject())
      return fail(error, "Claude changed a streamed tool identity.");
    const auto args = value.value("input").toObject();
    const auto text =
        QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    if (m_chars - block.argumentText.size() + text.size() > MaxChars)
      return fail(error, "Claude tool input exceeded its limit.");
    const bool changed =
        args != block.arguments || block.state == "input-streaming";
    m_chars += text.size() - block.argumentText.size();
    block.argumentText = text;
    block.arguments = args;
    if (final && !block.terminal)
      block.state = "input-available";
    if (changed || !final)
      tool(block, updates);
  } else if (!block.hidden) {
    const auto field = block.type == "text" ? "text" : "thinking";
    if (!value.value(field).isString())
      return fail(error, "Claude returned invalid text content.");
    if (!updateText(block, value.value(field).toString(), false, updates,
                    error))
      return false;
  }
  if (final)
    close(block, "done", updates);
  return true;
}
bool ClaudeStream::consume(const QJsonObject &frame, QJsonArray *updates,
                           QString *error) {
  const auto parent = frame.value("parent_tool_use_id").toString();
  if (parent.size() > 512)
    return fail(error, "Claude returned an invalid parent tool ID.");
  const auto type = frame.value("type").toString();
  if (type == "stream_event") {
    const auto event = frame.value("event").toObject();
    const auto eventType = event.value("type").toString();
    if (eventType == "message_start") {
      const auto message =
          event.value("message").toObject().value("id").toString();
      if (!identifier(message) ||
          (!m_current.contains(parent) && m_current.size() >= 128))
        return fail(error,
                    "Claude returned an invalid streamed message identity.");
      m_current[parent] = message;
      return true;
    }
    if (eventType == "message_delta" || eventType == "ping")
      return true;
    const auto message = m_current.value(parent);
    if (message.isEmpty())
      return fail(error, "Claude streamed content without a message identity.");
    if (eventType == "message_stop") {
      for (const auto &id : m_order)
        if (m_blocks[id].parent == parent && m_blocks[id].message == message)
          close(m_blocks[id], "done", updates);
      return true;
    }
    int index = -1;
    if (!indexValue(event.value("index"), &index))
      return fail(error, "Claude returned an invalid block index.");
    const auto id = key(parent, message, index);
    if (eventType == "content_block_start")
      return applyBlock(parent, message, index,
                        event.value("content_block").toObject(), false, updates,
                        error);
    if (!m_blocks.contains(id))
      return fail(error, "Claude updated an unknown content block.");
    auto &block = m_blocks[id];
    if (eventType == "content_block_stop") {
      if (block.type == "tool" && !block.terminal) {
        QJsonParseError parse;
        const auto input =
            QJsonDocument::fromJson(block.argumentText.toUtf8(), &parse);
        if (parse.error != QJsonParseError::NoError || !input.isObject())
          return fail(error, "Claude completed malformed tool arguments.");
        block.arguments = input.object();
        block.state = "input-available";
        tool(block, updates);
      }
      close(block, "done", updates);
      return true;
    }
    if (eventType != "content_block_delta")
      return fail(error, "Claude returned an unsupported streaming event.");
    if (block.closed)
      return fail(error, "Claude updated a completed content block.");
    const auto delta = event.value("delta").toObject();
    const auto deltaType = delta.value("type").toString();
    if (deltaType == "signature_delta")
      return true;
    if ((deltaType == "text_delta" && block.type == "text") ||
        (deltaType == "thinking_delta" && block.type == "reasoning")) {
      const auto value =
          delta.value(deltaType == "text_delta" ? "text" : "thinking");
      if (!value.isString())
        return fail(error, "Claude returned an invalid text delta.");
      return updateText(block, value.toString(), true, updates, error);
    }
    if (deltaType == "input_json_delta" && block.type == "tool") {
      const auto value = delta.value("partial_json");
      if (!value.isString() || m_chars + value.toString().size() > MaxChars)
        return fail(error, "Claude streamed tool input exceeded its limit.");
      // The initial {} is a placeholder; actual partial JSON starts separately.
      if (!block.inputStarted) {
        m_chars -= block.argumentText.size();
        block.argumentText.clear();
        block.inputStarted = true;
      }
      block.argumentText += value.toString();
      m_chars += value.toString().size();
      QJsonParseError parse;
      const auto doc =
          QJsonDocument::fromJson(block.argumentText.toUtf8(), &parse);
      if (parse.error == QJsonParseError::NoError && doc.isObject())
        block.arguments = doc.object();
      tool(block, updates);
      return true;
    }
    return fail(error, "Claude returned an unsupported content delta.");
  }
  if (type == "result" && frame.value("subtype") == "success" &&
      !frame.value("is_error").toBool()) {
    bool hasText = false;
    for (const auto &block : m_blocks)
      hasText |= block.type == "text" && block.parent.isEmpty() &&
                 !block.text.isEmpty();
    const auto result = frame.value("result");
    if (!hasText && result.isString() && !result.toString().isEmpty())
      return applyBlock({}, "ade-result-fallback", 0,
                        {{"type", "text"}, {"text", result}}, true, updates,
                        error);
  } else if (type == "assistant") {
    const auto message = frame.value("message").toObject();
    const auto id = message.value("id").toString();
    if (!identifier(id) || !message.value("content").isArray() ||
        message.value("content").toArray().size() > MaxParts)
      return fail(error, "Claude returned an invalid assistant message.");
    const auto content = message.value("content").toArray();
    for (qsizetype i = 0; i < content.size(); ++i)
      if (!applyBlock(parent, id, int(i), content[i].toObject(), true, updates,
                      error))
        return false;
  } else if (type == "user") {
    const auto content = frame.value("message").toObject().value("content");
    if (!content.isArray())
      return true;
    for (const auto &value : content.toArray()) {
      const auto result = value.toObject();
      if (result.value("type") != "tool_result")
        continue;
      const auto owned =
          toolKey(parent, result.value("tool_use_id").toString());
      if (!m_tools.contains(owned))
        return fail(error, "Claude returned an unassociated tool result.");
      auto &block = m_blocks[m_tools.value(owned)];
      bool unsupported = false;
      const auto text = outputText(result.value("content"), &unsupported);
      if (unsupported)
        return fail(error, "Claude tool output contains unsupported media.");
      if (m_chars - block.text.size() + text.size() > MaxChars)
        return fail(error, "Claude tool output exceeded its limit.");
      const auto state = result.value("is_error").toBool() ? "output-error"
                                                           : "output-available";
      if (block.terminal && block.state == state && block.text == text)
        continue;
      if (block.terminal)
        return fail(error, "Claude changed a completed tool result.");
      m_chars += text.size() - block.text.size();
      block.text = text;
      block.state = state;
      block.terminal = true;
      tool(block, updates);
    }
  }
  return true;
}
QJsonArray ClaudeStream::finish(const QString &status) {
  QJsonArray updates;
  for (const auto &id : m_order) {
    auto &block = m_blocks[id];
    close(block,
          status == "completed"
              ? "done"
              : (status == "interrupted" ? "canceled" : "error"),
          &updates);
    if (block.type == "tool" && !block.terminal) {
      block.terminal = true;
      block.state = status == "interrupted" ? "canceled" : "output-error";
      block.text = "The Claude turn ended before this tool completed.";
      tool(block, &updates);
    }
  }
  return updates;
}
QJsonArray ClaudeStream::snapshot() const {
  QJsonArray updates;
  for (const auto &id : m_order) {
    const auto &block = m_blocks[id];
    if (block.hidden)
      continue;
    if (block.type == "tool") {
      QJsonObject value{{"type", "tool"},
                        {"itemId", block.id},
                        {"name", "claude." + block.name},
                        {"arguments", block.arguments},
                        {"state", block.state},
                        {"output", block.text}};
      if (block.state == "input-streaming")
        value["argumentText"] = block.argumentText;
      append(&updates, value);
    } else {
      append(&updates, {{"type", block.type + "Snapshot"},
                        {"itemId", block.id},
                        {"text", block.text}});
      if (block.closed)
        append(&updates,
               {{"type", "partState"},
                {"itemId", block.id},
                {"state", block.state.isEmpty() ? "done" : block.state}});
    }
  }
  return updates;
}
} // namespace ade
