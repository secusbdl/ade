#pragma once
#include <QString>
#include <memory>
#ifdef ADE_HARNESS_CONTEXT_TESTING
#include <functional>
#endif
namespace ade {
// Windows native owner-scoped fallback files. Authentication files are never
// stored here. Selected user workspaces are outside this class's authority.
class HarnessFileContext final {
public:
  HarnessFileContext(const QString &dataDirectory, const QString &agentId);
  ~HarnessFileContext();
  QString rootDirectory() const;
  QString error() const;
#ifdef ADE_HARNESS_CONTEXT_TESTING
  std::function<void(const QString &)> beforeStageCreateForTest;
#endif
  bool cloneTo(const QString &targetAgentId, QString *error = nullptr) const;
  bool remove(QString *error = nullptr);

private:
  struct State;
  std::unique_ptr<State> m_state;
};
} // namespace ade
