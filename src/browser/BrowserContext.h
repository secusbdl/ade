#pragma once
#include <QList>
#include <QString>

namespace ade {
// Copied host state only: no QObject, callback, page content or runtime
// authority.
struct BrowserContextTab {
  QString id, title, url;
  qint64 lastFocusedAt = 0;
  quint64 focusOrder = 0;
};
struct BrowserContextSnapshot {
  bool available = false;
  QString activeTabId;
  QList<BrowserContextTab> tabs;
  qsizetype omittedTabs = 0;
};
inline constexpr qsizetype BrowserContextMaxTabs = 128;
inline constexpr qsizetype BrowserContextMaxBytes = 64 * 1024;
QString renderBrowserContext(const BrowserContextSnapshot &snapshot);
} // namespace ade
