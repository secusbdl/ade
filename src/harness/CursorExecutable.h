#pragma once
#include <QString>
#include <QStringList>
namespace ade {
struct CursorExecutable {
  QString program, entrypoint, identity;
  QStringList arguments;
  bool valid() const { return !program.isEmpty() && !identity.isEmpty(); }
  // A native executable or the separately installed vendor launcher/bundle.
  // Never executes shell source, downloads a runtime, or reads auth files.
  static CursorExecutable resolve(const QString &requested,
                                  QString *error = nullptr);
};
} // namespace ade
