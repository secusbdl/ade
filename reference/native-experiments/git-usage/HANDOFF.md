# Worktree usage staging handoff

Private implementation only. Production GitService, MainWindow, root CMake, cleanup controls and `usageKnown=false` are unchanged. This slice supplies durable usage evidence and a conservative eligibility policy; it does not remove worktrees or authorize filesystem paths.

## Files and build boundary

Migrate `src/git/WorktreeUsage.{h,cpp}`, `WorktreeUsageJournal.{h,cpp}` and `WorktreeUsageIdentity.{h,cpp}` into the existing QtCore `ade_git` target only after the host lifecycle is ready. Add `tests/test_git_usage.cpp` as a QtTest executable linked to `ade_git`. Do not migrate the copied `GitTypes.h`, private CMake, build directory or rename probe. No new third-party dependency is introduced.

The pure policy and mount registry are synchronous, thread-confined value types. The journal is synchronous and not thread-safe; serialize its bounded disk operations on one owner thread. The identity probe performs filesystem reads and belongs on a worker. Its result does not carry authorization: reject it if the host workspace/context generation changed before publication.

## Narrow host contract

1. Construct one `WorktreeUsageJournal(dataDirectory)` for the application profile. Call `open(now, sessionHistoryRevision)` before recording use. The revision is SHA-256 of the successfully loaded durable session history, not a speculative in-memory draft. Empty or changed history revisions, an absent journal or an interrupted previous run start a seven-day recovery floor. Any open/read/write failure disables cleanup eligibility while preserving existing data.
2. Keep an explicit live owner set separate from stored chat history. Activate an owner only after a successful workspace activation; normal conversation switches need not release an already live owner. Archive/delete/unload release the owner after the durable session transition and resource shutdown succeed. Archived history alone does not mount a workspace. Forks receive a new owner and ticket only when the new conversation is persisted and activated.
3. Reserve a mount transition with `WorktreeMountRegistry::beginUpdate(owner, canonicalPaths)`. It protects new paths immediately and keeps prior mounts protected while identity inspection is pending. Complete it with the exact verified identity set and the same ticket. Failed or stale completions cannot remount old paths. Release invalidates all pending completions for that owner. `replace` is available when identities are already verified synchronously.
4. Obtain identities only from a freshly authorized Git snapshot and its exact administrative directory. `probeWorktreeUsageIdentity` validates `.git`, `commondir` and `gitdir` relationships and combines the Windows physical identities of the common Git directory, administrative directory and worktree root. Persist `recordLiveUse` on each successful mount/use event; use the maximum actual timestamp for out-of-order events. `recordNewInstance` is restricted to a worktree whose creation the current application actually completed. Restored/imported history uses `observeImported`; absence of old history never means never used.
5. Preserve journal records when a chat reconnects, archives or is deleted. Several chats may reference the same instance. A successful stale activity event may still raise historical usage, but its stale mount ticket must never restore a live owner. Pending or failed identity lookups keep cleanup scope incomplete rather than silently dropping protection.
6. On orderly shutdown, first stop new lifecycle/activity events and finish successful session and journal writes, then call `closeClean(finalSessionHistoryRevision)`. Destruction does not mark a clean shutdown. A close failure stays visible and retains recovery protection on the next launch. A missing/error state must not be replaced automatically with an empty store.

The registry supports several mounts per owner, but the present Conversation model stores one workspace. This module does not implement host multi-workspace parity. Do not set `trackingScopeComplete=true` merely because all current stored chat rows were read; replaced historical workspace associations are not recoverable from them.

## Future cleanup boundary

Leave cleanup unexposed during initial journal integration. Later scans must obtain `validatedHealth(explicitCompleteScope)` once per complete scan and again before each proposed removal. Cached `health()` deliberately reports incomplete tracking and cannot authorize cleanup. Re-read the latest record, physical identity, managed-root authorization, registration, attached branch, clean status, merge relationship, live mounted owners and editor/agent/terminal/setup busy guards before mutation. Unknown or failed checks block eligibility. Preserve common-Git-directory serialization and existing removal guards; a policy result is not a durable deletion permit.

Eligible instances must have verified activity/creation evidence, be unused for at least seven days, be beyond the recovery floor, be unmounted and idle, be managed registered linked worktrees with a clean known status and an attached branch merged into the default/main/master/develop/dev target. Main worktrees are excluded. Future timestamps block. Dismissals last seven days; prune them only after a complete successful scan. No branch deletion is part of this slice.

The identity probe releases its handles when it returns. Its multiple metadata reads are not an atomic Git snapshot. Journal locking coordinates cooperating ADE instances; expected-revision checks before commit and cleanup detect the tested external edits but are not an OS-level compare-and-swap against a hostile simultaneous writer. The storage directory pin and canonical resolution were tested against junction retargeting; universal adversarial ancestor-retarget, power-loss durability, ReFS/network-share behavior and non-Windows identities have not been established. Unsupported physical identity calls return unverified.

## Evidence

`audit/tests-final.txt`: 30 passed, zero failed/skipped on Windows 11, Qt 6.11.2 static, MinGW GCC 16.2.0. Tests exercise restart/recovery/history revisions, bounded corrupt storage, writer locking and external edits, maximum activity, path reuse, unknown imports, exact age/snooze boundaries, failed scan pruning, live guards, several owners/mounts, stale asynchronous completions and actual temporary Git repositories.

`audit/storage-alias-before.txt` records a failing junction-retarget test before physical canonical storage pinning. `audit/root-identity-before.txt` records a failing folder-recreation test before adding the root physical identity. Both now pass. `cleanupHealthMustValidateTheCurrentJournalRevision` proves a formerly eligible cached record is rejected after an external journal edit.

All Git fixture mutations are confined to dedicated temporary repositories. No production cleanup is enabled and no user repository/worktree was removed. The separate Windows rename probe is diagnostic evidence for the file-mutation review; it is not a dependency of this module.

## Source semantics

Pinned Stagewise `17a0123a0e896885465df36169457199d5225eb5` reads root CHAT history (including archived rows), matching `mountedWorkspaces[]` to obtain maximum `lastMessageAt`; it uses a separate active mount registry for exclusions. See `packages/agent-core/src/services/agent-persistence/db.ts:629`, `apps/browser/src/backend/services/toolbox/services/mount-manager/index.ts:986` and cleanup candidate checks around `:1345`. Dismissal persistence is in `apps/browser/src/backend/services/preferences.ts:1039`.

The native unknown-history block, instance identity and recovery floor are conservative additions because the current native single-workspace session model cannot prove complete imported history. They intentionally do not claim exact source cleanup eligibility for undocumented older usage.
