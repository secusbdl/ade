#pragma once
#include "browser/BrowserInspector.h"
#include "browser/browserwidget.h"
#include <QPointer>
#include <QResizeEvent>
#include <QTimer>
#include <QWidget>
namespace ade {
class BrowserViewport final : public QWidget {
public:
  BrowserViewport(BrowserWidget *browser, BrowserInspector *inspector,
                  QWidget *parent = nullptr)
      : QWidget(parent), m_browser(browser), m_inspector(inspector) {
    setObjectName("browserViewport");
    setMinimumSize(0, 0);
    browser->setParent(this);
    m_resize.setSingleShot(true);
    m_resize.setInterval(30);
    connect(&m_resize, &QTimer::timeout, this, [this] {
      if (m_inspector)
        m_inspector->setViewportSize(size());
      arrange();
    });
    connect(inspector, &BrowserInspector::deviceChanged, this,
            [this] { arrange(); });
    connect(inspector, &BrowserInspector::busyChanged, this, [this](bool busy) {
      if (!busy)
        m_resize.start();
    });
  }

protected:
  void resizeEvent(QResizeEvent *event) override {
    QWidget::resizeEvent(event);
    arrange();
    m_resize.start();
  }

private:
  void arrange() {
    if (!m_browser || !m_inspector)
      return;
    const auto device = m_inspector->device();
    if (device.isEmpty()) {
      m_browser->setGeometry(rect());
      return;
    }
    const auto scale = m_inspector->viewportScale();
    const QSize pixels(qMax(1, qRound(device["width"].toInt() * scale)),
                       qMax(1, qRound(device["height"].toInt() * scale)));
    m_browser->setGeometry(QRect(QPoint((width() - pixels.width()) / 2,
                                        (height() - pixels.height()) / 2),
                                 pixels));
  }
  QPointer<BrowserWidget> m_browser;
  QPointer<BrowserInspector> m_inspector;
  QTimer m_resize;
};
} // namespace ade
