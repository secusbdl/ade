#include "BrowserViewport.h"
#include "Icons.h"
#include "MainWindow.h"
#include "attachments/AttachmentStore.h"
#include "browser/BrowserInspector.h"
#include "browser/BrowserScreenshot.h"
#include "browser/browserwidget.h"
#include <QActionGroup>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>

namespace ade {
void MainWindow::attachBrowserContext(const QString &id, const QString &name,
                                      const QByteArray &bytes) {
  auto *session = find(id);
  if (id != m_currentId || !session || session->archived || bytes.isEmpty())
    return;
  if (session->draftAttachments.size() >= 32) {
    m_status->setText("A message can contain at most 32 attachments.");
    return;
  }
  QString error;
  const auto ref =
      AttachmentStore(m_store.directory()).write(id, name, bytes, &error);
  if (ref.isEmpty()) {
    m_status->setText(error);
    return;
  }
  const auto previous = session->draftAttachments;
  session->draftAttachments.append(ref.toJson());
  if (!saveSessions())
    session->draftAttachments = previous;
  refreshAttachments();
}
QWidget *MainWindow::configureBrowserTools(QWidget *page,
                                           BrowserWidget *browser,
                                           QHBoxLayout *nav) {
  if (auto *address = page->findChild<QLineEdit *>("browserAddress"))
    address->setMinimumWidth(40);
  auto *inspector = new BrowserInspector(browser, page);
  inspector->setObjectName("browserInspector");
  auto makeButton = [page](const QString &image, const QString &name,
                           const QString &hint) {
    auto *result = new QToolButton(page);
    result->setObjectName(name);
    result->setIcon(icon(image));
    result->setToolTip(hint);
    result->setAccessibleName(hint);
    result->setAutoRaise(true);
    result->setFixedSize(28, 28);
    return result;
  };
  auto *select = makeButton("select", "selectBrowserElement",
                            "Select an element to add to your message");
  select->setCheckable(true);
  auto *screenshot = makeButton("camera", "captureBrowserScreenshot",
                                "Attach a screenshot of this page");
  auto *devices =
      makeButton("device", "browserDeviceEmulation", "Device preview");
  auto *tools = makeButton("code", "openBrowserDevTools", "Developer tools");
  nav->addWidget(select);
  nav->addWidget(screenshot);
  nav->addWidget(devices);
  nav->addWidget(tools);
  auto *menu = new QMenu(devices);
  devices->setMenu(menu);
  devices->setPopupMode(QToolButton::InstantPopup);
  auto *group = new QActionGroup(menu);
  group->setExclusive(true);
  auto *desktop = menu->addAction("Desktop");
  desktop->setCheckable(true);
  desktop->setChecked(true);
  group->addAction(desktop);
  connect(desktop, &QAction::triggered, inspector,
          [inspector] { inspector->setDevice({}); });
  for (const auto &value : BrowserInspector::devicePresets()) {
    const auto preset = value.toObject();
    auto *action = menu->addAction(preset["label"].toString() +
                                   QString("  %1 × %2")
                                       .arg(preset["width"].toInt())
                                       .arg(preset["height"].toInt()));
    action->setCheckable(true);
    action->setData(preset["id"].toString());
    group->addAction(action);
    connect(action, &QAction::triggered, inspector,
            [inspector, preset] { inspector->setDevice(preset); });
  }
  menu->addSeparator();
  auto *rotate = menu->addAction("Rotate device");
  rotate->setEnabled(false);
  connect(rotate, &QAction::triggered, inspector, [inspector] {
    auto device = inspector->device();
    if (device.isEmpty())
      return;
    const auto width = device.value("width");
    device["width"] = device.value("height");
    device["height"] = width;
    inspector->setDevice(device);
  });
  auto *custom = menu->addAction("Custom viewport…");
  connect(custom, &QAction::triggered, page, [page, inspector] {
    auto *dialog = new QDialog(page);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Custom viewport");
    auto *layout = new QFormLayout(dialog);
    auto *width = new QSpinBox(dialog);
    auto *height = new QSpinBox(dialog);
    auto *scale = new QDoubleSpinBox(dialog);
    width->setRange(200, 4096);
    height->setRange(200, 4096);
    scale->setRange(0.5, 4);
    scale->setSingleStep(0.25);
    const auto device = inspector->device();
    width->setValue(device["width"].toInt(1280));
    height->setValue(device["height"].toInt(720));
    scale->setValue(device["deviceScaleFactor"].toDouble(1));
    layout->addRow("Width", width);
    layout->addRow("Height", height);
    layout->addRow("Pixel ratio", scale);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog,
            [dialog, inspector, width, height, scale] {
              const QJsonObject config{{"id", "custom"},
                                       {"width", width->value()},
                                       {"height", height->value()},
                                       {"deviceScaleFactor", scale->value()},
                                       {"mobile", false}};
              const QPointer<BrowserInspector> safe(inspector);
              dialog->accept();
              if (safe)
                safe->setDevice(config);
            });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    dialog->open();
  });
  connect(inspector, &BrowserInspector::deviceChanged, page,
          [devices, rotate, group](const QJsonObject &device) {
            rotate->setEnabled(!device.isEmpty());
            devices->setToolTip(
                device.isEmpty()
                    ? "Device preview"
                    : QString("%1 × %2 · %3× pixels")
                          .arg(device["width"].toInt())
                          .arg(device["height"].toInt())
                          .arg(device["deviceScaleFactor"].toDouble()));
            for (auto *action : group->actions())
              action->setChecked(device.isEmpty()
                                     ? action->data().isNull()
                                     : action->data().toString() ==
                                           device["id"].toString());
          });
  connect(select, &QToolButton::clicked, page,
          [this, inspector, select](bool enabled) {
            const auto *session = current();
            if (enabled && (!session || session->archived)) {
              const QSignalBlocker block(select);
              select->setChecked(false);
              return;
            }
            if (enabled)
              inspector->setProperty("captureAgentId", m_currentId);
            inspector->setSelecting(enabled);
          });
  connect(screenshot, &QToolButton::clicked, page, [this, inspector] {
    if (!current() || current()->archived)
      return;
    inspector->setProperty("captureAgentId", m_currentId);
    inspector->captureScreenshot();
  });
  connect(tools, &QToolButton::clicked, browser, &BrowserWidget::openDevTools);
  connect(inspector, &BrowserInspector::selectingChanged, select,
          [select](bool selecting) {
            const QSignalBlocker block(select);
            select->setChecked(selecting);
          });
  auto updateEnabled = [browser, select, screenshot, devices, tools,
                        inspector] {
    const bool ready = browser->isReady() && !inspector->isBusy();
    select->setEnabled(ready);
    screenshot->setEnabled(ready);
    devices->setEnabled(ready);
    tools->setEnabled(browser->isReady());
  };
  connect(inspector, &BrowserInspector::busyChanged, page, updateEnabled);
  connect(browser, &BrowserWidget::readyChanged, page, updateEnabled);
  updateEnabled();
  connect(inspector, &BrowserInspector::failed, page,
          [this, group, inspector](const QString &error) {
            m_status->setText(error);
            const auto device = inspector->device();
            for (auto *action : group->actions())
              action->setChecked(device.isEmpty()
                                     ? action->data().isNull()
                                     : action->data().toString() ==
                                           device["id"].toString());
          });
  connect(inspector, &BrowserInspector::elementSelected, page,
          [this, page](QJsonObject element, const QString &id) {
            element.insert("tab_id", page->property("browserTabId").toString());
            attachBrowserContext(
                id, element["tagName"].toString() + ".swdomelement",
                QJsonDocument(element).toJson(QJsonDocument::Indented));
          });
  connect(inspector, &BrowserInspector::screenshotCaptured, page,
          [this](const QByteArray &image, const QString &id) {
            // PNG normally; JPEG when the PNG exceeded the screenshot transport.
            attachBrowserContext(id,
                                 "Page screenshot." +
                                     browserScreenshotExtension(image),
                                 image);
          });
  return new BrowserViewport(browser, inspector, page);
}
} // namespace ade
