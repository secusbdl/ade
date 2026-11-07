#pragma once
#include "BrowserScreenshot.h"
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QTimer>
#include <functional>
#include <memory>

namespace ade {
class BrowserWidget;

// User-driven inspection of an existing tab. Page data is captured as an
// attachment, never interpreted as host commands or filesystem authority.
class BrowserInspector final : public QObject {
  Q_OBJECT
public:
  explicit BrowserInspector(BrowserWidget *browser, QObject *parent = nullptr);
  ~BrowserInspector() override;
  bool isSelecting() const { return m_selecting; }
  bool isBusy() const { return m_busy; }
  QJsonObject device() const { return m_device; }
  double viewportScale() const;
  static QJsonArray devicePresets();
  // Lowers the screenshot transport cutoff (bounded to the 2 MiB default).
  void setScreenshotTransportChars(qsizetype chars);
public slots:
  void setSelecting(bool enabled);
  void inspectNode(int backendNodeId);
  void setDevice(const QJsonObject &device);
  void captureScreenshot();
  void setViewportSize(const QSize &size);
signals:
  void selectingChanged(bool enabled);
  void busyChanged(bool busy);
  void elementSelected(const QJsonObject &element,
                       const QString &captureContext);
  // PNG, or JPEG when the PNG exceeded the transport (see BrowserScreenshot.h).
  void screenshotCaptured(const QByteArray &image,
                          const QString &captureContext);
  void deviceChanged(const QJsonObject &device);
  void failed(const QString &message);

private:
  using Reply = std::function<void(const QJsonObject &, const QString &)>;
  using RawReply = std::function<void(const QString &, bool)>;
  struct Pending {
    Reply reply;
    RawReply raw;
    QPointer<QTimer> timer;
    quint64 epoch = 0;
  };
  void call(const QString &method, const QJsonObject &parameters, Reply reply);
  // Unparsed reply for callers that bound and parse it themselves.
  void callRaw(const QString &method, const QJsonObject &parameters,
               RawReply reply);
  void dispatch(const QString &method, const QJsonObject &parameters,
                Pending pending);
  void continueScreenshot(const std::shared_ptr<BrowserScreenshotLadder> &ladder,
                          const QString &context, quint64 epoch);
  void resetForNavigation();
  void finishSelection(const QString &error = {});
  void emitFailure(const QString &error);
  QPointer<BrowserWidget> m_browser;
  QHash<QString, Pending> m_pending;
  quint64 m_epoch = 1;
  bool m_selecting = false;
  bool m_busy = false;
  QString m_selectionContext;
  QJsonObject m_device;
  QSize m_viewportSize;
  QSize m_appliedViewportSize;
  qsizetype m_screenshotTransportChars = BrowserScreenshotTransportChars;
};
} // namespace ade
