#pragma once
#include <QJsonObject>
#include <QList>
#include <QString>
namespace ade {
// Lexical display mounts only: never a filesystem or tool authorization.
struct AcpDisplayMount { QString prefix, root; };
class AcpToolPresentation final {
public:
  static constexpr int MaximumEntries = 32;
  static constexpr int MaximumText = 65536;
  static constexpr int MaximumTotalText = 262144;
  static QJsonObject map(const QJsonObject &tool,
                         const QList<AcpDisplayMount> &mounts);
  static bool valid(const QJsonObject &value);
  static QString mountedPath(const QString &path,
                              const QList<AcpDisplayMount> &mounts);
  static QString sourceName(const QJsonObject &tool);
  // Source-listed UI wrappers only. These predicates never grant permission,
  // discard raw tool events or change execution/routing authority.
  static bool isQuestionWrapper(const QJsonObject &tool);
  static bool suppressQuestionWrapperPart(const QJsonObject &part);
};
}
