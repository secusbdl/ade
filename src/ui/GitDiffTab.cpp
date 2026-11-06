#include "GitDiffTab.h"
#include "editor/CodeEditor.h"
#include "editor/TextFile.h"
#include "git/GitService.h"
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QVBoxLayout>

namespace ade {
GitDiffTab::GitDiffTab(GitService *service, QWidget *parent)
    : QWidget(parent), m_service(service) {
  setObjectName("gitDiffTab");
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(8, 8, 8, 8);
  auto *header = new QHBoxLayout;
  m_status = new QLabel;
  m_status->setTextFormat(Qt::PlainText);
  m_status->setWordWrap(true);
  m_status->setObjectName("gitDiffStatus");
  header->addWidget(m_status, 1);
  m_refresh = new QPushButton("Refresh");
  m_refresh->setObjectName("gitDiffRefresh");
  header->addWidget(m_refresh);
  layout->addLayout(header);
  auto *split = new QSplitter(Qt::Horizontal);
  split->setChildrenCollapsible(false);
  layout->addWidget(split, 1);
  auto addPane = [split](const QString &name, QLabel *&label,
                         CodeEditor *&editor) {
    auto *pane = new QWidget;
    auto *column = new QVBoxLayout(pane);
    column->setContentsMargins(0, 0, 0, 0);
    label = new QLabel;
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    column->addWidget(label);
    editor = new CodeEditor;
    editor->setObjectName(name);
    editor->setReadOnly(true);
    editor->setMinimumWidth(0);
    column->addWidget(editor, 1);
    split->addWidget(pane);
  };
  addPane("gitDiffOriginal", m_originalTitle, m_original);
  addPane("gitDiffModified", m_modifiedTitle, m_modified);
  connect(m_refresh, &QPushButton::clicked, this, &GitDiffTab::refresh);
  connect(m_original->verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int value) {
            QSignalBlocker block(m_modified->verticalScrollBar());
            m_modified->verticalScrollBar()->setValue(value);
          });
  connect(m_modified->verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int value) {
            QSignalBlocker block(m_original->verticalScrollBar());
            m_original->verticalScrollBar()->setValue(value);
          });
  if (service) {
    connect(service, &GitService::completed, this, &GitDiffTab::finish);
    connect(service, &QObject::destroyed, this, [this] {
      m_service = nullptr;
      m_request = 0;
      m_ready = false;
      m_error = "Git service is unavailable.";
      m_refresh->setEnabled(false);
      m_status->setText(m_error);
      emit stateChanged();
    });
  }
}
GitDiffTab::~GitDiffTab() { cancel(); }
QString GitDiffTab::tabKey(const QString &workspace, const QString &path,
                           bool staged, const QString &oldPath) {
  return QString::fromUtf8(
      QJsonDocument(
          QJsonArray{GitService::pathKey(workspace), path, staged, oldPath})
          .toJson(QJsonDocument::Compact));
}
QString GitDiffTab::tabKey() const {
  return tabKey(m_workspace, m_path, m_staged, m_oldPath);
}
void GitDiffTab::cancel() {
  const auto id = m_request;
  m_request = 0;
  if (id && m_service)
    m_service->cancel(id);
}
void GitDiffTab::load(const QString &workspace, const QString &path,
                      bool staged, const QString &oldPath) {
  cancel();
  m_workspace = GitService::canonicalPath(workspace);
  m_path = path;
  m_oldPath = oldPath;
  m_staged = staged;
  // Text and scrollbar signals are local presentation details, not a public
  // reentry boundary while replacing both sides of one atomic comparison.
  {
    QSignalBlocker a(m_original), b(m_modified),
        c(m_original->verticalScrollBar()), d(m_modified->verticalScrollBar());
    m_original->clear();
    m_modified->clear();
    m_original->setFileName(oldPath.isEmpty() ? path : oldPath);
    m_modified->setFileName(path);
  }
  refresh();
}
void GitDiffTab::refresh() {
  cancel();
  m_ready = false;
  m_error.clear();
  m_originalTitle->setText("Committed · " +
                           (m_oldPath.isEmpty() ? m_path : m_oldPath));
  m_modifiedTitle->setText((m_staged ? "Staged · " : "Working file · ") +
                           m_path);
  if (!m_service || m_workspace.isEmpty() || m_path.isEmpty()) {
    m_error = "The comparison workspace is unavailable.";
    m_status->setText(m_error);
    m_refresh->setEnabled(false);
    emit stateChanged();
    return;
  }
  m_status->setText("Loading comparison…");
  m_refresh->setEnabled(false);
  m_request = m_service->fileDiff(m_workspace, m_path, m_staged, m_oldPath);
  emit stateChanged();
}
void GitDiffTab::finish(quint64 id, const GitResult &result) {
  if (!id || id != m_request || result.operation != GitOperation::FileDiff)
    return;
  m_request = 0;
  m_refresh->setEnabled(true);
  TextFile before, after;
  QString decodeError;
  const bool decoded =
      result.ok &&
      TextFileIO::decode(result.fileDiff.original, &before, &decodeError) &&
      TextFileIO::decode(result.fileDiff.modified, &after, &decodeError);
  {
    QSignalBlocker a(m_original), b(m_modified),
        c(m_original->verticalScrollBar()), d(m_modified->verticalScrollBar());
    if (!decoded) {
      m_original->clear();
      m_modified->clear();
      m_error = result.ok ? "Binary or unsupported text: this file cannot be "
                            "displayed as a text comparison."
                          : result.error.message;
      m_status->setText(m_error);
    } else {
      m_original->setPlainText(before.text);
      m_modified->setPlainText(after.text);
      m_ready = true;
      m_originalTitle->setText((result.fileDiff.originalExists
                                    ? "Committed · "
                                    : "Not present in HEAD · ") +
                               (m_oldPath.isEmpty() ? m_path : m_oldPath));
      m_modifiedTitle->setText(
          (result.fileDiff.modifiedExists
               ? (m_staged ? "Staged · " : "Working file · ")
               : "Deleted · ") +
          m_path);
      m_status->setText(m_staged ? "Committed → staged snapshot · Read only"
                                 : "Committed → working file · Read only");
    }
  }
  emit stateChanged();
}
} // namespace ade
