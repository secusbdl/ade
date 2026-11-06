#include "AgentEngine.h"
#include "harness/AcpHarnessSession.h"
#include "QueuedMessages.h"
#include <QJsonDocument>
#include <QUuid>
namespace ade {
void AgentEngine::setExternalToolTimeout(const QString& name,int milliseconds){
    if(!m_busy&&!name.isEmpty()&&name.size()<=128&&milliseconds>=1000&&milliseconds<=310000)m_externalTimeouts.insert(name,milliseconds);
}
QString AgentEngine::externalToolCallId(const QString &requestId) const {
  return m_busy && !requestId.isEmpty() && requestId == m_externalRequestId &&
                 m_toolIndex >= 0 && m_toolIndex < m_pendingTools.size()
             ? m_pendingTools[m_toolIndex].id
             : QString();
}
void AgentEngine::setFileMutationExecutor(FileMutationExecutor executor) {
  if (!m_busy)
    m_tools.setFileMutationExecutor(std::move(executor));
}
void AgentEngine::setQueueCommitter(QueueCommitter committer) {
  if (!m_busy && !m_committingQueue) {
    m_queueCommitter = std::move(committer);
    reconcileMutationHistory();
  }
}
bool AgentEngine::setQueuedMessages(const QJsonArray &queue, QString *error) {
  if (m_busy || m_committingQueue) {
    if (error)
      *error = "Cannot restore the queue while the agent is working.";
    return false;
  }
  if (!QueuedMessages::validate(queue, error))
    return false;
  m_queuedMessages = queue;
  m_queuePaused = true;
  ++m_queueResumeSerial;
  emit queuedMessagesChanged();
  return true;
}
bool AgentEngine::commitQueue(const QJsonArray &history,
                              const QJsonArray &queue, QString *error) {
  if (m_committingQueue) {
    if (error)
      *error = "A queue save is already in progress.";
    return false;
  }
  if (!QueuedMessages::validate(queue, error))
    return false;
  const auto commit = m_queueCommitter;
  if (!commit)
    return true;
  const QPointer<AgentEngine> self(this);
  m_committingQueue = true;
  bool saved = false;
  try {
    saved = commit(history, queue, error);
  } catch (...) {
    if (error)
      *error = "The queued message could not be saved.";
  }
  if (!self)
    return false;
  m_committingQueue = false;
  if (!saved && error && error->isEmpty())
    *error = "The queued message could not be saved.";
  return saved;
}
bool AgentEngine::replaceQueue(const QJsonArray &queue) {
  if (m_committingQueue)
    return false;
  const QPointer<AgentEngine> self(this);
  const auto generation = m_generation;
  const auto old = m_queuedMessages;
  QString error;
  const bool saved = commitQueue(m_messages, queue, &error);
  if (!self || m_generation != generation || m_queuedMessages != old)
    return false;
  if (!saved) {
    emit queueError(error);
    return false;
  }
  m_queuedMessages = queue;
  emit queuedMessagesChanged();
  return true;
}
bool AgentEngine::sendUserMessage(const QString &text,
                                  const QJsonArray &attachments,
                                  const QJsonObject &references,
                                  const QJsonObject &pathMetadata,bool queueIfBlocked) {
  if (m_committingQueue || (text.trimmed().isEmpty() && attachments.isEmpty()))
    return false;
  const auto message =
      QueuedMessages::create(text, attachments, references, pathMetadata);
  if (!m_busy && m_queuedMessages.isEmpty() && !queueIfBlocked) {
    QString error;
    if (!QueuedMessages::validate({message}, &error)) {
      emit queueError(error);
      return false;
    }
    m_queuePaused = false;
    return startUserMessage(text, attachments, references, pathMetadata, {},
                            message);
  }
  auto queue = m_queuedMessages;
  queue.append(message);
  const QPointer<AgentEngine> self(this);
  const auto generation = m_generation;
  const bool question = !m_questionToken.isEmpty();
  if (!replaceQueue(queue))
    return false;
  if (!self || m_generation != generation)
    return true;
  if (queueIfBlocked) {
    if (!m_busy && !m_queuePaused) resumeQueue();
    return true;
  }
  if (question && !queueIfBlocked && !m_questionToken.isEmpty()) {
    m_questionStopReason = "user_sent_message";
    cancel();
    if (!self || m_busy || m_generation != generation + 1)
      return true;
  }
  m_queuePaused = false;
  if (!m_busy)
    resumeQueue();
  return true;
}
bool AgentEngine::editQueuedMessage(const QString &id, const QString &text) {
  auto queue = m_queuedMessages;
  const int index = QueuedMessages::indexOf(queue, id);
  if (index < 0)
    return false;
  auto row = queue[index].toObject();
  const auto old = row["metadata"].toObject();
  auto updated = QueuedMessages::create(text, old["attachments"].toArray(),
                                        old["pathReferences"].toObject(),
                                        old["pathMetadata"].toObject());
  // Editing text keeps the queue identity and original captured attachment
  // data.
  auto metadata = old;
  auto display = updated["metadata"].toObject()["display"].toObject();
  display["id"] = id;
  auto parts = display["parts"].toArray();
  auto part = parts[0].toObject();
  part["id"] = id + "/0";
  parts[0] = part;
  display["parts"] = parts;
  metadata["display"] = display;
  row["content"] = text;
  row["metadata"] = metadata;
  queue[index] = row;
  return replaceQueue(queue);
}
bool AgentEngine::removeQueuedMessage(const QString &id) {
  auto queue = m_queuedMessages;
  const int index = QueuedMessages::indexOf(queue, id);
  if (index < 0)
    return false;
  queue.removeAt(index);
  return replaceQueue(queue);
}
bool AgentEngine::moveQueuedMessage(const QString &id, int to) {
  auto queue = m_queuedMessages;
  const int from = QueuedMessages::indexOf(queue, id);
  if (from < 0 || to < 0 || to >= queue.size())
    return false;
  if (from == to)
    return true;
  const QJsonValue row = queue.at(from);
  queue.removeAt(from);
  queue.insert(to, row);
  return replaceQueue(queue);
}
bool AgentEngine::flushQueuedMessage(const QString &id) {
  if (m_committingQueue || m_queuedMessages.isEmpty())
    return false;
  const QPointer<AgentEngine> self(this);
  const auto generation = m_generation;
  if (!id.isEmpty() && !moveQueuedMessage(id, 0))
    return false;
  if (!self || m_generation != generation)
    return false;
  if (m_busy) {
    cancel();
    if (!self || m_busy || m_generation != generation + 1)
      return false;
  }
  return resumeQueue();
}
bool AgentEngine::resumeQueue() {
  if (m_busy || m_committingQueue || m_queuedMessages.isEmpty())
    return false;
  const auto row = m_queuedMessages.first().toObject(),
             meta = row["metadata"].toObject();
  m_queuePaused = false;
  const QPointer<AgentEngine> self(this);
  const bool accepted =
      startUserMessage(row["content"].toString(), meta["attachments"].toArray(),
                       meta["pathReferences"].toObject(),
                       meta["pathMetadata"].toObject(), row["id"].toString());
  if (self && !accepted)
    m_queuePaused = true;
  return accepted;
}
void AgentEngine::scheduleQueue() {
  if (m_busy || m_queuePaused || m_queuedMessages.isEmpty())
    return;
  const auto generation = m_generation, serial = m_queueResumeSerial;
  QTimer::singleShot(0, this, [this, generation, serial] {
    if (!m_busy && !m_queuePaused && generation == m_generation &&
        serial == m_queueResumeSerial)
      resumeQueue();
  });
}
QJsonObject AgentEngine::pendingQuestion() const {
  return m_questionToken.isEmpty() ? QJsonObject{}
                                   : m_question.snapshot(m_questionToken);
}
void AgentEngine::beginQuestion(const ToolCall &call,
                                const QJsonObject &arguments) {
  QString error;
  QuestionForm question;
  if (!m_questionToken.isEmpty() || !question.begin(arguments, &error)) {
    recordToolResult(call.id,
                     error.isEmpty()
                         ? QString("Only one user question may be active.")
                         : error,
                     true);
    return;
  }
  m_question = std::move(question);
  m_questionToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
  m_questionCallId = call.id;
  emit questionChanged();
}
bool AgentEngine::updateQuestionDraft(const QString &token, int step,
                                      const QJsonObject &answers) {
  if (!m_busy || token.isEmpty() || token != m_questionToken)
    return false;
  QString error;
  if (!m_question.updateDraft(step, answers, &error)) {
    if (!error.isEmpty())
      emit questionError(token, error);
    return false;
  }
  return true;
}
bool AgentEngine::submitQuestionStep(const QString &token, int step,
                                     const QJsonObject &answers) {
  if (!m_busy || token.isEmpty() || token != m_questionToken)
    return false;
  QString error;
  if(m_provider.isOpenCodeHarness()&&m_acp&&!m_harnessQuestionToken.isEmpty()) {
    auto candidate=m_question;
    if(candidate.submit(step,answers,&error)&&candidate.completed()&&!m_acp->validQuestionAnswer(m_harnessQuestionToken,candidate.result({})["answers"].toObject())) {
      emit questionError(token,"The answers do not satisfy the coding agent's form constraints.");return false;
    }
  }
  if (!m_question.submit(step, answers, &error)) {
    emit questionError(token, error);
    return false;
  }
  if (m_question.completed())
    finishQuestion({});
  else {
    m_questionToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
    emit questionChanged();
  }
  return true;
}
bool AgentEngine::goBackQuestion(const QString &token, int step) {
  if (!m_busy || token.isEmpty() || token != m_questionToken ||
      !m_question.back(step))
    return false;
  m_questionToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
  emit questionChanged();
  return true;
}
bool AgentEngine::cancelQuestion(const QString &token,
                                 const QJsonObject &draft) {
  if (!m_busy || token.isEmpty() || token != m_questionToken)
    return false;
  if (!draft.isEmpty() && !updateQuestionDraft(token, m_question.step(), draft))
    return false;
  finishQuestion("user_cancelled");
  return true;
}
void AgentEngine::finishQuestion(const QString &reason) {
  if(!m_harnessQuestionToken.isEmpty()){finishHarnessQuestion(reason);return;}
  const auto id = m_questionCallId;
  const auto output = QString::fromUtf8(
      QJsonDocument(m_question.result(reason)).toJson(QJsonDocument::Compact));
  m_questionToken.clear();
  m_questionCallId.clear();
  m_questionStopReason.clear();
  const QPointer<AgentEngine> self(this);
  const auto generation = m_generation;
  recordToolResult(id, output, false, {},
                   reason.isEmpty() ? QString("output-available")
                                    : QString("canceled"));
  if (self && generation == m_generation)
    emit questionChanged();
}
} // namespace ade
