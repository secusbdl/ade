#include "HarnessQuestionMcp.h"
#include "QuestionMcpProtocol.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalBlocker>
#include <cmath>
namespace ade {
HarnessQuestionMcp::HarnessQuestionMcp(QObject *parent)
    : QObject(parent), m_bridge(this) {
  connect(&m_bridge, &QuestionMcpBridge::questionRequested, this,
          [this](const QString &token, const QString &turn,
                 const QJsonObject &form) {
            if (m_turn.isEmpty() || turn != m_turn || m_blocked || pending()) {
              m_bridge.complete(token, QuestionMcpProtocol::cancelledResult());
              return;
            }
            m_token = token;
            m_pendingTurn = turn;
            m_form = form;
            const auto epoch = m_epoch;
            const QPointer<HarnessQuestionMcp> self(this);
            emit pendingChanged(true);
            if (!self || epoch != m_epoch || token != m_token)
              return;
            emit event(presentation(token, turn, form, {}));
            if (!self || epoch != m_epoch || token != m_token)
              return;
            emit required(token, form);
          });
  connect(&m_bridge, &QuestionMcpBridge::questionCancelled, this,
          &HarnessQuestionMcp::cancelOwned);
}
QJsonObject HarnessQuestionMcp::configure(const QString &executable,
                                          QString *error) {
  if (executable != m_executable)
    revoke();
  m_executable = executable;
  const auto config = m_bridge.start(executable, error);
  if (config.isEmpty())
    return {};
  QJsonObject env;
  for (const auto &value : config.value("env").toArray()) {
    const auto variable = value.toObject();
    env[variable.value("name").toString()] = variable.value("value");
  }
  return {{"command", config.value("command")},
          {"args", config.value("args")},
          {"env", env}};
}
void HarnessQuestionMcp::beginTurn(const QString &turn) {
  const auto expected = m_epoch + 1;
  const QPointer<HarnessQuestionMcp> self(this);
  endTurn();
  if (!self || m_epoch != expected)
    return;
  m_turn = turn;
  m_blocked = false;
  m_bridge.beginTurn(turn);
}
void HarnessQuestionMcp::endTurn() {
  ++m_epoch;
  m_turn.clear();
  m_bridge.endTurn();
}
void HarnessQuestionMcp::revoke() {
  ++m_epoch;
  m_turn.clear();
  m_token.clear();
  m_pendingTurn.clear();
  m_form = {};
  m_executable.clear();
  m_blocked = false;
  const QSignalBlocker silent(this);
  m_bridge.revoke();
}
QJsonObject HarnessQuestionMcp::presentation(const QString &token,
                                             const QString &turn,
                                             const QJsonObject &form,
                                             const QJsonObject &result,
                                             const QString &error) {
  return {{"type", "tool"},
          {"itemId", "stagewise-mcp:" + token},
          {"turnId", turn},
          {"name", "askUserQuestions"},
          {"arguments", form},
          {"state", result.isEmpty() && error.isEmpty() ? "input-available"
                    : error.isEmpty()                   ? "output-available"
                                                        : "output-error"},
          {"output", error.isEmpty()
                         ? result.isEmpty()
                               ? QString{}
                               : QString::fromUtf8(QJsonDocument(result).toJson(
                                     QJsonDocument::Compact))
                         : error}};
}
void HarnessQuestionMcp::cancelOwned(const QString &token) {
  if (!owns(token))
    return;
  const auto form = m_form;
  const auto turn = m_pendingTurn;
  const auto epoch = m_epoch;
  m_token.clear();
  m_pendingTurn.clear();
  m_form = {};
  const QPointer<HarnessQuestionMcp> self(this);
  emit pendingChanged(false);
  if (!self || epoch != m_epoch)
    return;
  emit event(
      presentation(token, turn, form, QuestionMcpProtocol::cancelledResult()));
  if (!self || epoch != m_epoch)
    return;
  emit cancelled(token);
}
bool HarnessQuestionMcp::complete(const QString &token,
                                  const QJsonObject &result,
                                  const QString &error) {
  if (!owns(token))
    return false;
  if (error.isEmpty()) {
    const auto steps = result.value("completedSteps");
    if (!result.value("completed").isBool() ||
        !result.value("cancelled").isBool() ||
        !result.value("answers").isObject() || !steps.isDouble() ||
        steps.toDouble() < 0 ||
        steps.toDouble() > m_form.value("steps").toArray().size() ||
        std::floor(steps.toDouble()) != steps.toDouble() ||
        (result.value("completed").toBool() &&
         result.value("cancelled").toBool()) ||
        QJsonDocument(result).toJson(QJsonDocument::Compact).size() >
            2 * 1024 * 1024)
      return false;
  }
  const auto form = m_form;
  const auto turn = m_pendingTurn;
  const auto epoch = m_epoch;
  m_token.clear();
  m_pendingTurn.clear();
  m_form = {};
  const QPointer<HarnessQuestionMcp> self(this);
  emit pendingChanged(false);
  if (!self || epoch != m_epoch)
    return false;
  const bool accepted = m_bridge.complete(token, result, error);
  if (!self || epoch != m_epoch)
    return accepted;
  emit event(presentation(token, turn, form, result, error));
  return accepted;
}
QString HarnessQuestionMcp::preference() {
  return "<stagewise-agent-instructions>\nFor structured user input, use the "
         "MCP tool `stagewise_request_user_input`. "
         "Do not use a provider-native question tool or ask the structured "
         "question in plain text. "
         "This tool is never used for command or tool approval: invoke the "
         "command or tool directly "
         "and let the host show its approval "
         "UI.\n</stagewise-agent-instructions>";
}
QJsonObject HarnessQuestionMcp::wrapperPresentation(const QString &rawName) {
  return {{"version", 1},
          {"questionWrapper", true},
          {"omitted", 0},
          {"truncated", false},
          {"entries", QJsonArray{QJsonObject{{"id", "0"},
                                             {"kind", "generic"},
                                             {"name", rawName.left(4096)},
                                             {"title", rawName.left(4096)}}}}};
}
} // namespace ade
