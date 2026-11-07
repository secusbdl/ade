#pragma once
#include <QByteArray>
#include <QString>
namespace ade {
inline constexpr qsizetype BrowserModelOutputBytes = 40 * 1024;
// Model-facing only. The raw CDP/sandbox transport keeps its independent limit.
QString capBrowserModelOutput(const QString &output, bool error = false,
                              const QString &redactionValue = {});
struct BrowserScreenshotResult {
  QByteArray bytes;
  QString fileName, mimeType, error;
  QString note; // Transport fallback explanation for the model, if any.
};
BrowserScreenshotResult decodeBrowserScreenshot(const QString &output);
} // namespace ade
