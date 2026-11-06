#pragma once
#include "GitTypes.h"
#include "WorktreeUsage.h"
namespace ade {
// Read-only worker helper. The snapshot must come from an authorized, current
// GitService inspection. This function does not grant workspace authority.
// It verifies the linked .git/commondir/gitdir relationship and derives a
// physical token; unsupported platforms return an unverified identity.
WorktreeUsageIdentity probeWorktreeUsageIdentity(const GitSnapshot &snapshot,
    const QString &administrativeDirectory,QString *error=nullptr);
}
