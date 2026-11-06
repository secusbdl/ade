#include "ChatSurface.h"
#include "Composer.h"
#include "ContentControlsScrollArea.h"
#include "MainWindow.h"
#include "MessageQueuePanel.h"
#include "ModelSelection.h"
#include "QuestionPanel.h"
#include "SettingsPage.h"
#include "core/AgentEngine.h"
#include <QDateTime>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
namespace ade {
namespace {
class ControlBounds final : public QObject {
public:
  ControlBounds(QWidget *chat, QScrollArea *scroll)
      : QObject(scroll), m_chat(chat), m_scroll(scroll) {
    chat->installEventFilter(this);
    fit();
  }

protected:
  bool eventFilter(QObject *, QEvent *event) override {
    if (event->type() == QEvent::Resize)
      fit();
    return false;
  }

private:
  void fit() {
    if (m_chat && m_scroll)
      m_scroll->setMaximumHeight(qBound(48, m_chat->height() - 100, 440));
  }
  QPointer<QWidget> m_chat;
  QPointer<QScrollArea> m_scroll;
};
} // namespace
void MainWindow::initializeAgentControls() {
  if (m_agentControlsScroll)
    return;
  m_agentControlsScroll = new ContentControlsScrollArea(m_chat);
  m_agentControlsScroll->setObjectName("agentControlsScroll");
  m_agentControlsScroll->setWidgetResizable(true);
  m_agentControlsScroll->setFrameShape(QFrame::NoFrame);
  m_agentControlsScroll->setMinimumHeight(0);
  m_agentControlsScroll->setSizePolicy(QSizePolicy::Expanding,
                                       QSizePolicy::Preferred);
  auto *body = new QWidget(m_agentControlsScroll);
  auto *layout = new QVBoxLayout(body);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(4);
  auto *back = new QPushButton("Back to questions", body);
  back->setObjectName("backToQuestions");
  back->hide();
  layout->addWidget(back);
  m_questionPanel = new QuestionPanel(body);
  m_queuePanel = new MessageQueuePanel(body);
  layout->addWidget(m_questionPanel);
  layout->addWidget(m_queuePanel);
  m_agentControlsScroll->setWidget(body);
  auto *footer = m_chat->footerLayout();
  const int composerIndex = footer->indexOf(m_composerBox);
  footer->insertWidget(qMax(0, composerIndex), m_agentControlsScroll);
  new ControlBounds(m_chat, m_agentControlsScroll);
  m_agentControlsScroll->hide();
  connect(back, &QPushButton::clicked, this, [this] {
    m_questionMessageToken.clear();
    refreshAgentControls();
  });
  auto currentEngine = [this](const QString &chat) -> AgentEngine * {
    const auto *session = find(chat);
    if (chat != m_currentId || !session || session->archived)
      return nullptr;
    const QPointer<MainWindow> self(this);
    const QPointer<AgentEngine> engine = engineFor(chat);
    return self && engine ? engine.data() : nullptr;
  };
  connect(m_questionPanel, &QuestionPanel::submitStep, this,
          [currentEngine](const QString &chat, const QString &token, int step,
                          const QJsonObject &answers) {
            if (auto *engine = currentEngine(chat))
              engine->submitQuestionStep(token, step, answers);
          });
  connect(m_questionPanel, &QuestionPanel::draftChanged, this,
          [currentEngine](const QString &chat, const QString &token, int step,
                          const QJsonObject &answers) {
            if (auto *engine = currentEngine(chat))
              engine->updateQuestionDraft(token, step, answers);
          });
  connect(m_questionPanel, &QuestionPanel::goBack, this,
          [currentEngine](const QString &chat, const QString &token, int step) {
            if (auto *engine = currentEngine(chat))
              engine->goBackQuestion(token, step);
          });
  connect(m_questionPanel, &QuestionPanel::cancelled, this,
          [currentEngine](const QString &chat, const QString &token,
                          const QJsonObject &answers) {
            if (auto *engine = currentEngine(chat))
              engine->cancelQuestion(token, answers);
          });
  connect(m_questionPanel, &QuestionPanel::messageRequested, this,
          [this, currentEngine](const QString &chat, const QString &token) {
            auto *engine = currentEngine(chat);
            if (!engine || token != engine->pendingQuestion()["id"].toString())
              return;
            m_questionMessageToken = token;
            const QPointer<MainWindow> self(this);
            refreshAgentControls();
            if (self)
              m_composer->setFocus();
          });
  connect(m_queuePanel, &MessageQueuePanel::editRequested, this,
          [currentEngine](const QString &chat, const QString &id,
                          const QString &text) {
            if (auto *engine = currentEngine(chat))
              engine->editQueuedMessage(id, text);
          });
  connect(m_queuePanel, &MessageQueuePanel::removeRequested, this,
          [currentEngine](const QString &chat, const QString &id) {
            if (auto *engine = currentEngine(chat))
              engine->removeQueuedMessage(id);
          });
  connect(m_queuePanel, &MessageQueuePanel::moveRequested, this,
          [currentEngine](const QString &chat, const QString &id, int index) {
            if (auto *engine = currentEngine(chat))
              engine->moveQueuedMessage(id, index);
          });
  auto resume = [this, currentEngine](const QString &chat,
                                      const QString &queuedId) {
    auto *engine = currentEngine(chat);
    auto *session = find(chat);
    if (!engine || !session)
      return;
    if (gitPathBusy(session->workspace)) {
      m_status->setText(
          "Wait for the workspace operation before resuming queued messages.");
      return;
    }
    if (!engine->isBusy()) {
      ModelSelection selection{
          session->providerId, session->model, session->presetId, {}, {}};
      ProviderConfig provider;
      if (!selection.resolve(m_providers, &provider)) {
        m_status->setText("Select a valid model before resuming the queue.");
        return;
      }
      engine->setModelChain(
          selectedModelChain(selection, m_providers,
                             m_settings->preference("modelPresets").toArray()),
          selection.presetId);
      engine->setAlwaysAllow(session->alwaysAllow);
    }
    if (queuedId.isEmpty())
      engine->resumeQueue();
    else
      engine->flushQueuedMessage(queuedId);
  };
  connect(m_queuePanel, &MessageQueuePanel::resumeRequested, this,
          [resume](const QString &chat) { resume(chat, {}); });
  connect(m_queuePanel, &MessageQueuePanel::sendNowRequested, this, resume);
}
void MainWindow::connectAgentControls(const QString &id, AgentEngine *engine) {
  const QPointer<MainWindow> self(this);
  const QPointer<AgentEngine> owner(engine);
  if (const auto *session = find(id)) {
    QString error;
    if (!engine->setQueuedMessages(session->queuedMessages, &error) &&
        !error.isEmpty())
      m_status->setText(error);
  }
  engine->setQueueCommitter([self, owner, id](const QJsonArray &history,
                                              const QJsonArray &queue,
                                              QString *error) {
    if (!self || !owner || self->m_engines.value(id) != owner) {
      if (error)
        *error = "The chat is no longer available.";
      return false;
    }
    auto candidate = self->m_sessions;
    int index = -1;
    for (int i = 0; i < candidate.size(); ++i)
      if (candidate[i].id == id) {
        index = i;
        break;
      }
    if (index < 0 || candidate[index].archived) {
      if (error)
        *error = "Archived or removed chats cannot accept queued messages.";
      return false;
    }
    auto &chat = candidate[index];
    const bool accepted = chat.messages != history;
    if(accepted&&self->gitPathBusy(chat.workspace)){
      if(error)*error="Wait for the workspace operation before starting this queued message.";
      return false;
    }
    chat.messages = history;
    chat.queuedMessages = queue;
    chat.updated = QDateTime::currentMSecsSinceEpoch();
    if (accepted)
      chat.lastMessageAt = chat.updated;
    if (!self->m_store.save(candidate, error))
      return false;
    self->m_sessions = std::move(candidate);
    return true;
  });
  connect(engine, &AgentEngine::queuedMessagesChanged, this,
          [this, id, self, owner] {
            if (!owner || m_engines.value(id) != owner)
              return;
            if (id == m_currentId)
              refreshAgentControls();
            if (!self)
              return;
            refreshAgents();
          });
  connect(engine, &AgentEngine::queueError, this,
          [this, id](const QString &error) {
            if (id == m_currentId)
              m_status->setText("Queue: " + error);
          });
  connect(engine, &AgentEngine::questionChanged, this, [this, id] {
    if (id == m_currentId)
      refreshAgentControls();
  });
  connect(engine, &AgentEngine::questionError, this,
          [this, id](const QString &token, const QString &error) {
            if (m_questionPanel)
              m_questionPanel->setError(id, token, error);
          });
  connect(engine, &AgentEngine::busyChanged, this, [this, id](bool) {
    if (id == m_currentId)
      refreshAgentControls();
  });
}
void MainWindow::refreshAgentControls() {
  if (!m_questionPanel || !m_queuePanel || !m_agentControlsScroll)
    return;
  const auto *session = current();
  const auto id = session ? session->id : QString();
  const QPointer<AgentEngine> engine = m_engines.value(id);
  const bool active = session && !session->archived;
  const auto question =
      active && engine ? engine->pendingQuestion() : QJsonObject{};
  const auto queue =
      active ? (engine ? engine->queuedMessages() : session->queuedMessages)
             : QJsonArray{};
  const bool busy = engine && engine->isBusy();
  const bool answering = !question.isEmpty();
  if (!answering || m_questionMessageToken != question["id"].toString())
    m_questionMessageToken.clear();
  const bool writing = answering && !m_questionMessageToken.isEmpty();
  const QPointer<MainWindow> self(this);
  m_questionPanel->setQuestion(id, question);
  if (!self)
    return;
  m_questionPanel->setVisible(answering && !writing);
  m_queuePanel->setQueue(id, queue, busy);
  if (!self)
    return;
  if (auto *back =
          m_agentControlsScroll->findChild<QPushButton *>("backToQuestions"))
    back->setVisible(writing);
  m_agentControlsScroll->setVisible(answering || !queue.isEmpty());
  m_composerBox->setVisible(!answering || writing);
}
} // namespace ade
