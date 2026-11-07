#pragma once
#include <memory>

namespace ade {

// A system power request. acquire() may be retried after a failure.
class IPowerBackend {
public:
    virtual ~IPowerBackend() = default;
    virtual bool acquire() = 0;
    virtual void release() = 0;
};

// Tests and headless hosts: never changes power state.
class NullPowerBackend final : public IPowerBackend {
public:
    bool acquire() override { return false; }
    void release() override {}
};

// PowerCreateRequest/PowerSetRequest(SystemRequired, plus ExecutionRequired
// when available), falling back to SetThreadExecutionState. Never requests
// the display. Use from the GUI thread only: the fallback is per thread.
class WinPowerBackend final : public IPowerBackend {
public:
    WinPowerBackend() = default;
    WinPowerBackend(const WinPowerBackend&) = delete;
    WinPowerBackend& operator=(const WinPowerBackend&) = delete;
    ~WinPowerBackend() override;
    bool acquire() override;
    void release() override;

private:
    void* m_request = nullptr;
    bool m_execution = false;
    bool m_threadState = false;
};

// Holds the request exactly while keep-awake is enabled and at least one
// agent is actively working. Idempotent; the destructor releases.
class PowerRequestGuard {
public:
    explicit PowerRequestGuard(std::unique_ptr<IPowerBackend> backend);
    PowerRequestGuard(const PowerRequestGuard&) = delete;
    PowerRequestGuard& operator=(const PowerRequestGuard&) = delete;
    ~PowerRequestGuard();
    void update(bool enabled, int activeAgents);
    bool held() const { return m_held; }

private:
    std::unique_ptr<IPowerBackend> m_backend;
    bool m_held = false;
};

} // namespace ade
