#include "MessageQueuePanel.h"
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
namespace ade {
MessageQueuePanel::MessageQueuePanel(QWidget *parent)
    : QFrame(parent), m_layout(new QVBoxLayout(this)) {
  setObjectName("messageQueuePanel");
  m_layout->setContentsMargins(8, 6, 8, 6);
  hide();
}
void MessageQueuePanel::setQueue(const QString &chatId,
                                 const QJsonArray &messages, bool busy) {
  if (m_chatId == chatId && m_messages == messages && m_busy == busy)
    return;
  m_chatId = chatId;
  m_messages = messages;
  m_busy = busy;
  while (auto *item = m_layout->takeAt(0)) {
    delete item->widget();
    delete item;
  }
  if (chatId.isEmpty() || messages.isEmpty()) {
    hide();
    return;
  }
  auto *header = new QWidget(this);
  auto *top = new QHBoxLayout(header);
  top->setContentsMargins(0, 0, 0, 0);
  top->addWidget(new QLabel(QString("%1 Queued").arg(messages.size()), header));
  top->addStretch();
  auto *resume = new QPushButton("Resume queue", header);
  resume->setObjectName("queueResume");
  resume->setVisible(!busy);
  top->addWidget(resume);
  connect(resume, &QPushButton::clicked, this,
          [this, chatId] { emit resumeRequested(chatId); });
  m_layout->addWidget(header);
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setMaximumHeight(200);
  scroll->setMinimumHeight(qMin(150, int(messages.size()) * 40));
  auto *body = new QWidget(scroll);
  auto *rows = new QVBoxLayout(body);
  rows->setContentsMargins(0, 0, 0, 0);
  for (int i = 0; i < messages.size(); ++i) {
    const auto message = messages[i].toObject();
    const auto id = message["id"].toString();
    auto *row = new QWidget(body);
    auto *line = new QVBoxLayout(row);
    line->setContentsMargins(0, 0, 0, 2);
    auto *actions = new QWidget(row);
    auto *buttons = new QHBoxLayout(actions);
    buttons->setContentsMargins(0, 0, 0, 0);
    QString preview = message["content"].toString().simplified();
    if (preview.isEmpty())
      preview = "Attachments";
    auto *label = new QLabel(preview.left(140), row);
    label->setTextFormat(Qt::PlainText);
    label->setToolTip(preview.left(2000));
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    line->addWidget(label);
    auto make = [&](const QString &text, const QString &name) {
      auto *button = new QPushButton(text, actions);
      button->setObjectName(name + "_" + id);
      buttons->addWidget(button);
      return button;
    };
    auto *up = make("↑", "queueUp");
    up->setFixedWidth(28);
    up->setAccessibleName("Move queued message up");
    up->setEnabled(i > 0);
    auto *down = make("↓", "queueDown");
    down->setFixedWidth(28);
    down->setAccessibleName("Move queued message down");
    down->setEnabled(i + 1 < messages.size());
    auto *edit = make("Edit", "queueEdit");
    auto *now = make("Send now", "queueSend");
    auto *remove = make("Remove", "queueRemove");
    line->addWidget(actions);
    auto *editor = new QWidget(row);
    auto *editLayout = new QVBoxLayout(editor);
    editLayout->setContentsMargins(0, 0, 0, 0);
    auto *input = new QPlainTextEdit(message["content"].toString(), editor);
    input->setObjectName("queueText_" + id);
    input->setMaximumHeight(96);
    editLayout->addWidget(input);
    auto *save = new QPushButton("Save edit", editor);
    save->setObjectName("queueSave_" + id);
    editLayout->addWidget(save);
    line->addWidget(editor);
    editor->hide();
    connect(edit, &QPushButton::clicked, editor, [editor, input] {
      editor->setVisible(!editor->isVisible());
      if (editor->isVisible())
        input->setFocus();
    });
    connect(save, &QPushButton::clicked, this, [this, chatId, id, input] {
      emit editRequested(chatId, id, input->toPlainText());
    });
    connect(up, &QPushButton::clicked, this,
            [this, chatId, id, i] { emit moveRequested(chatId, id, i - 1); });
    connect(down, &QPushButton::clicked, this,
            [this, chatId, id, i] { emit moveRequested(chatId, id, i + 1); });
    connect(now, &QPushButton::clicked, this,
            [this, chatId, id] { emit sendNowRequested(chatId, id); });
    connect(remove, &QPushButton::clicked, this,
            [this, chatId, id] { emit removeRequested(chatId, id); });
    rows->addWidget(row);
  }
  scroll->setWidget(body);
  m_layout->addWidget(scroll);
  show();
}
} // namespace ade
