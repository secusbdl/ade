#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QSize>
#include <QString>
namespace ade {
// UTF-16 code units of one CDP reply; the same bound as the browser bridge.
inline constexpr qsizetype BrowserScreenshotTransportChars = 2 * 1024 * 1024;
// Headroom kept below the transport for the fallback note and JSON envelope.
inline constexpr qsizetype BrowserScreenshotEnvelopeChars = 1024;
// Decoded-image bounds shared by every screenshot consumer.
inline constexpr int BrowserScreenshotMaxSide = 16384;
inline constexpr qint64 BrowserScreenshotMaxPixels = 64ll * 1024 * 1024;

// One message for every invalid-image path (ladder and attachment decode).
inline constexpr char BrowserScreenshotInvalidMessage[] =
    "The browser screenshot was not a valid bounded PNG, JPEG or WebP image. "
    "Retry with a smaller clip or use executeSandboxJs and "
    "API.createAttachment.";

// TooLarge: a PNG, JPEG or WebP header whose dimensions exceed the side or
// pixel bound; it is never decoded. Invalid: anything else that is not a
// complete image. format and size are set only for Valid.
enum class BrowserScreenshotImageCheck { Valid, Invalid, TooLarge };
BrowserScreenshotImageCheck
checkBrowserScreenshotImage(const QByteArray &bytes,
                            QByteArray *format = nullptr,
                            QSize *size = nullptr);
// Accepts only bounded PNG, JPEG or WebP data that decodes completely.
bool validateBrowserScreenshotImage(const QByteArray &bytes,
                                    QByteArray *format = nullptr,
                                    QSize *size = nullptr);
// File extension for validated capture bytes: "png", "jpeg" or "webp".
QString browserScreenshotExtension(const QByteArray &bytes);

// Native equivalent of Stagewise compressImageToTargetSize
// (browsing-tab-controller.ts): when the requested capture does not fit the
// transport, ask the browser itself for JPEG at qualities 85, 70, 50 and 30,
// then halve the capture scale and retry the qualities until the shorter side
// would be 100 CSS pixels or less. A fitting reply whose image exceeds the
// decoded side/pixel bounds skips straight to halving. Without a requested clip
// the halved area is the area the caller's capture covered: the visual viewport,
// or the whole document with captureBeyondViewport (as WebView2 captures it).
// Nothing larger than the
// transport bound is ever parsed or decoded. The caller owns dispatch,
// deadlines and cancellation.
class BrowserScreenshotLadder {
public:
  enum class Step { Request, Done, Failed };
  struct Request {
    QString method;
    QJsonObject params;
  };
  explicit BrowserScreenshotLadder(
      const QJsonObject &parameters,
      qsizetype transportChars = BrowserScreenshotTransportChars);
  Request request() const;
  // Feed the successful reply of request(); error replies stay with the caller.
  Step accept(const QString &reply);
  Step step() const { return m_step; }
  // Done: the CDP reply, or {"data","adeScreenshotFallback"} after a fallback.
  QString output() const;
  QByteArray bytes() const { return m_bytes; }
  QString mimeType() const;
  QString note() const { return m_note; }
  QString error() const { return m_error; }
  int attempts() const { return m_attempts; }
  bool fellBack() const { return !m_note.isEmpty(); }

private:
  enum class Stage { Requested, Quality, Metrics };
  Step fail(const QString &error);
  Step advance();
  Step resize();
  Step halve();
  Step finish(const QString &reply);
  QJsonObject captureParameters() const;
  QJsonObject m_original;
  QList<int> m_qualities;
  qsizetype m_fit = 0;
  Stage m_stage = Stage::Requested;
  Step m_step = Step::Request;
  int m_quality = -1;
  int m_attempts = 0;
  bool m_scaled = false;
  bool m_transportBound = false, m_imageBound = false;
  double m_scale = 1;
  QJsonObject m_clip;
  QString m_reply, m_note, m_error;
  QByteArray m_bytes, m_format;
};
} // namespace ade
