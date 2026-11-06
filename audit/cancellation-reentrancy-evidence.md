# Cancellation and observer-destruction regressions

2026-11-05, native Windows / MinGW / static Qt 6.11.2. Scope released by the integration owner: `src/core/ToolExecutor.cpp`, `src/network/NetworkBroker.cpp`, `tests/test_core.cpp`, `tests/test_network.cpp`. Other core, attachment and plugin production sources remained frozen.

## ToolExecutor

Reproduced the stale-completion defect for both `write` and `multiEdit`: a `fileChanged` observer canceled the current operation, changed the workspace, and started a new write using **the same request ID**. The old call then resumed and completed the replacement with its own saved-file result. Both data rows failed before the fix; evidence is `test-results/core-reentrancy-before-fix.txt` (2 failed, setup/cleanup passed).

The write path now computes its result before emitting, captures a QPointer, generation and request ID, and returns if the observer destroyed or superseded it. Its already-committed file content remains accurate; cancellation does not pretend to roll that commit back. Tests require the old cancellation result followed by the actual replacement write/result in the new workspace, with no write in the old workspace under the replacement filename. A separate observer destroys the executor on `fileChanged`; committed bytes remain, and no stale completion is emitted.

The emission audit also covered process output. `drainProcess()` emits `processOutput`, so the process-finished caller now rechecks lifetime, generation and process identity before reading members or finishing. A fixture disconnects the normal ready-read drain, forcing the final bytes to be drained from the finished callback; its observer cancels/replaces the operation, and the replacement completes normally. Error/started callbacks now bind the originating process/generation, and the synchronous `QProcess::start()` return path checks lifetime/identity before arming the timer. `finish()`, busy-rejection and direct output emissions have no further member access after their final signal; cancellation invalidates queued operations before emitting completion.

## NetworkBroker

`cancelAgent()` and credential `invalidateType()` iterated snapshots of pending requests, but `finish()` emitted an external completion after each one. An observer could destroy the broker on the first completion; the next iteration then accessed its dead pending map. `registerCredentialType()` also wrote the registry after `removeCredential()` could emit and destroy it.

The regression holds **two real loopback HTTP requests** using an invented credential. On the first cancellation completion, it destroys the broker. On Windows its isolated VirtualAlloc storage is then changed to PAGE_NOACCESS, so a stale member access fails deterministically instead of depending on heap reuse. Before the fix, the `cancel` row terminated with **0xc0000005 / exit -1073741819**. `test-results/network-delete-before-fix.txt` contains the test start before the exception; the command output captured the exception. This was not an AddressSanitizer run. The test allocates/protects only its own process memory, not any application or user file.

The loops now stop immediately when a QPointer reports destruction. Registration checks lifetime after removal before changing its registry; credential rotation returns failure if the broker was destroyed during its cancellation callbacks. Reset already invalidates handles before cancellation and performs no member access after the cancel call. Six data rows cover cancel, reset, credential removal, rotation, re-registration and allowlist change; all require exactly one observer completion, destroyed lifetime, and no later callback.

## Verification and limits

- `test-results/core-tests.txt` / `.xml`: **40 passed, 0 failed, 0 skipped**. Independent target in `private/attachments-probe-build`.
- `test-results/network-tests.txt` / `.xml`: **19 passed, 0 failed, 0 skipped**. Independent target in `private/network-probe-build`.
- No actual provider/plugin credential, user workspace content, external service request or shared build mutation was used.
- This fixes the reproduced observer/reentrancy boundaries. The separate host-level atomic workspace swap/reset behavior belongs to AgentToolHost integration; these tests do not assert that every caller handles workspace validation or every QObject in the application is reentrancy-safe.


## SandboxHost startup and cancellation-loop correction

A later integrated checkpoint exposed another lifetime failure in `native_sandbox`: 24 of 25 CTest suites passed, but `reentrantDestructionFromSignals` crashed. `test-results/integrated-attachments-apps-git-final.xml` records exception `0xc0000005` at `0x00007ff62a6db5b8`. A full-suite GDB reproduction resolved that exact address to `QProcess::state()`, called by `SandboxHost::send()` from `SandboxHost::execute()`.

The missing worker executable made `QProcess::start()` emit startup failure synchronously. The completion observer destroyed the host inside `ensureProcess()`, then the old `execute()` resumed and read its dead QProcess. The focused test happened to pass when freed heap storage remained readable, so a new regression explicitly destroys a SandboxHost in its own VirtualAlloc storage and changes that storage to PAGE_NOACCESS. It crashed before the fix in 6 ms. Evidence: `test-results/sandbox-protected-startup-before.txt`, `test-results/sandbox-startup-gdb.txt`, and `test-results/sandbox-protected-startup-gdb-test.txt`. These are ordinary native/GDB runs, not AddressSanitizer results.

The startup return path now checks QPointer lifetime **and** that the same pending wire ID and live context generation still own the call. The identity check fixes a separately reproduced case where a startup-failure observer kept the host alive and replaced the call using the same external request ID. Previously, the obsolete frame was sent after the replacement and destroyed its persistent state. `test-results/sandbox-startup-replacement-before.txt` records the expected retained value `42` becoming empty. The new test requires the replacement result, retained global state, and absence of the obsolete script's console marker.

