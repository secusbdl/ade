#include "BrowserScreenshot.h"
#include <QBuffer>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringList>
#include <cmath>
namespace ade {
namespace {
// Upstream compressImageToTargetSize quality steps and resize stop rule.
constexpr int Qualities[] = {85, 70, 50, 30};
constexpr double MinScaledSide = 100;
// Each ladder is bounded: 1 + 4 + metrics + (halvings x 4) stays well below.
constexpr int MaxAttempts = 48;
constexpr qsizetype MaxMetricsChars = 64 * 1024;
bool finitePositive(const QJsonValue &value, double max) {
  return value.isDouble() && std::isfinite(value.toDouble()) &&
         value.toDouble() > 0 && value.toDouble() <= max;
}
bool finiteValue(const QJsonValue &value) {
  return value.isDouble() && std::isfinite(value.toDouble()) &&
         std::abs(value.toDouble()) <= 1e7;
}
QString formatName(const QJsonObject &parameters) {
  const auto format = parameters.value("format").toString().toLower();
  if (format == "jpeg")
    return "JPEG";
  if (format == "webp")
    return "WebP";
  return "PNG";
}
QString number(double value) { return QString::number(value, 'g', 6); }
} // namespace

BrowserScreenshotImageCheck
checkBrowserScreenshotImage(const QByteArray &bytes, QByteArray *format,
                            QSize *size) {
  using Check = BrowserScreenshotImageCheck;
  if (bytes.isEmpty() || bytes.size() > BrowserScreenshotTransportChars)
    return Check::Invalid;
  QBuffer buffer;
  buffer.setData(bytes);
  if (!buffer.open(QIODevice::ReadOnly))
    return Check::Invalid;
  QImageReader reader(&buffer);
  auto detected = reader.format();
  if (detected == "jpg")
    detected = "jpeg";
  const auto dimensions = reader.size();
  if (!QList<QByteArray>{"png", "jpeg", "webp"}.contains(detected) ||
      !dimensions.isValid())
    return Check::Invalid;
  // Header dimensions only: an over-bound image is never decoded.
  if (dimensions.width() > BrowserScreenshotMaxSide ||
      dimensions.height() > BrowserScreenshotMaxSide ||
      qint64(dimensions.width()) * dimensions.height() >
          BrowserScreenshotMaxPixels)
    return Check::TooLarge;
  if (reader.read().isNull())
    return Check::Invalid;
  if (format)
    *format = detected;
  if (size)
    *size = dimensions;
  return Check::Valid;
}

bool validateBrowserScreenshotImage(const QByteArray &bytes,
                                    QByteArray *format, QSize *size) {
  return checkBrowserScreenshotImage(bytes, format, size) ==
         BrowserScreenshotImageCheck::Valid;
}

QString browserScreenshotExtension(const QByteArray &bytes) {
  // Header sniff only: callers pass bytes that were already validated.
  QBuffer buffer;
  buffer.setData(bytes);
  if (!buffer.open(QIODevice::ReadOnly))
    return "png";
  const auto format = QImageReader(&buffer).format();
  if (format == "jpeg" || format == "jpg")
    return "jpeg";
  return format == "webp" ? "webp" : "png";
}

BrowserScreenshotLadder::BrowserScreenshotLadder(const QJsonObject &parameters,
                                                 qsizetype transportChars)
    : m_original(parameters) {
  m_fit = qBound<qsizetype>(16 * 1024, transportChars,
                            BrowserScreenshotTransportChars) -
          BrowserScreenshotEnvelopeChars;
  int ceiling = 101;
  const auto quality = parameters.value("quality");
  if (parameters.value("format").toString().toLower() == "jpeg" &&
      quality.isDouble() && quality.toDouble() >= 0 &&
      quality.toDouble() <= 100)
    ceiling = int(quality.toDouble());
  for (const int value : Qualities)
    if (value < ceiling)
      m_qualities.append(value);
}

BrowserScreenshotLadder::Request BrowserScreenshotLadder::request() const {
  if (m_stage == Stage::Metrics)
    return {"Page.getLayoutMetrics", {}};
  return {"Page.captureScreenshot", captureParameters()};
}

QJsonObject BrowserScreenshotLadder::captureParameters() const {
  auto parameters = m_original;
  if (m_scaled) {
    auto clip = m_clip;
    clip.insert("scale", m_scale);
    parameters.insert("clip", clip);
  }
  if (m_quality >= 0) {
    parameters.insert("format", "jpeg");
    parameters.insert("quality", m_qualities.at(m_quality));
  }
  return parameters;
}

BrowserScreenshotLadder::Step
BrowserScreenshotLadder::fail(const QString &error) {
  m_error = error;
  m_bytes.clear();
  m_reply.clear();
  m_step = Step::Failed;
  return m_step;
}

BrowserScreenshotLadder::Step BrowserScreenshotLadder::accept(
    const QString &reply) {
  if (m_step != Step::Request)
    return m_step;
  if (++m_attempts > MaxAttempts)
    return fail("The browser screenshot fallback exceeded its attempt bound.");
  if (m_stage == Stage::Metrics) {
    if (reply.size() > MaxMetricsChars)
      return fail("The browser layout metrics were too large.");
    const auto metrics =
        QJsonDocument::fromJson(reply.toUtf8()).object();
    // Metrics are read only when the caller gave no clip. Without
    // captureBeyondViewport Chromium captures the visible viewport; with it,
    // WebView2 captures the whole document (verified by
    // fullPageFlagWithoutClipKeepsCapturedArea). The fallback keeps the area
    // the caller's PNG covered, as upstream re-encodes that same image.
    const bool beyond = m_original.value("captureBeyondViewport").toBool();
    QJsonObject area = beyond ? metrics.value("cssContentSize").toObject()
                              : metrics.value("cssVisualViewport").toObject();
    if (area.isEmpty() && !beyond)
      area = metrics.value("cssLayoutViewport").toObject();
    const auto x = beyond ? area.value("x") : area.value("pageX");
    const auto y = beyond ? area.value("y") : area.value("pageY");
    const auto width = beyond ? area.value("width") : area.value("clientWidth");
    const auto height = beyond ? area.value("height") : area.value("clientHeight");
    if (!finiteValue(x) || !finiteValue(y) || !finitePositive(width, 1e6) ||
        !finitePositive(height, 1e6))
      return fail("The browser viewport size could not be read for a smaller "
                  "screenshot.");
    m_clip = {{"x", x}, {"y", y}, {"width", width}, {"height", height}};
    m_scale = 1;
    return halve();
  }
  if (reply.size() > m_fit) {
    m_transportBound = true;
    return advance();
  }
  return finish(reply);
}

BrowserScreenshotLadder::Step BrowserScreenshotLadder::advance() {
  if (m_quality + 1 < m_qualities.size()) {
    ++m_quality;
    m_stage = Stage::Quality;
    return m_step;
  }
  return resize();
}

BrowserScreenshotLadder::Step BrowserScreenshotLadder::resize() {
  if (m_scaled)
    return halve();
  const auto clip = m_original.value("clip").toObject();
  const auto scale = clip.value("scale");
  if (finiteValue(clip.value("x")) && finiteValue(clip.value("y")) &&
      finitePositive(clip.value("width"), 1e6) &&
      finitePositive(clip.value("height"), 1e6) &&
      (scale.isUndefined() || finitePositive(scale, 16))) {
    m_clip = clip;
    m_clip.remove("scale");
    m_scale = scale.isUndefined() ? 1 : scale.toDouble();
    return halve();
  }
  m_stage = Stage::Metrics;
  return m_step;
}

BrowserScreenshotLadder::Step BrowserScreenshotLadder::halve() {
  const double width = m_clip.value("width").toDouble() * m_scale;
  const double height = m_clip.value("height").toDouble() * m_scale;
  if (width <= MinScaledSide || height <= MinScaledSide)
    return fail(QString("The browser screenshot does not fit the %1 MiB "
                        "transport and %2 px image bounds even as JPEG at "
                        "quality 30 and the smallest capture scale. Capture a "
                        "smaller clip.")
                    .arg(BrowserScreenshotTransportChars / (1024 * 1024))
                    .arg(BrowserScreenshotMaxSide));
  m_scale /= 2;
  m_scaled = true;
  // Upstream restarts its quality steps on the resized image.
  m_quality = m_qualities.isEmpty() ? -1 : 0;
  m_stage = Stage::Quality;
  return m_step;
}

BrowserScreenshotLadder::Step
BrowserScreenshotLadder::finish(const QString &reply) {
  const QString invalid = QString::fromLatin1(BrowserScreenshotInvalidMessage);
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(reply.toUtf8(), &parse);
  if (parse.error != QJsonParseError::NoError || !document.isObject() ||
      !document.object().value("data").isString())
    return fail(invalid);
  const auto encoded = document.object().value("data").toString();
  const auto decoded = QByteArray::fromBase64Encoding(
      encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
  if (!decoded)
    return fail(invalid);
  const auto check = checkBrowserScreenshotImage(decoded.decoded, &m_format);
  if (check == BrowserScreenshotImageCheck::TooLarge) {
    // Lower JPEG quality cannot shrink dimensions: halve the capture scale
    // (requested clip or layout metrics), like an oversized reply would.
    m_imageBound = true;
    return resize();
  }
  if (check != BrowserScreenshotImageCheck::Valid)
    return fail(invalid);
  m_bytes = decoded.decoded;
  m_reply = reply;
  if (m_quality >= 0 || m_scaled) {
    const QString captured =
        m_quality >= 0
            ? QString("JPEG at quality %1").arg(m_qualities.at(m_quality))
            : QString("the requested format");
    QStringList bounds;
    if (m_transportBound)
      bounds.append(QString("the %1 MiB browser transport")
                        .arg(BrowserScreenshotTransportChars / (1024 * 1024)));
    if (m_imageBound)
      bounds.append(QString("the %1 px image bound")
                        .arg(BrowserScreenshotMaxSide));
    m_note = QString("The %1 capture exceeded %2, so ADE captured %3%4.")
                 .arg(formatName(m_original), bounds.join(" and "))
                 .arg(captured,
                      m_scaled ? " at capture scale " + number(m_scale)
                               : QString());
    m_reply = QString::fromUtf8(
        QJsonDocument(QJsonObject{{"data", encoded},
                                  {"adeScreenshotFallback", m_note}})
            .toJson(QJsonDocument::Compact));
  }
  m_step = Step::Done;
  return m_step;
}

QString BrowserScreenshotLadder::output() const {
  return m_step == Step::Done ? m_reply : QString();
}

QString BrowserScreenshotLadder::mimeType() const {
  return m_format.isEmpty() ? QString()
                            : "image/" + QString::fromLatin1(m_format);
}
} // namespace ade
