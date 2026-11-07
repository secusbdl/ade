#include "PowerRequest.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ade {
namespace {
// REASON_CONTEXT takes a mutable string; it must outlive the request.
wchar_t powerReason[] = L"ADE agents are working";
}

WinPowerBackend::~WinPowerBackend() { release(); }

bool WinPowerBackend::acquire() {
    if (m_request || m_threadState) return true;
    REASON_CONTEXT context{};
    context.Version = POWER_REQUEST_CONTEXT_VERSION;
    context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    context.Reason.SimpleReasonString = powerReason;
    const HANDLE request = PowerCreateRequest(&context);
    if (request && request != INVALID_HANDLE_VALUE) {
        if (PowerSetRequest(request, PowerRequestSystemRequired)) {
            // Best effort: the closest match to an app-suspension blocker.
            m_execution = PowerSetRequest(request, PowerRequestExecutionRequired) != FALSE;
            m_request = request;
            return true;
        }
        CloseHandle(request);
    }
    if (SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) != 0) {
        m_threadState = true;
        return true;
    }
    return false;
}

void WinPowerBackend::release() {
    if (m_request) {
        const HANDLE request = static_cast<HANDLE>(m_request);
        if (m_execution) PowerClearRequest(request, PowerRequestExecutionRequired);
        PowerClearRequest(request, PowerRequestSystemRequired);
        CloseHandle(request);
        m_request = nullptr;
        m_execution = false;
    }
    if (m_threadState) {
        SetThreadExecutionState(ES_CONTINUOUS);
        m_threadState = false;
    }
}

PowerRequestGuard::PowerRequestGuard(std::unique_ptr<IPowerBackend> backend) : m_backend(std::move(backend)) {}

PowerRequestGuard::~PowerRequestGuard() { update(false, 0); }

void PowerRequestGuard::update(bool enabled, int activeAgents) {
    if (!m_backend) return;
    const bool wanted = enabled && activeAgents > 0;
    if (wanted == m_held) return;
    if (wanted) {
        // A failed acquire leaves the guard released; the next update retries.
        m_held = m_backend->acquire();
        return;
    }
    m_backend->release();
    m_held = false;
}

} // namespace ade
