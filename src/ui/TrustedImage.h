#pragma once
#include <QBuffer>
#include <QImageReader>
#include <QSet>
namespace ade {
inline QImage readTrustedImage(const QByteArray &bytes,
                               QString *error = nullptr,
                               QSize *originalSize = nullptr) {
  if (error)
    error->clear();
  if (originalSize)
    *originalSize = {};
  if (bytes.size() > 16 * 1024 * 1024) {
    if (error)
      *error = "Image exceeds the 16 MiB display limit.";
    return {};
  }
  QByteArray format;
  if (bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a")))
    format = "png";
  else if (bytes.startsWith(QByteArray::fromHex("ffd8ff")))
    format = "jpeg";
  else if (bytes.startsWith("GIF87a") || bytes.startsWith("GIF89a"))
    format = "gif";
  else if (bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WEBP")
    format = "webp";
  else if (bytes.startsWith("BM"))
    format = "bmp";
  else if (bytes.startsWith(QByteArray::fromHex("00000100")))
    format = "ico";
  else if (bytes.mid(4, 4) == "ftyp" && (bytes.mid(8, 24).contains("avif") ||
                                         bytes.mid(8, 24).contains("avis")))
    format = "avif";
  if (format.isEmpty()) {
    if (error)
      *error = "Unsupported image format.";
    return {};
  }
  QBuffer buffer;
  buffer.setData(bytes);
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer, format);
  reader.setAutoDetectImageFormat(false);
  reader.setAutoTransform(true);
  const auto dimensions = reader.size();
  if (!reader.canRead() || !dimensions.isValid() ||
      qint64(dimensions.width()) * dimensions.height() > 32 * 1024 * 1024 ||
      dimensions.width() > 32768 || dimensions.height() > 32768) {
    if (error)
      *error = "Unsupported image or image exceeds the display limit.";
    return {};
  }
  if (originalSize)
    *originalSize = dimensions;
  if (dimensions.width() > 2048 || dimensions.height() > 2048)
    reader.setScaledSize(dimensions.scaled(2048, 2048, Qt::KeepAspectRatio));
  auto image = reader.read();
  if (image.isNull() && error)
    *error = reader.errorString();
  return image;
}
} // namespace ade
