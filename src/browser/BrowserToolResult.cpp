#include "BrowserToolResult.h"
#include <QBuffer>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
namespace ade {
namespace {
qsizetype encodedSize(const QString &text) {
  // Same JSON-string accounting as upstream capToolOutput(string), including
  // escaping; subtract only the array brackets used by this Qt serializer.
  return QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact).size() -
         2;
}
} // namespace
QString capBrowserModelOutput(const QString &output, bool error,
                              const QString &redactionValue) {
  // Redact before slicing so a token cannot become an unredactable fragment.
  // Engine performs its own redaction again when retaining the result; account
  // for that pass too (short synthetic/local keys can expand the marker).
  auto safe = output;
  if (!redactionValue.isEmpty())
    safe.replace(redactionValue, "[REDACTED]");
  const auto retainedSize = [&](QString value) {
    if (!redactionValue.isEmpty())
      value.replace(redactionValue, "[REDACTED]");
    return encodedSize(value);
  };
  const qsizetype budget =
      BrowserModelOutputBytes - (error ? 6 : 0); // Engine's "Error: " prefix.
  if (safe.size() <= budget && retainedSize(safe) <= budget)
    return safe;
  const QString marker =
      "\n[Browser tool output truncated at 40 KiB. Use a narrower query or "
      "fewer console logs. For full CDP data, use executeSandboxJs and persist "
      "needed data with API.createAttachment.]";
  qsizetype low = 0, high = qMin(safe.size(), budget), best = 0;
  while (low <= high) {
    const auto mid = low + (high - low) / 2;
    auto boundary = mid;
    if (boundary > 0 && boundary < safe.size() &&
        safe[boundary - 1].isHighSurrogate() && safe[boundary].isLowSurrogate())
      --boundary;
    if (retainedSize(safe.left(boundary) + marker) <= budget) {
      best = boundary;
      low = mid + 1;
    } else
      high = mid - 1;
  }
  return safe.left(best) + marker;
}
BrowserScreenshotResult decodeBrowserScreenshot(const QString &output) {
  BrowserScreenshotResult result;
  const auto fail = [&] {
    result.bytes.clear();
    result.error = "The browser screenshot was not a valid bounded PNG, JPEG "
                   "or WebP image. Retry with a smaller clip or use "
                   "executeSandboxJs and API.createAttachment.";
    return result;
  };
  // The bridge already enforces this UTF-16 transport bound. Keep this helper
  // defensive when called independently, without allocating an unbounded copy.
  if (output.size() > 2 * 1024 * 1024)
    return fail();
  QJsonParseError parse;
  const auto doc = QJsonDocument::fromJson(output.toUtf8(), &parse);
  if (parse.error != QJsonParseError::NoError || !doc.isObject() ||
      !doc.object().value("data").isString())
    return fail();
  const auto encoded = doc.object().value("data").toString();
  if (encoded.isEmpty() || encoded.size() > 2 * 1024 * 1024)
    return fail();
  const auto decoded = QByteArray::fromBase64Encoding(
      encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
  if (!decoded || decoded.decoded.isEmpty() ||
      decoded.decoded.size() > 2 * 1024 * 1024)
    return fail();
  result.bytes = decoded.decoded;
  QBuffer buffer(&result.bytes);
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer);
  const auto format = reader.format();
  const auto size = reader.size();
  if (!QList<QByteArray>{"png", "jpeg", "jpg", "webp"}.contains(format) ||
      !size.isValid() ||
      qint64(size.width()) * size.height() > 64 * 1024 * 1024 ||
      reader.read().isNull())
    return fail();
  const auto extension = format == "jpg" ? QByteArray("jpeg") : format;
  result.fileName = "browser-screenshot." + QString::fromLatin1(extension);
  result.mimeType = "image/" + QString::fromLatin1(extension);
  return result;
}
} // namespace ade
