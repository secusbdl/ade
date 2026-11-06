#pragma once
#include <QString>
#include <memory>
namespace ade {
struct HarnessWorkspace {
  QString path, identity;
  std::shared_ptr<void> pin;
  static HarnessWorkspace resolve(const QString& path);
};
}
