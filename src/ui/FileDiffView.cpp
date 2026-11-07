#include "FileDiffView.h"
#include "AppFonts.h"
#include "mutations/FileMutationDiff.h"
#include <QLabel>
#include <QPlainTextEdit>
#include <QSyntaxHighlighter>
#include <QVBoxLayout>
#include <QVector>
namespace ade {
namespace {
class DiffColors final : public QSyntaxHighlighter {
public:
  explicit DiffColors(QPlainTextEdit *editor)
      : QSyntaxHighlighter(editor->document()), m_editor(editor) {}

protected:
  void highlightBlock(const QString &text) override {
    const bool dark =
        m_editor->palette().color(QPalette::Base).lightness() < 128;
    QTextCharFormat f;
    if (text.startsWith('+')) {
      f.setForeground(dark ? QColor("#8cddb3") : QColor("#145d35"));
      f.setBackground(dark ? QColor("#183b2a") : QColor("#e1f4e7"));
    } else if (text.startsWith('-')) {
      f.setForeground(dark ? QColor("#f2a4ab") : QColor("#91252e"));
      f.setBackground(dark ? QColor("#43262a") : QColor("#fce4e6"));
    } else if (text.startsWith("@@")) {
      f.setForeground(dark ? QColor("#a8c9ed") : QColor("#355e90"));
    } else
      return;
    setFormat(0, text.size(), f);
  }

private:
  QPlainTextEdit *m_editor;
};
class DiffText final : public QPlainTextEdit {
public:
  explicit DiffText(QWidget *parent)
      : QPlainTextEdit(parent), colors(new DiffColors(this)) {}

protected:
  void changeEvent(QEvent *event) override {
    QPlainTextEdit::changeEvent(event);
    if (colors && (event->type() == QEvent::PaletteChange ||
                   event->type() == QEvent::ApplicationPaletteChange))
      colors->rehighlight();
  }

private:
  DiffColors *colors = nullptr;
};
QStringList lines(const QString &text) {
  if (text.isEmpty())
    return {};
  auto result = text.split('\n');
  if (text.endsWith('\n'))
    result.removeLast();
  return result;
}
} // namespace
FileDiffView::FileDiffView(QWidget *parent)
    : QWidget(parent), m_layout(new QVBoxLayout(this)) {
  setObjectName("fileDiffView");
  m_layout->setContentsMargins(0, 0, 0, 0);
  m_layout->setSpacing(6);
}
QString FileDiffView::unifiedPreview(const QString &before,
                                     const QString &after) {
  const auto a = lines(before), b = lines(after);
  QStringList output;
  bool truncated = false;
  auto append = [&](QChar type, const QString &text) {
    if (output.size() < 1200)
      output.append(type + text);
    else
      truncated = true;
  };
  int prefix = 0;
  while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix])
    ++prefix;
  int suffix = 0;
  while (suffix < a.size() - prefix && suffix < b.size() - prefix &&
         a[a.size() - suffix - 1] == b[b.size() - suffix - 1])
    ++suffix;
  if (prefix == a.size() && prefix == b.size() &&
      before.endsWith('\n') == after.endsWith('\n'))
    return "No text changes.";
  output.append(QString("@@ -%1 +%1 @@").arg(qMax(1, prefix - 2)));
  for (int i = qMax(0, prefix - 3); i < prefix; ++i)
    append(' ', a[i]);
  const int na = a.size() - prefix - suffix, nb = b.size() - prefix - suffix;
  if (qint64(na + 1) * (nb + 1) <= 1000000) {
    QVector<int> lcs((na + 1) * (nb + 1));
    auto at = [&](int i, int j) -> int & { return lcs[i * (nb + 1) + j]; };
    for (int i = na - 1; i >= 0; --i)
      for (int j = nb - 1; j >= 0; --j)
        at(i, j) = a[prefix + i] == b[prefix + j]
                       ? 1 + at(i + 1, j + 1)
                       : qMax(at(i + 1, j), at(i, j + 1));
    int i = 0, j = 0;
    while (i < na || j < nb) {
      if (i < na && j < nb && a[prefix + i] == b[prefix + j]) {
        append(' ', a[prefix + i]);
        ++i;
        ++j;
      } else if (i < na && (j == nb || at(i + 1, j) >= at(i, j + 1)))
        append('-', a[prefix + i++]);
      else
        append('+', b[prefix + j++]);
    }
  } else {
    for (int i = 0; i < na; ++i)
      append('-', a[prefix + i]);
    for (int j = 0; j < nb; ++j)
      append('+', b[prefix + j]);
  }
  for (int i = 0; i < qMin(3, suffix); ++i)
    append(' ', a[a.size() - suffix + i]);
  if (truncated)
    output.append("@@ Preview truncated after 1,200 lines @@");
  if (before.endsWith('\n') != after.endsWith('\n'))
    output.append(after.endsWith('\n') ? "+ Final newline added"
                                       : "- Final newline removed");
  return output.join('\n');
}
void FileDiffView::setDiff(const QJsonObject &diff) {
  if (diff == m_diff)
    return;
  m_diff = diff;
  while (auto *item = m_layout->takeAt(0)) {
    delete item->widget();
    delete item;
  }
  if (!validMutationDiff(diff)) {
    hide();
    return;
  }
  for (const auto &value : diff["files"].toArray()) {
    const auto file = value.toObject();
    const bool added = file["before"].isNull(),
               deleted = file["after"].isNull();
    auto *name =
        new QLabel(file["path"].toString() + (added     ? " · Added"
                                              : deleted ? " · Deleted"
                                                        : " · Modified"),
                   this);
    name->setTextFormat(Qt::PlainText);
    name->setWordWrap(true);
    name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_layout->addWidget(name);
    auto *text = new DiffText(this);
    text->setObjectName("fileDiffText");
    text->setReadOnly(true);
    text->setFrameShape(QFrame::NoFrame);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    auto face = text->font();
    face.setFamily(AppFonts::load().mono);
    text->setFont(face);
    text->setPlainText(added && file["after"].toString().isEmpty()
                           ? QString("Empty file created.")
                       : deleted && file["before"].toString().isEmpty()
                           ? QString("Empty file deleted.")
                           : unifiedPreview(file["before"].toString(),
                                            file["after"].toString()));
    text->setFixedHeight(qBound(
        50, text->blockCount() * text->fontMetrics().lineSpacing() + 12, 220));
    m_layout->addWidget(text);
  }
  if (diff["omitted"].toInt() > 0) {
    auto *notice =
        new QLabel(QString("%1 binary or large file changes are recorded in "
                           "File changes; inline preview is limited.")
                       .arg(diff["omitted"].toInt()),
                   this);
    notice->setWordWrap(true);
    m_layout->addWidget(notice);
  }
  setVisible(!diff["files"].toArray().isEmpty() || diff["omitted"].toInt() > 0);
}
} // namespace ade
