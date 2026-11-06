#include "Icons.h"
#include "MainWindow.h"
#include "SettingsPage.h"
#include "runtime/AgentToolHost.h"
#include "shell/ShellService.h"
#include "terminal/TerminalWidget.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyleHints>
#include <QTabWidget>
#include <QVBoxLayout>
namespace ade {
void MainWindow::showAgentShell(const QString &id, const QString &sessionId) {
  const auto *chat = find(id);
  const QPointer<AgentToolHost> host = m_toolHosts.value(id);
  if (!chat || chat->archived || !host ||
      !host->shells()->ownsSession(sessionId))
    return;
  const auto title = chat->title.left(24);
  const QPointer<MainWindow> self(this);
  for (int i = 0; i < m_tabs->count(); ++i) {
    const QPointer<QWidget> page = m_tabs->widget(i);
    if (page->property("shellAgentId") != id ||
        page->property("shellSessionId") != sessionId)
      continue;
    m_tabs->setCurrentWidget(page);
    if (self && page)
      revealContent();
    if (self && page)
      if (auto *terminal = page->findChild<TerminalWidget *>())
        terminal->setFocus();
    return;
  }
  const QPointer<ShellService> service = host->shells();
  auto *model = service->sessionModel(sessionId);
  auto *process = service->sessionProcess(sessionId);
  if (!model || !process)
    return;
  auto *page = new QWidget(m_tabs);
  page->setObjectName("agentShellPage");
  page->setProperty("shellAgentId", id);
  page->setProperty("shellSessionId", sessionId);
  page->setProperty("ownerConversationId", id);
  auto *layout = new QVBoxLayout(page);
  layout->setContentsMargins(6, 4, 6, 4);
  auto *tools = new QHBoxLayout;
  auto *status = new QLabel(page);
  status->setObjectName("agentShellStatus");
  status->setTextFormat(Qt::PlainText);
  status->setWordWrap(true);
  auto *stop = new QPushButton("Stop session", page);
  stop->setObjectName("stopAgentShell");
  tools->addWidget(status, 1);
  tools->addWidget(stop);
  layout->addLayout(tools);
  auto *terminal = new TerminalWidget(model, process, page);
  terminal->setObjectName("agentShellTerminal");
  layout->addWidget(terminal, 1);
  const auto appearance =
      m_settings->preference("personalization.appearance").toString();
  terminal->setDarkTheme(
      appearance == "dark" ||
      (appearance == "system" &&
       qApp->styleHints()->colorScheme() == Qt::ColorScheme::Dark));
  terminal->setZoomPercent(
      m_settings->preference("personalization.uiZoom").toInt(100));
  const auto refresh = [service, sessionId, status, stop] {
    if (!service)
      return;
    for (const auto &value : service->snapshot()["sessions"].toArray()) {
      const auto row = value.toObject();
      if (row["id"] != sessionId)
        continue;
      status->setText((row["running"].toBool() ? QString("PowerShell · ")
                                               : QString("Exited · ")) +
                      row["cwd"].toString());
      stop->setEnabled(row["running"].toBool());
      return;
    }
    status->setText("Session unavailable");
    stop->setEnabled(false);
  };
  connect(service, &ShellService::manifestChanged, page, refresh);
  refresh();
  connect(stop, &QPushButton::clicked, page, [service, sessionId] {
    if (service && service->ownsSession(sessionId))
      service->execute(
          "ui-stop-" + QUuid::createUuid().toString(QUuid::WithoutBraces),
          "executeShellCommand",
          {{"session_id", sessionId},
           {"kill", true},
           {"explanation", "The user stopped this shell session."}});
  });
  connect(terminal, &TerminalWidget::errorOccurred, this,
          [this](const QString &error) { m_status->setText(error); });
  connect(host, &QObject::destroyed, page, &QObject::deleteLater);
  const QPointer<QWidget> guard(page);
  const QPointer<TerminalWidget> terminalGuard(terminal);
  int index = -1;
  {
    const QSignalBlocker blocked(m_tabs);
    index = m_tabs->addTab(page, icon("terminal"), "Shell · " + title);
    m_tabs->setCurrentIndex(index);
  }
  if (!self || !guard || !service)
    return;
  emit m_tabs->currentChanged(index);
  if (self && guard)
    revealContent();
  if (self && terminalGuard)
    terminalGuard->setFocus();
}
} // namespace ade
