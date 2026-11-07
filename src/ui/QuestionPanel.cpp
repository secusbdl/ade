#include "QuestionPanel.h"
#include "ContentControlsScrollArea.h"
#include "MarkdownParts.h"
#include "core/QuestionForm.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QKeyEvent>
#include <QShortcut>
#include <cmath>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QTextBrowser>
#include <QTextDocument>
#include <QVBoxLayout>
namespace ade {
namespace {
class SubmitTextOnEnter final : public QObject {
public:
  SubmitTextOnEnter(QObject *parent, std::function<void()> submit)
      : QObject(parent), m_submit(std::move(submit)) {}
protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (event->type() != QEvent::KeyPress) return false;
    const auto *key = static_cast<QKeyEvent *>(event);
    if ((key->key() != Qt::Key_Return && key->key() != Qt::Key_Enter) ||
        (key->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::MetaModifier)))
      return false;
    // Copy before calling out: submission may synchronously replace/delete us.
    if (auto *button = qobject_cast<QPushButton *>(watched)) {
      if (!key->isAutoRepeat()) button->click();
      return true;
    }
    const auto submit = m_submit;
    if (!key->isAutoRepeat()) submit();
    return true;
  }
private:
  std::function<void()> m_submit;
};
TranscriptText *formText(const QString &markdown, QWidget *parent) {
  // Shared denied document + image-object handler prevent Qt's local-file
  // image fallback. Questions do not install a trusted image resolver.
  auto *text = new TranscriptText(parent);
  text->setSource(markdown);
  return text;
}
} // namespace
QuestionPanel::QuestionPanel(QWidget *parent)
    : QFrame(parent), m_layout(new QVBoxLayout(this)) {
  setObjectName("questionPanel");
  m_layout->setContentsMargins(12, 10, 12, 10);
  m_layout->setSpacing(8);
  hide();
}
QJsonObject QuestionPanel::draftAnswers() const {
  QJsonObject out;
  for (auto it = m_values.begin(); it != m_values.end(); ++it)
    out[it.key()] = it.value()();
  return out;
}
void QuestionPanel::setQuestion(const QString &chatId,
                                const QJsonObject &question) {
  if (m_chatId == chatId && m_question == question)
    return;
  m_chatId = chatId;
  m_question = question;
  rebuild();
}
void QuestionPanel::setError(const QString &chatId, const QString &token,
                             const QString &error) {
  if (chatId == m_chatId && token == this->token() && m_error) {
    m_error->setText(error);
    m_error->setVisible(!error.isEmpty());
  }
}
void QuestionPanel::changed() {
  if (m_error) {
    m_error->clear();
    m_error->hide();
  }
  emit draftChanged(m_chatId, token(), m_question["currentStep"].toInt(),
                    draftAnswers());
}
void QuestionPanel::rebuild() {
  m_values.clear();
  m_error = nullptr;
  while (auto *item = m_layout->takeAt(0)) {
    delete item->widget();
    delete item;
  }
  const auto steps = m_question["steps"].toArray();
  const int step = m_question["currentStep"].toInt(-1);
  if (m_chatId.isEmpty() || token().isEmpty() || step < 0 ||
      step >= steps.size() || !QuestionForm::validate(m_question)) {
    hide();
    return;
  }
  const auto chat = m_chatId, id = token();
  const auto data = steps[step].toObject();
  const auto fields = data["fields"].toArray();
  auto *title = new QLabel(m_question["title"].toString(), this);
  title->setTextFormat(Qt::PlainText);
  title->setWordWrap(true);
  auto titleFont = title->font();
  titleFont.setBold(true);
  title->setFont(titleFont);
  m_layout->addWidget(title);
  auto *scroll = new ContentControlsScrollArea(this);
  scroll->setObjectName("questionFields");
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setMaximumHeight(320);
  scroll->setMinimumHeight(100);
  auto *body = new QWidget(scroll);
  auto *layout = new QVBoxLayout(body);
  layout->setContentsMargins(0, 0, 6, 0);
  layout->setSpacing(8);
  for (const auto &text :
       {m_question["description"].toString(), data["title"].toString(),
        data["description"].toString()})
    if (!text.isEmpty())
      layout->addWidget(formText(text, body));
  const auto values =
      QuestionForm::defaults(fields, m_question["answers"].toObject());
  for (const auto &entry : fields) {
    const auto f = entry.toObject();
    const auto fieldId = f["questionId"].toString(),
               type = f["type"].toString();
    auto *container = new QWidget(body);
    container->setObjectName("questionField_" + fieldId);
    auto *fieldLayout = new QVBoxLayout(container);
    fieldLayout->setContentsMargins(0, 4, 0, 4);
    fieldLayout->setSpacing(4);
    if (type != "checkbox")
      fieldLayout->addWidget(formText(
          f["label"].toString() +
              (f["required"].toBool() ? QStringLiteral(" *") : QString()),
          container));
    if (!f["description"].toString().isEmpty())
      fieldLayout->addWidget(formText(f["description"].toString(), container));
    if (type == "input") {
      auto *input = new QLineEdit(container);
      input->setObjectName("answer_" + fieldId);
      input->setAccessibleName(f["label"].toString());
      input->setMaxLength(f.contains("maxLength") ? f["maxLength"].toInt()
                                                  : 8192);
      input->setPlaceholderText(f["placeholder"].toString());
      input->setText(
          values[fieldId].isString()
              ? values[fieldId].toString()
              : QString::number(values[fieldId].toDouble(), 'g', 17));
      if (f["inputType"] == "password")
        input->setEchoMode(QLineEdit::Password);
      fieldLayout->addWidget(input);
      const bool number = f["inputType"] == "number";
      m_values[fieldId] = [input, number] {
        const auto text = input->text();
        if (number && !text.isEmpty()) {
          bool ok = false;
          const double value = text.toDouble(&ok);
          if (ok && std::isfinite(value)) return QJsonValue(value);
        }
        return QJsonValue(text);
      };
      connect(input, &QLineEdit::textChanged, this, [this] { changed(); });
    } else if (type == "checkbox") {
      auto *input = new QCheckBox(f["label"].toString(), container);
      input->setObjectName("answer_" + fieldId);
      input->setChecked(values[fieldId].toBool());
      fieldLayout->addWidget(input);
      m_values[fieldId] = [input] { return QJsonValue(input->isChecked()); };
      connect(input, &QCheckBox::toggled, this, [this] { changed(); });
    } else if (type == "checkbox-group") {
      QList<QPair<QString, QCheckBox *>> inputs;
      for (const auto &option : f["options"].toArray()) {
        const auto o = option.toObject();
        auto *input = new QCheckBox(o["label"].toString(), container);
        input->setObjectName("answer_" + fieldId + "_" + o["value"].toString());
        input->setChecked(values[fieldId].toArray().contains(o["value"]));
        fieldLayout->addWidget(input);
        inputs.append({o["value"].toString(), input});
        connect(input, &QCheckBox::toggled, this, [this] { changed(); });
      }
      m_values[fieldId] = [inputs] {
        QJsonArray out;
        for (const auto &input : inputs)
          if (input.second->isChecked())
            out.append(input.first);
        return out;
      };
    } else {
      auto *group = new QButtonGroup(container);
      QHash<int, QString> options;
      int index = 0;
      for (const auto &option : f["options"].toArray()) {
        const auto o = option.toObject();
        auto *input = new QRadioButton(o["label"].toString(), container);
        input->setObjectName("answer_" + fieldId + "_" + o["value"].toString());
        input->setChecked(values[fieldId] == o["value"]);
        group->addButton(input, index);
        options[index++] = o["value"].toString();
        fieldLayout->addWidget(input);
      }
      QLineEdit *other = nullptr;
      const int otherId = index;
      if (f["allowOther"] != false) {
        auto *radio = new QRadioButton("Other", container);
        radio->setObjectName("answer_" + fieldId + "_other");
        group->addButton(radio, otherId);
        fieldLayout->addWidget(radio);
        other = new QLineEdit(container);
        other->setObjectName("answer_" + fieldId + "_otherText");
        other->setPlaceholderText("Your answer");
        other->setMaxLength(8192 - 10);
        const auto saved = values[fieldId].toString();
        radio->setChecked(saved.startsWith("__other__:"));
        if (radio->isChecked())
          other->setText(saved.mid(10));
        other->setEnabled(radio->isChecked());
        fieldLayout->addWidget(other);
        connect(radio, &QRadioButton::toggled, other, &QWidget::setEnabled);
        connect(other, &QLineEdit::textChanged, this, [this] { changed(); });
      }
      m_values[fieldId] = [group, options, other, otherId] {
        const int checked = group->checkedId();
        return QJsonValue(other && checked == otherId
                              ? "__other__:" + other->text()
                              : options.value(checked));
      };
      connect(group, &QButtonGroup::buttonToggled, this,
              [this](QAbstractButton *, bool checked) {
                if (checked)
                  changed();
              });
    }
    layout->addWidget(container);
  }
  layout->addStretch();
  scroll->setWidget(body);
  m_layout->addWidget(scroll);
  m_error = new QLabel(this);
  m_error->setObjectName("questionError");
  m_error->setWordWrap(true);
  m_error->setTextFormat(Qt::PlainText);
  m_error->hide();
  m_layout->addWidget(m_error);
  auto *actions = new QWidget(this);
  auto *row = new QHBoxLayout(actions);
  row->setContentsMargins(0, 0, 0, 0);
  auto *cancel = new QPushButton("Cancel", actions);
  cancel->setObjectName("questionCancel");
  row->addWidget(cancel);
  auto *progress =
      new QLabel(QString("%1 / %2").arg(step + 1).arg(steps.size()), actions);
  row->addWidget(progress);
  row->addStretch();
  auto *back = new QPushButton("Back", actions);
  back->setObjectName("questionBack");
  back->setEnabled(step > 0);
  row->addWidget(back);
  auto *submit = new QPushButton(
      step + 1 == steps.size() ? QString("Submit") : QString("Next"), actions);
  submit->setObjectName("questionSubmit");
  // Explicit text/command-Enter handlers below avoid QDialog default-button
  // behavior submitting when Enter belongs to a checkbox/radio control.
  submit->setAutoDefault(false);
  row->addWidget(submit);
  m_layout->addWidget(actions);
  connect(cancel, &QPushButton::clicked, this,
          [this, chat, id] { emit cancelled(chat, id, {}); });
  connect(back, &QPushButton::clicked, this,
          [this, chat, id, step] { emit goBack(chat, id, step); });
  const auto submitCurrent = [this, chat, id, step, fields] {
    if (m_chatId != chat || token() != id || m_question["currentStep"].toInt() != step)
      return;
    const auto answers = draftAnswers();
    QString error;
    if (!QuestionForm::validateAnswers(fields, answers, true, &error)) {
      setError(chat, id, error);
      return;
    }
    emit submitStep(chat, id, step, answers);
  };
  connect(submit, &QPushButton::clicked, this, submitCurrent);
  auto *enter = new SubmitTextOnEnter(actions, submitCurrent);
  for (auto *input : body->findChildren<QLineEdit *>()) input->installEventFilter(enter);
  // Main and side-chat forms may be visible together. Route command-Enter to
  // the focused form, so equal window-wide shortcuts cannot cancel each other.
  for (const auto modifier : {Qt::CTRL, Qt::META})
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
      auto *shortcut = new QShortcut(QKeySequence(modifier | key), this);
      shortcut->setContext(Qt::WidgetWithChildrenShortcut);
      shortcut->setAutoRepeat(false);
      connect(actions, &QObject::destroyed, shortcut, [shortcut] { delete shortcut; });
      connect(shortcut, &QShortcut::activated, this, submitCurrent);
    }
  auto *message = new QPushButton("Write a message instead", this);
  message->setObjectName("questionWriteMessage");
  m_layout->addWidget(message);
  connect(message, &QPushButton::clicked, this,
          [this, chat, id] { emit messageRequested(chat, id); });
  for (auto *button : findChildren<QPushButton *>()) {
    button->setAutoDefault(false);
    button->installEventFilter(enter);
  }
  show();
}
} // namespace ade
