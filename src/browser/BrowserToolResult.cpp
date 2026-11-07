#include "BrowserToolResult.h"
#include "BrowserScreenshot.h"
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
    result.error = QString::fromLatin1(BrowserScreenshotInvalidMessage);
    return result;
  };
  // The bridge already enforces this UTF-16 transport bound. Keep this helper
  // defensive when called independently, without allocating an unbounded copy.
  if (output.size() > BrowserScreenshotTransportChars)
    return fail();
  QJsonParseError parse;
  const auto doc = QJsonDocument::fromJson(output.toUtf8(), &parse);
  if (parse.error != QJsonParseError::NoError || !doc.isObject() ||
      !doc.object().value("data").isString())
    return fail();
  const auto encoded = doc.object().value("data").toString();
  if (encoded.isEmpty() || encoded.size() > BrowserScreenshotTransportChars)
    return fail();
  const auto decoded = QByteArray::fromBase64Encoding(
      encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
  if (!decoded || decoded.decoded.isEmpty() ||
      decoded.decoded.size() > BrowserScreenshotTransportChars)
    return fail();
  QByteArray format;
  if (!validateBrowserScreenshotImage(decoded.decoded, &format))
    return fail();
  result.bytes = decoded.decoded;
  result.fileName = "browser-screenshot." + QString::fromLatin1(format);
  result.mimeType = "image/" + QString::fromLatin1(format);
  // Present only when the bridge fell back from an oversized capture.
  const auto note = doc.object().value("adeScreenshotFallback");
  if (note.isString() && note.toString().size() <= 512)
    result.note = note.toString();
  return result;
}
} // namespace ade
