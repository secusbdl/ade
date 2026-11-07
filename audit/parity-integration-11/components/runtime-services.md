# Native consumers for notification sounds, keep-awake and single-instance (corrected)

## 0. Constraints
- Apply only after the full206 gate finishes and its checkpoint is captured. The following are all in the uncommitted frozen206 diff: CMakeLists.txt, src/core/AgentEngine.h, src/ui/MainWindow.{h,cpp} and src/ui/SettingsPage.cpp. src/main.cpp is not modified. Keep edits additive and run `git apply --check` for each slice.
- No Qt Multimedia. C:/Qt/6.11.2-mingw64-static/lib contains no Multimedia or FFmpeg libraries. Use winmm (PlaySound and MCI).
- QtNetwork is linked through ade_core (CMakeLists `target_link_libraries(ade_core PUBLIC ... Qt6::Network ...)`), and qtnetwork-config.h has `QT_FEATURE_localserver 1`.
- New src/ui/*.cpp files are auto-globbed into ade_ui. SettingsPage.cpp is also compiled standalone by render_settings, test_settings and test_settings_audit, so SettingsPage must never include the new sound, power or instance headers.
- MinGW's default `_WIN32_WINNT` is 0x0601 (_mingw.h:256), which is enough for PowerCreateRequest, PowerSetRequest and PowerClearRequest (winbase.h:1297-1301).

## 1. Upstream contract (pinned 17a0123a, all lines opened)
### 1.1 Sounds: apps/browser/src/backend/services/notification-sounds.ts
- Events are done, question and error (line 39). Loudness is off, subtle or default, with gains 0, 0.251 and 1 (40-49). Defaults are subtle and bubble-pops (shared-types.ts:800-803). UI labels are Off / Subtle / Loud (use-sound-settings.ts:6-10).
- Trigger policy (653-700):
  1. If the window is focused, reset lastSoundPlayedAt to 0.
  2. Debounce Done only, at 10 s (164, 666-671).
  3. Skip when lastOpenAgentId equals the agent and the window is focused (676-681).
  4. Play if loudness is not off.
  5. Arm the debounce only when the sound was delivered (697-699).
  - A pack missing that event delivers nothing (playFromBuffer 761-769).
- Preview plays the pack's done sound at the requested loudness and returns false when loudness is off (267-284). The UI previews on a loudness change (use-sound-settings.ts:75) and on a pack change (110). The Play button is disabled when loudness is off (general-settings-section.tsx:187-189).
- Import (334-557):
  - Accepts a single .mp3 (name = basename without its extension) or a JSON manifest with a non-empty name, an optional description, and sounds {done?, question?, error?: string|null}.
  - At least one sound must be named. Each path is resolved relative to the manifest, and traversal is rejected.
  - Each file must have a .mp3 extension, exist, and start with "ID3" or bytes where `b0==0xFF && (b1&0xE0)==0xE0`.
  - If no file copies, the pack directory is removed. pack.json is rewritten with null for missing events.
  - The JSON syntax error message is "The file is not valid JSON."
  - Packs live in userData/imported-sound-packs/<uuid> (main.ts:367-370). A successful import is selected (main.ts:1110-1121), and the UI says "<name> is now selected for notifications." (general-settings-section.tsx:104-110).
- Event sources (packages/agent-core/src/agents/base-agent.ts):
  - done: a genuine completion where the history contains any assistant message and the step has no approval request (2236-2245), or an external step that ends with an empty queue (2410-2414).
  - question: once per approval id (3270-3289). There is no upstream notification for ask-user.
  - error: at 1830, 1872, 1960, 2092 (excluding plan-limit, model-restricted and subscription errors, 2084-2092), 2314 and 2401.
- There is no OS toast for agent events. services/notification.ts shows only in-app toasts.

### 1.2 Keep awake: services/agent-power-save-blocker.ts
- Enabled by blockAppSuspensionWhenAgentsActive, default true (53).
- An agent is active when isWorking, with no pendingUserQuestion and no `approval-requested` tool part in the latest assistant message (16-49).
- Start 'prevent-app-suspension' when enabled and at least one agent is active, and stop otherwise. This is idempotent, re-derived on every state change, and released on teardown (89-157).
- Never request display-on.

### 1.3 Single instance
- index.ts:100-104: requestSingleInstanceLock; quit if not acquired. Electron keys the lock by userData.
- window-layout/index.ts:662-668: restore the window if minimized, then focus it.
- main.ts:1539-1552 and 1422-1434: on second-instance, forward only URLs. Skip args beginning with '-'; allow http, https and stagewise* (1404-1416). The initial launch handles argv URLs at main.ts:1316 and 1571-1597.
- No workspace or prompt forwarding exists upstream.

## 2. Native state (verified)
- SettingsPage.cpp:
  - defaultPreferences (421-423) sets `agent:{keepAwake:true, notificationLoudness:"normal", notificationSound:"default"}`.
  - buildGeneral (723-740) has `agentKeepAwake`, a `notificationLoudness` combo (Off/Normal/Loud, data off/normal/loud), and placeholder text at 739.
  - validPreferences (316-418) checks only agent.* types, through matchingKnownTypes (306-314).
  - When validation fails on load, the store is marked invalid and every save is blocked (reload 597-610).
  - savePreferences emits preferencesChanged (644). MainWindow.cpp:268 connects that signal to applyPreferences.
- AgentEngine.h:
  - Accessors: isBusy (94), lastTurnSucceeded (95), queuedMessages (53), pendingQuestion (65, implemented in AgentEngineControl.cpp:217) and messages (91).
  - Signals at 111-133.
  - Private state: m_waitingApproval (236), m_harnessApprovals (185) and m_questionToken (223).
- AgentEngine.cpp:
  - startUserMessage:221 emits failed while busy and emits no finished.
  - complete() (1084-1096): busyChanged(false), then finished, then scheduleQueue.
  - fail() (1098-1121): m_busy=false, busyChanged(false), failed, then finished.
  - cancel() (1123-1145): busyChanged(false), then finished, with lastTurnSucceeded=false.
  - The built-in approval path sets m_waitingApproval and emits displayChanged before approvalRequired (892-898). approveTool is at 937-967.
- Harness question tokens: Claude 85/133, ACP 173/195, Cursor 177, Codex harness 346/367, built-in Control 234/280/289. Harness approvals: Harness 285, Claude 53, ACP 140, Cursor 127.
- MainWindow.cpp:
  - The constructor calls buildUi(), initializeSessionRecovery() and applyPreferences() at line 86.
  - engineFor (378-418) is the only production AgentEngine creation site. ProviderService.cpp:109 creates a test engine, which is not watched.
  - disposeConversation is in MainWindowConversations.cpp:133-139.
  - The openUrlRequested lambda at 272 is the existing URL-opening pattern.
  - ~MainWindow is at 791. closeEvent (909-960) uses m_sessionTransition and m_sessionRecoveryDialog.
- main.cpp: `--question-mcp-stdio` runs before QApplication (84). The parser has no positional arguments (88-105). The smoke branch is 108-191. MainWindow is constructed at 192. ADE is a WIN32-subsystem executable.
- Test fixtures: FixtureServer in tests/test_core.cpp:32 (with built-in approval at 359-380), and test_main_chrome.cpp:116-134, which runs real turns in a MainWindow.

## 3. Design

### 3.1 Assets and licensing
Do not copy upstream MP3s or pack names; they have no stated provenance, and NOTICE forbids restricted upstream assets. Built-in packs are synthesized in C++ as 16-bit PCM mono WAV at 44.1 kHz, 600 ms or shorter:
- `ade-chime` "ADE Chime" (default)
- `ade-soft` "Soft Tones"
- `ade-pulse` "Digital Pulse"

In each pack, done is a rising two-tone, question is two equal pings, and error is a falling tone. There are no binary assets.

### 3.2 Preferences (no migration)
- Keep the stored loudness values off, normal and loud. normal maps to subtle (0.251) and loud to default (1.0). Relabel the combo Off / Subtle / Loud.
- `agent.notificationSound`: "default" means `ade-chime`. Any other value is a pack id, resolved only through the enumerated listing; an unknown id falls back to `ade-chime`.
- Add to validPreferences (value checks; the type checks already exist):
  - loudness must be one of {off, normal, loud};
  - sound must be "default" or match `^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$`, which rejects "." and "..".
- These are the only values ever written, so existing files still validate.
- Import limits (ADE-specific):
  - each audio file is 5 MiB or less; the manifest is 64 KiB or less;
  - at most 256 imported packs, and the cap error names the folder;
  - reject sources where isSymLink, isJunction or isShortcut is true;
  - reject manifest names that contain ':', are absolute or UNC, or whose canonical path is not under the canonical manifest directory (case-insensitive).
- WAV extension: PCM 16-bit mono or stereo only, validated by a bounded RIFF chunk walk.

### 3.3 Components (new files in src/ui/)

**A. AgentActivityMonitor (AgentActivityMonitor.{h,cpp})**
- `struct AgentSnapshot { bool busy, awaitingUser, succeeded, queueEmpty, hasAssistant, hasQuestion; };`
- `void watch(const QString& id, AgentEngine* engine, std::function<AgentSnapshot()> snapshot = {});` The default reads the engine: isBusy, isAwaitingUser, lastTurnSucceeded, queuedMessages().isEmpty(), any messages() row with role=="assistant", and !pendingQuestion().isEmpty().
- `void unwatch(const QString& id); void refresh(const QString& id); int activeCount() const;`
- `bool notifyQuestionTool = true;` (an ADE deviation; set it to false for strict parity).
- Signals: `activeCountChanged(int)`, `attention(QString id, int event)`, where `enum class AttentionEvent { Done, Question, Error }` is passed as int for moc simplicity.
- Rules:
  - Error: on failed, only if `!snapshot.busy`. This filters AgentEngine.cpp:221.
  - Done: on finished, if succeeded && queueEmpty && !awaitingUser && hasAssistant. A cancel produces nothing.
  - Question: on approvalRequired, once per (agent, approval id). The id set is cleared when busy becomes false.
  - ADE deviation: also emit Question when hasQuestion goes from false to true, using a per-agent bool, so token regeneration between steps does not re-fire.
  - Active = busy && !awaitingUser.
  - Recompute through one coalescing zero-ms single-shot timer on busyChanged, approvalRequired, displayChanged, questionChanged, toolStarted, toolFinished, finished, destroyed and refresh(). A destroyed engine counts as inactive. activeCountChanged is emitted only when the count changes.
- Out of scope: plan-limit, subscription and model-restricted error suppression (no native classification).

**B. AgentEngine.h**
Add an inline accessor: `bool isAwaitingUser() const { return !m_waitingApproval.isEmpty() || !m_harnessApprovals.isEmpty() || !m_questionToken.isEmpty(); }`. It changes no behaviour. All harnesses set m_questionToken (see section 2).

**C. NotificationSoundService (NotificationSounds.{h,cpp})**
- Constructor: `NotificationSoundService(QString importedPacksDir, ISoundOutput*, std::function<qint64()> clock = QDateTime::currentMSecsSinceEpoch, QObject* parent = nullptr)`. The directory is `<dataDir>/imported-sound-packs`. On construction, remove stale `.staging-*` directories.
- `configure(const QJsonObject& agentPrefs)`; `QList<PackInfo{id,name,builtin}> packs() const` returns built-ins first, then imported packs sorted by id. Only UUID-named directories with at least one event file are listed. The name comes from pack.json `name`, otherwise the id is capitalised.
- `QString resolvePack(const QString&) const`
- `bool notify(AttentionEvent, const QString& agentId, bool windowFocused, bool agentVisible)`:
  - `std::optional<qint64> m_lastPlayedAt`; focus resets it;
  - Done is debounced only when a value exists and `now - value < 10000`;
  - suppress when visible and focused;
  - off delivers nothing;
  - a missing event file delivers nothing;
  - arm the debounce only when the output returns true.
- `bool preview(packId, loudness)` plays done, returns false when loudness is off or the pack has no done sound, and never arms the debounce.
- `ImportResult importFile(path)` returns `{id, name, error}`, following the upstream semantics plus the 3.2 limits:
  - it stages into `importedPacksDir/.staging-XXXXXX` (a QTemporaryDir template in the same directory), writes pack.json, then renames the directory to a UUID (`QUuid::WithoutBraces`) and calls setAutoRemove(false);
  - any failure leaves nothing behind;
  - files are saved as `<event>.mp3` or `<event>.wav` by validated type.
- Static helpers:
  - `synthesizeWav(pack, event)`
  - `scalePcm16Wav(wav, gain)`: gain between 0 and 4, clamps to [-32768, 32767], and returns empty for malformed input
  - `looksLikeMp3(head)`, using the exact upstream mask
  - `looksLikePcm16Wav(bytes)`
  - `gainFor(loudness)`: off=0, normal=0.251, loud=1.0, and unknown maps to normal
- Playback:
  - built-in sounds and imported WAVs: read the file, call scalePcm16Wav, then playWav;
  - imported MP3s: playFile(path, gain).

**D. ISoundOutput, WinSoundOutput and NullSoundOutput (SoundOutput.{h,cpp})**
- Interface: `virtual bool playWav(QByteArray) = 0; virtual bool playFile(const QString&, double gain) = 0; virtual void stop() = 0;`
- `WinSoundOutput::playWav`:
  1. `PlaySoundW(nullptr, nullptr, 0)`
  2. close the MCI alias
  3. **then** `m_buffer = wav`
  4. `PlaySoundW(reinterpret_cast<LPCWSTR>(m_buffer.constData()), nullptr, SND_MEMORY|SND_ASYNC|SND_NODEFAULT)`
- `playFile`:
  1. stop PlaySound
  2. `close ade_notify`
  3. `open "<native path>" type mpegvideo alias ade_notify`
  4. `setaudio ade_notify volume to <round(gain*1000)>`
  5. `play ade_notify`
  - Return false on any MCI error; Windows N editions may lack codecs. Paths are always under the data dir and are quoted.
- stop() and the destructor stop both mechanisms before the buffer is released. GUI thread only. Link winmm.
- NullSoundOutput returns false and records nothing.

**E. PowerRequestGuard, WinPowerBackend and NullPowerBackend (PowerRequest.{h,cpp})**
- Interface: `IPowerBackend { virtual bool acquire() = 0; virtual void release() = 0; }`.
- `WinPowerBackend::acquire`:
  - Fill REASON_CONTEXT with POWER_REQUEST_CONTEXT_VERSION and SIMPLE_STRING, pointing at a static `wchar_t[] = L"ADE agents are working"`.
  - Call `h = PowerCreateRequest(&rc)`. If h is INVALID_HANDLE_VALUE or null, fall back.
  - Call `PowerSetRequest(h, PowerRequestSystemRequired)`. If that fails, CloseHandle and fall back.
  - Best effort: `PowerSetRequest(h, PowerRequestExecutionRequired)`. This matches Chromium's believed mapping, which is unverified locally.
  - Fallback: `SetThreadExecutionState(ES_CONTINUOUS|ES_SYSTEM_REQUIRED)` on the GUI thread.
- release clears whatever was set, then calls CloseHandle, or `SetThreadExecutionState(ES_CONTINUOUS)` for the fallback.
- Never use display requests.
- `PowerRequestGuard(std::unique_ptr<IPowerBackend>)`:
  - `update(bool enabled, int active)`: acquire when enabled && active > 0 and not held; release otherwise. Idempotent. A failed acquire leaves held() false and is retried on the next update.
  - `held()`; the destructor releases.

**F. InstanceCoordinator (InstanceCoordinator.{h,cpp})**
- `struct InstanceRequest { QList<QUrl> urls; QString cwd; };` declared with Q_DECLARE_METATYPE.
- `InstanceCoordinator(QString dataDir, QObject* parent = nullptr)`; `enum class Role { Primary, Forwarded, Failed, Refused }`.
- `Role acquire(const InstanceRequest&, int timeoutMs = 5000, QString* error = nullptr)`:
  1. Use `QLockFile(dataDir + "/ade-instance.lock")` with `setStaleLockTime(0)`, the same pattern as WorktreeUsageJournal.cpp:109, so only a dead or foreign PID makes the lock stale.
  2. If `tryLock(0)` succeeds:
     - generate `server = "ade-" + 32 random hex characters` and `listen` with UserAccessOption;
     - write `<dataDir>/ade-instance.json` {v:1, pid, server} with QSaveFile;
     - return Primary. If listen fails, log it, skip the sidecar, and still return Primary without IPC.
  3. Otherwise, loop until the deadline, every 100 ms:
     - read the sidecar, call `AllowSetForegroundWindow(pid)` (exact pid, not ASFW_ANY), and connect;
     - send the frame and wait up to 2 s for the reply;
     - `{ok:true}` returns Forwarded;
     - `{ok:false, reason:"closing"}` means retry tryLock, and the client may become Primary;
     - a missing sidecar means retry.
     - Timeout returns Failed with an error.
- Frame: a 4-byte big-endian length followed by compact JSON, 64 KiB or less: `{"v":1,"type":"activate","cwd":abs,"urls":[...]}`.
  - URLs must be http or https, parse in QUrl StrictMode, be 2048 bytes or less, number 16 or fewer, and fit within the frame. The client drops any that overflow.
  - The server rejects frames that are oversized, malformed, of the wrong version or type, or not complete within 2 s (injectable). It closes the connection without emitting, and keeps serving.
  - The server allows at most 4 pending connections.
- Requests that arrive before `markReady()` are buffered (8 at most) and delivered after it is called.
- `setClosing()` makes later requests receive `{ok:false, reason:"closing"}`.
- The destructor closes the server, removes the sidecar if it still holds this pid, and unlocks.
- Signal: `requestReceived(ade::InstanceRequest)`.

### 3.4 Integration edits

**src/main.cpp**
- `parser.addPositionalArgument("urls", "http(s) URLs to open", "[urls...]")`. Filter like extractUrlsFromArgs: skip anything starting with '-', keep http and https. Custom schemes are out of scope.
- The `--question-mcp-stdio` (84) and `--smoke` (108) paths bypass the coordinator.
- After the smoke branch and before line 192, construct `InstanceCoordinator coordinator(directory)`:
  - If another instance holds the lock (try acquire in "probe" mode first) and `--import-keys`, `--prompt` or `--workspace` is set, return Refused and exit 3 with the message "ADE is already running for this data directory; close it before using --import-keys/--prompt/--workspace." Show it on stderr and in QMessageBox::warning.
  - Otherwise, Forwarded exits 0. Failed exits 4 and shows the error on stderr and in a QMessageBox.
  - Primary:
    1. construct MainWindow
    2. connect requestReceived to `&MainWindow::handleInstanceRequest`
    3. connect `qApp->aboutToQuit` to `coordinator.setClosing()`
    4. run the existing import-keys and workspace handling
    5. `window.show()`
    6. open startup URLs through handleInstanceRequest
    7. `coordinator.markReady()`
- Never start a second primary while the lock is held.

**src/ui/MainWindow.{h,cpp}**
- `struct SystemEffects { std::function<std::unique_ptr<ISoundOutput>()> sound; std::function<std::unique_ptr<IPowerBackend>()> power; }; static void setSystemEffectsFactory(SystemEffects);`
  - The default returns Null implementations when `QGuiApplication::platformName()` is "offscreen" or "minimal", or when `ADE_DISABLE_SYSTEM_EFFECTS=1`, and Win implementations otherwise. This keeps the CTest MainWindow hosts silent and free of power requests.
- At the start of the constructor body, before `buildUi()` at line 86, create:
  - m_soundOutput;
  - `m_sounds = new NotificationSoundService(m_store.directory() + "/imported-sound-packs", m_soundOutput.get(), ..., this)`;
  - `m_power = std::make_unique<PowerRequestGuard>(...)`;
  - `m_activity = new AgentActivityMonitor(this)`.
- In engineFor, after the engine is created: `m_activity->watch(id, engine)`. In disposeConversation, call `m_activity->unwatch(id)` before deleting the engine. After every approveTool call that MainWindow makes, call `m_activity->refresh(id)`.
- Connections:
  - attention → `m_sounds->notify(event, id, isActiveWindow() && !isMinimized(), id == m_currentId || (m_sideChats.value(id) && m_sideChats.value(id)->isVisible()))`
  - activeCountChanged → `m_power->update(m_settings->preference("agent.keepAwake").toBool(true), n)`
- applyPreferences, null-guarded: `m_sounds->configure(m_settings->preference("agent").toObject()); m_power->update(keepAwake, m_activity->activeCount());`
- After m_settings is created, call `m_settings->setSoundHooks(...)` with:
  - packs, from m_sounds->packs();
  - preview;
  - chooseAndImport, which opens QFileDialog titled "Use custom sound" with the filters "Sound files (*.mp3 *.wav *.json)", "MP3 audio (*.mp3)", "WAV audio (*.wav)" and "Sound pack JSON (*.json)", then calls `m_sounds->importFile`.
- Public `void handleInstanceRequest(const InstanceRequest&)`:
  - `if (isMinimized()) showNormal(); show(); raise(); activateWindow();`
  - If m_sessionRecoveryDialog is set, raise and activate it (closeEvent pattern 912-915).
  - Otherwise, if !m_sessionTransition, for each URL: `m_pages->setCurrentIndex(0); addBrowserTab(url);`
  - If the window is still not active, call `QApplication::alert(this)`.
- ~MainWindow, first:
  1. `m_activity->disconnect()`
  2. `m_power.reset()`, which releases the request
  3. `m_soundOutput->stop()`

**src/ui/SettingsPage.{h,cpp}** (no new includes)
- `struct SoundHooks { std::function<QList<QPair<QString,QString>>()> packs; std::function<bool(QString pack, QString loudness)> preview; std::function<SoundImportOutcome()> chooseAndImport; };`
  - `struct SoundImportOutcome { bool cancelled; QString id, name, error; };`
  - `void setSoundHooks(SoundHooks)` rebuilds the content if the General section is visible.
- buildGeneral:
  - Loudness items Off/Subtle/Loud, with data off/normal/loud. On change, save, then preview if the new value is not off.
  - `notificationSoundPack` QComboBox, filled from hooks.packs or just ("ade-chime", "ADE Chime"). "default" selects ade-chime. On change, save and preview.
  - `notificationSoundPreview` QToolButton, disabled when loudness is off or there is no hook. Its enabled state updates on loudness change without a rebuild.
  - `notificationSoundImport` button, "Use custom sound…":
    - cancelled shows nothing;
    - an error calls showStatus(error, true) and leaves the preference unchanged;
    - success calls setPreference("agent.notificationSound", id), then buildContent(), then showStatus("<name> is now selected for notifications.").
  - Remove the line-739 placeholder. The keep-awake copy becomes "Prevent the system from sleeping while agents actively work. Waiting for a question or approval counts as idle."
- validPreferences gets the 3.2 value checks.

**CMakeLists.txt**
- Add `winmm` to the ade_ui link libraries.
- Add a loop `foreach(attention_test IN ITEMS agent_activity notification_sounds power_request)` using the control_ui_test pattern: ade_ui, Qt6::Test, the offscreen plugin, TIMEOUT 120, offscreen env.
- `test_instance_coordinator`: built from tests/test_instance_coordinator.cpp and src/ui/InstanceCoordinator.cpp, linking Qt6::Network and Qt6::Test. Add a helper target `instance_client_helper` (InstanceCoordinator.cpp plus a tiny main), registered via a compile definition with its path.
- Add `test_instance_window_host` (ade_ui, offscreen).

## 4. Slices and tests

### Slice 1: isAwaitingUser and AgentActivityMonitor
Files: AgentEngine.h, AgentActivityMonitor.{h,cpp}, tests/test_agent_activity.cpp, CMakeLists.txt.

Tests use a real default AgentEngine as the emitter with a snapshot lambda:
- `doneOnSuccessfulFinishWithEmptyQueue`
- `noDoneWhenQueueNonEmpty`
- `noDoneWithoutAnyAssistantMessage`
- `noDoneOnCancel`
- `errorOnRealFailOnly` (busy=false, failed, finished gives exactly one Error)
- `rejectedSendWhileBusyIsSilent` (busy=true, failed gives nothing, and the next successful finish gives Done)
- `questionOnApprovalDedupedById` (a, a, b gives 2; after a busy cycle, a gives 1 more)
- `questionToolTransitionEmitsOnceAcrossSteps` (false, true, true(new token), false, true gives 2; notifyQuestionTool=false gives 0)
- `activeCountExcludesAwaitingUser` (coalesced, one emission per change)
- `destroyedEngineReleasesActivity`
- `realEngineAwaitingApprovalIsIdle` (mandatory; FixtureServer pattern from test_core.cpp:32 and 359-380): isAwaitingUser is true and activeCount is 0 between approvalRequired and approveTool, and activeCount is 1 while the tool runs.

### Slice 2: NotificationSoundService (no OS)
Files: NotificationSounds.{h,cpp}, SoundOutput.h, tests/test_notification_sounds.cpp.

Tests:
- `gainMapping`
- `synthesizedWavIsValidAndDeterministic`: valid PCM16, 44.1 kHz mono, 600 ms or less, non-silent; the same bytes across two calls; the events differ.
- `scalePcm16Wav`: 10000 becomes 2510; clamps at gain 1.5; rejects non-PCM16 or truncated input.
- `debounceDoneOnly`: the clock starts at 0; the first Done plays.
- `focusResetsDebounce`, `visibleAndFocusedSuppressed`, `offPlaysNothingAndDoesNotArmDebounce`, `failedOutputDoesNotArmDebounce`, `missingEventDeliversNothing`
- `packFallback`: "default", "", "missing", ".." and "../x" all resolve to ade-chime.
- `importSingleMp3`: the name is the basename without its extension.
- `importManifest`
- `importWav` (PCM16) and `importRejectsNonPcm16Wav`
- `importRejectsTraversal`, `importRejectsAbsoluteAndAds`, `importRejectsBadHeader` (0xFF 0xC0 rejected, 0xFF 0xFB accepted), `importRejectsEmptySounds`, `importRejectsOversize`, `importRejectsSymlink` (QSKIP if a symlink cannot be created), `importRejectsInvalidJson`
- `importAtomicOnFailure`: no UUID or staging directory remains.
- `staleStagingIgnoredAndCleaned`
- `packsListingAndDisplayNames`
- `previewUsesDoneSoundAndDoesNotArmDebounce`

### Slice 3: WinSoundOutput, Settings UI and MainWindow sound wiring
Files: SoundOutput.cpp, SettingsPage.{h,cpp}, MainWindow.{h,cpp}, CMakeLists.txt (winmm), tests/test_settings.cpp.

test_settings:
- `generalSoundControlsWithoutHooks`
- `generalSoundControlsWithHooks`: selecting a pack persists and previews once; changing loudness previews once and does not preview when switching to off; preview enablement follows loudness; chooseAndImport success selects the id and shows the status; an error shows the error status and leaves the preference unchanged; cancel shows nothing.
- `agentPreferenceValidation`: "max", "..", "../x" and 65-character ids are rejected; off, normal, loud, "default" and UUIDs are accepted; an existing file with the default values reloads valid.

test_notification_sounds:
- `winOutputSmoke`: QSKIP unless ADE_AUDIO_SMOKE=1. Plays a WAV, then plays again immediately, which exercises the stop-then-replace order.

Host test:
- `mainWindowUsesNullEffectsOffscreen`

Evidence: a General-section render through render_settings after the gate.

### Slice 4: keep-awake
Files: PowerRequest.{h,cpp}, MainWindow.{h,cpp}, tests/test_power_request.cpp.

Tests:
- `acquireOnlyWhenEnabledAndActive`, `disablingReleasesImmediately`, `destructorReleases`, `acquireFailureIsRetriedOnNextUpdate`
- `winBackendRoundTrip`: acquire and release return true, and the handle is not leaked (a second acquire after release succeeds).
- Host test `hostBusyAcquiresAwaitingReleasesPrefOffReleases`, using a fake via setSystemEffectsFactory plus the FixtureServer approval flow.

Manual evidence: admin `powercfg /requests` shows "ADE agents are working".

### Slice 5: InstanceCoordinator
Files: InstanceCoordinator.{h,cpp}, tests/test_instance_coordinator.cpp, the instance_client_helper target.

Tests:
- `firstIsPrimaryAndWritesSidecar`
- `secondSameDirForwardsAndPrimaryReceives`: the secondary acquire runs on a worker thread while the main thread pumps events.
- `crossProcessForward`: the helper process exits with Forwarded; the primary receives 2 URLs.
- `differentDataDirsBothPrimary`
- `filtersUrls`: javascript:, file:, stagewise:, "-flag" and URLs over 2048 bytes are dropped; 17 are capped to 16.
- `rejectsOversizedMalformedWrongVersionFrames`, `slowClientTimesOut`
- `requestsBufferedUntilReady`
- `closingPrimaryRefusesAndLockHandsOver`
- `lockHeldButNoServerFails` (300 ms)
- `primaryReleaseAllowsNewPrimary`
- `listenFailureStillPrimary` (injectable)

### Slice 6: main.cpp and MainWindow activation
Files: main.cpp, MainWindow.{h,cpp}, tests/test_instance_window_host.cpp.

Tests:
- `handleInstanceRequestRestoresAndOpensTabs`: minimized to normal, 2 more browser tabs.
- `handleInstanceRequestDuringTransitionOnlyActivates`
- `recoveryDialogIsRaised`, if a test seam exists.

Manual or packaged evidence:
- Launching twice with the same --data-dir: the second exits 0 within 5 s and the first window comes to the front (screenshot).
- A different --data-dir: both run.
- `--prompt`, `--import-keys` or `--workspace` while running: exit 3 with a message box.
- `--smoke` runs alongside a GUI instance.

## 5. Risks
- MCI mpegvideo depends on system codecs, which Windows N editions may lack. In that case it returns false and preview reports that the sound cannot play. The MCI volume curve versus -12 dB is unverified. Media Foundation decoding is a possible future step.
- Only one PlaySound runs at a time, so overlapping sounds cut each other off. Upstream may overlap. This is documented.
- Windows may still deny foreground activation. Mitigations are AllowSetForegroundWindow(pid) and a QApplication::alert fallback.
- The SetThreadExecutionState fallback is per-thread, so keep it on the GUI thread.
- The hot files are all in the frozen206 diff. Rebase on the post-gate tree.

## 6. Out of scope
- macOS dock bounce and an optional taskbar flash preference.
- OS toasts.
- Plan-limit and subscription error suppression.
- Custom URL schemes, protocol registration and auth-callback forwarding.
- Forwarding workspace, prompt or keys.
- A UI for removing imported packs; upstream has none.
- Any upstream audio assets or pack names.

Parity is not claimed until slices 1-6 pass and the packaged evidence (screenshots, an audible preview note and powercfg output) is captured.

### Critical files
- D:/age/ADE/src/ui/MainWindow.cpp
- D:/age/ADE/src/ui/SettingsPage.cpp
- D:/age/ADE/src/core/AgentEngine.h
- D:/age/ADE/src/main.cpp
- D:/age/ADE/CMakeLists.txt