`cancelAll()` also iterated a snapshot of external IDs without checking whether the first synchronous reset callback destroyed the host. A regression starts two real pending worker contexts, destroys/protects the host on the first `contextReset`, then calls `cancelAll()`. Before the fix it crashed with `0xc0000005`; GDB shows `SandboxHost::cancel()` called by `cancelAll()`. Evidence: `test-results/sandbox-cancel-all-before.txt`, `test-results/sandbox-cancel-all-gdb.txt`, and `test-results/sandbox-cancel-all-gdb-test.txt`. The loop now returns after deletion. `resetAgent()` likewise snapshots its intended context generation and checks lifetime after sending before invalidating it.

Scope was explicitly released for `src/sandbox/SandboxHost.cpp`, `src/sandbox/SandboxHost.h` and `tests/test_sandbox.cpp`; only the implementation and tests required changes. No worker runtime redesign or next-phase app-authoring work was included.

Final verification of this correction:

- Focused original deletion case plus three deterministic startup/cancellation regressions: **6 passed, 0 failed**, `test-results/sandbox-lifecycle-focused-after.txt` (including init/cleanup).
- Independent complete sandbox suite: **25 passed, 0 failed, 0 skipped**, 4.440 seconds, `test-results/sandbox-lifecycle-full-after.txt` / `.xml`.
- Rebuilt integrated `test_sandbox`: **25 passed, 0 failed, 0 skipped**, 4.362 seconds, `test-results/sandbox-lifecycle-integrated-after.txt` / `.xml`.
- Rebuilt integrated `test_runtime_apps`: **11 passed, 0 failed, 0 skipped**, 3.596 seconds, `test-results/runtime-apps-lifecycle-integrated-after.txt` / `.xml`. This includes actual mini-app close deleting its host during reset, synchronous browser cancellation deleting its host, queued-file cancellation, atomic workspace replacement, canonical no-op and invalid-candidate preservation.
- The app suite reports temporary-directory cleanup warnings for WebView2 profile files still held during immediate teardown. Functional assertions passed; the logs preserve those warnings. They are not represented as complete filesystem cleanup coverage.

The two named integrated targets were rebuilt under the integration owner's release. These fixes establish the tested synchronous startup/reset/deletion boundaries, not universal QObject lifetime safety.

## PluginCatalog refresh and rejection correction

A subsequent read-only review traced the same callback boundary through `PluginCatalog::configureAgent`. Its rejection path called `setAllowedCredentialTypes`, then continued removing credentials through an unguarded broker reference. Its reload path called `registerCredentialType`, then called rejection cleanup after registration reported broker destruction. Separately, callbacks could destroy the window containing the catalog itself; the returning function still accessed its `m_revisions` member. NetworkBroker's own internal guards did not protect these callers.

The integration owner released only `src/plugins/PluginCatalog.cpp`, `src/plugins/PluginCatalog.h` and `tests/test_plugins.cpp` for this correction. Before changing production code, two separate regression processes reproduced access violations:

- `cancellationMayDeleteBrokerOrCatalogOwner:malformed-broker`: a real held localhost HTTP request is canceled by rejection of malformed plugin preferences; its first completion destroys the broker. The process terminated with **0xc0000005 / exit -1073741819**, after 70 ms in the test function. `test-results/plugins-delete-before-fix.txt` contains the test start; the command output contains the exception and native stack addresses.
- `cancellationMayDeleteBrokerOrCatalogOwner:reload-catalog`: a real held localhost request uses a fixture-only opaque credential. Reload restores the host registry, causing cancellation; its first completion destroys the catalog owner while leaving the broker alive. The process terminated with **0xc0000005 / exit -1073741819**, after 77 ms. `test-results/plugins-owner-before-fix.txt` contains the test start; the command output contains the exception and native stack addresses.

Both fixtures use their own VirtualAlloc storage and apply PAGE_NOACCESS after destruction, so success cannot depend on freed heap bytes remaining readable. This is ordinary Windows native execution, not AddressSanitizer. The reload fixture temporarily registers the invented token's origin as its own localhost server; no request is sent to GitHub or another real service. All encrypted-store values are explicit fake fixture tokens.

The catalog now copies the host agent ID and retains a local shared pointer to its non-secret revision-cache state before any broker mutation. It does not access catalog members or the caller-owned agent-ID reference after callback boundaries. A local QPointer guards the broker after every emitting mutation, including allowlist reset, each rejection-loop removal, registration and credential assignment. If registration returned false because the broker died, the catalog stops instead of invoking cleanup on the destroyed object. Cache state survives an owning-window deletion only for the duration of an active call; cached broker entries remain QPointers and cannot authorize a replacement object at the same raw address.

Final independent `private/plugins-probe-build/test_plugins.exe` verification: **20 passed, 0 failed, 0 skipped**, **0.949 seconds**, recorded in `test-results/plugin-tests.txt` and `test-results/plugin-tests.xml`. Nine data rows cover malformed preferences, encrypted credential reload and allowlist change, each destroying the broker, catalog owner, or both. They require exactly one completion and no late callback. Catalog-owner-only cases also verify that the remaining broker reaches the expected configured or rejected state. Existing source-owned routing, disabled-plugin isolation, encrypted storage, extraction provenance and broker-cache replacement tests remain passing. `git diff --check` passed for the three released files.

No shared build or other production module was changed during this correction. Root-owned app-tab registration and UI plugin-refresh callers were separately identified for lifetime guards; their fixes and integrated UI verification are not established by this plugin-only result. These regressions cover the specified synchronous deletion paths, not every nested reconfiguration, thread-safety scenario or QObject ownership boundary. Production plugin code was refrozen after this result for the integrated checkpoint.
