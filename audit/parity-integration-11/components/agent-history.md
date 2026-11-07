# Conversation history actions: native design for ADE (agent-history), corrected

Upstream is `upstream/stagewise` at 17a0123a (same blobs as `reference/stagewise`). Every native path and function below was opened. Line numbers refer to the current dirty working tree. Prior approved scope: `private/agent-history-stage/HANDOFF.json`, which approves failed-turn Retry with no file undo as the first slice.

**Hard ordering:** make no edits until the full206 gate checkpoint is captured.

---

## 1. Upstream contract (verified)

### Backend: `packages/agent-core/src/agents/base-agent.ts`

**`replaceUserMessage`, lines 1072-1104**
1. Finds the selected message by id.
2. Collects the tool-call ids of every assistant message in `history.slice(idx)`.
3. Optionally calls `toolbox.undoToolCalls`.
4. Calls `externalRuntime.resetThread()`.
5. Calls `state.commands.replaceUserMessage`.
6. Calls `sendUserMessage(new)`.

**`retryLastUserMessage`, lines 1113-1140**
- Requires `state.error`.
- Walks backward to the last user message.
- Calls `revertToUserMessage(id, false)`, then `sendUserMessage(sameMessage)`.

**`revertToUserMessage`, lines 1159-1198**
- Throws while `isWorking`.
- Optionally undoes tool calls, resets the thread, calls `truncateHistoryAt`, then `scheduleMemorySnapshotWrite`.

**State mutations, `services/agent-manager/state-mutations/history.ts:24-59`**
- Both mutations clear `queuedMessages` in the same `store.update()` that changes history.

**Error lifecycle, `state-mutations/lifecycle.ts:53-90`**
- `beginStep` clears `state.error` at every step start.
- `recordStepError` sets it.

**RPC wiring, `agent-manager.ts`**
- 636-664 and 772-777, delegating at 1864-1911.
- An offline auto-retry exists at 945-970. It is out of scope.

**Contract:** `apps/browser/src/shared/karton-contracts/ui/index.ts:1415-1426`.

**File undo, `services/diff-history/index.ts:1262+`**
- Restores each file separately, with try/catch per file.
- No intervening-edit check. Not atomic.

### UI: `apps/browser/src/ui/screens/main/agent-chat/chat/_components`

**Retry**
- `chat-history.tsx:984-989`: `canRetry` = error && !isWorking && the last filtered message is a user message.
- `message-runtime-error.tsx:87`: `canShowRetry = canRetry || !isWorking`, so Retry shows whenever the agent is idle with an error.
- Ctrl+Enter priority is `ERROR_RETRY = 40`.

**Edit (`message-user.tsx`)**
- `canEdit = !isWorking && !!msg.id` (line 152). Clicking the bubble starts editing (744-747).
- Submit is ignored when the text has 2 characters or fewer (320-322).
- When there are later file modifications, the Keep/Revert popover is shown.
- Submission is a single `replaceUserMessage` call (217-307).
- Note: upstream sets `isEditing=false` *before* awaiting. On failure it only closes the popover, so it does not actually stay in edit mode.

**File-change map:** `chat-history.tsx:504-536`. An assistant tool output carrying `_diff` or `_hasFileWrites` counts as a file change.

**Edit-nearest shortcut**
- `chat-history.tsx:895-936`: ignored while working. Enabled only when no editor is active (`!activeEditMessageId`). Picks the editable bubble whose bottom edge is closest to the visible bottom and fully above it.
- `shared/hotkeys.ts:173-177`: `Ctrl+Shift+E` on Windows.
- Conflicts with `TOGGLE_FILE_TREE_FILES` `Mod+Shift+E` (465-468). Both are capture-phase window listeners, and the first registered one wins via `stopImmediatePropagation` (`use-hotkey-listener.tsx:60-70`).

**Restore checkpoint**
- `message-assistant.tsx:322-344, 447-476`: shown on non-last assistant messages.
- `panel-footer.tsx:663-743`:
  - finds the next user message;
  - calls `revertToUserMessage` without awaiting it;
  - clears and restores composer text and attachments;
  - if running, stops first and restores once idle.

**Early-abort revert:** `panel-footer.tsx:748-790` (deferred).

---

## 2. Native facts the design depends on

**`AgentEngine::startUserMessage` (AgentEngine.cpp:219-310), private**
- Signature: `(prompt, refs, pathRefs, pathMeta, queuedId={}, immediateMessage={})`.
- Resolves `m_fallback` and `m_provider` first (225-231), runs preflights, then `recoverInterruptedHistory(m_messages)`.
- Builds the display envelope (272-273).
- Calls `commitQueue` only for queued or immediate messages (281-285), then publishes. `turnAboutToStart` is a reentrancy point.

**Failure, cancel and complete (`AgentEngine.cpp`)**
- `fail()` (1098-1121):
  - sets `m_queuePaused=true` and increments `m_generation`;
  - calls `stopHarness`, then `m_display.error(...)`, then `retainPartialResponse`.
  - For HTTP, an empty-output failure retains no row (837-839). The error then lives only in the provisional display row (`displayMessages`, 175-178).
  - Harness failures always retain a row, because `m_display` is non-empty (`retainHarnessResponse`, AgentEngineHarness.cpp:100-123).
- `cancel()` (1123-1145) returns early when idle.

**Queue (`AgentEngineControl.cpp`)**
- `commitQueue` (42-68) returns true when no committer is set.
- `sendUserMessage` (88-130) and `resumeQueue` (192-206) set `m_queuePaused=false` before starting.
- `scheduleQueue` (207-216) honours `m_queuePaused`.

**Harness continuity**
- `HarnessHistory::resumable` (HarnessHistory.cpp:60-83) resumes when the prefix's last assistant row carries a proof matching `canonicalDigest(prefix)`.
- A completed harness turn strips earlier bindings and binds only the newest row (AgentEngineHarness.cpp:105-110).
- A failed turn binds nothing, so the prefix's last assistant still proves resumability. Retry, edit and revert must call `HarnessHistory::withoutBindings(prefix)`.
- `dispatchPreparedHarness` (AgentEngineHarness.cpp:222-245) then starts a fresh session with the `<conversation-history>` transcript.

**Compression**
- `ade_compressedHistory` lives only on `role=="user"` rows (`applyCompression`, AgentEngine.cpp:145-152; AgentEngineCompression.cpp:73-115).
- For Retry the failed user is the last user row, so any boundary is ≤ i and the summary covers prefix rows only.

**Mutations (`AgentEngineMutations.cpp`)**
- `registerMutation` (57-75) records the invocation and commits.
- `settleMutation` (86-112) silently drops settlements whose invocation row is gone. It sets `m_mutationHistoryDirty = !saved || !m_queueCommitter`.
- `reconcileMutationHistory` (137-168) recovers settlements.

**Journal**
- `FileMutationJournal::undo` (FileMutationJournal.cpp:108-117) ignores `requestId` and generates its own nonce.
- `FileMutationStore::undoMany` (FileMutationStore.cpp:358-377):
  - accepts 1-100 committed ids;
  - all-or-nothing;
  - fails on any intervening edit.
- Original rows stay `committed` after an undo (undo rows carry `undo_of`, line 223).
- `describe(nonce)` (337-341) returns the state.
- Token rule: `^[A-Za-z0-9_-]{1,128}$` (line 111).

**Untracked writes**
- `runCommand` in `ToolExecutor.cpp:381,397` runs PowerShell without journaling.
- External harness edits are not journaled.

**Host committer (MainWindowControl.cpp:180-236)**
- Builds a candidate and calls `m_store.save`, which locks and CAS-checks via `validateRevision`, SessionStore.cpp:162-188.
- Its custom-model guard relies on the `newUser` size heuristic (199-219).
- `queueError` shows "Queue: …" (175-178).

**Persistence (SessionStore)**
- `Conversation` (SessionStore.h:10-18).
- `load` is strict and refuses the whole store on any invalid record (SessionStore.cpp:77-148).
- `writeState` (189-219).
- `cloneConversation` (MainWindowConversations.cpp:98-108) copies `*source` and clears per-owner fields at 108.

**Wiring**
- `engineFor` (MainWindow.cpp:378-417): `setMessages` at 382, `finished` persistence at 413.
- `send` (418-466) and `sendSideChat` (MainWindowSideChats.cpp:117-127).
- `connectTurnFileControls` (MainWindowTurnFiles.cpp:16-77), called at MainWindow.cpp:128 and MainWindowSideChats.cpp:62.
- `refreshSideChat` (MainWindowSideChats.cpp:40-55).
- `visibleTranscript` (MainWindowLinks.cpp:95).

**ConversationView**
- `ConversationView.cpp:433-488`: `MessageRow`.
- 493-494: `Private` (`order` holds group keys).
- 554-555: activity and tail.
- 625-633: `callbacks.retryTurn` only re-fetches turn files.
- Assistant rows are grouped (`TranscriptGrouping.cpp:10-29`); user rows never group.

**Shortcuts**
- One `Ctrl+Shift+E` QShortcut exists (MainWindowWorkbench.cpp:98). The F5 patch moves it to `m_pages->widget(0)`.
- `MainWindowShortcuts.cpp:32-45` `bind()` creates one QShortcut per action. A duplicate key causes `activatedAmbiguously`.

**Icons:** drawn `reload` and `clock` exist in Icons.cpp; `edit` and `more` do not.

**Fixtures**
- Codex: `ADE_HARNESS_FIXTURE_SCENARIO=thread-error`.
- OpenCode: `ADE_ACP_SCENARIO=session-error`.
- Claude: `ADE_CLAUDE_FIXTURE=error`, already present.
- Cursor: scenarios `auth-error` (after the authentication approval), `load-error`, `missing-model`.
- CMake: `test_harness_continuity` depends on `harness_fixture acp_fixture claude_fixture` (869). `cursor_fixture` is defined at 1205.

**Pending patches:** `git apply --check` passes for `question-priority.patch` and `settings-shortcuts.patch`.

---

## 3. Behaviour per backend

| Backend | Retry | Edit | Revert | Thread reset | File undo |
|---|---|---|---|---|---|
| HTTP (openai, responses, anthropic, google) | yes | yes | yes | n/a; the prefix, including raw signed `ade_native` blocks, is replayed unchanged | Committed journal mutations from built-in file tools and sandbox file requests. `runCommand` changes are not tracked and are disclosed. |
| Codex app-server, OpenCode ACP, Claude Code, Cursor ACP | yes | yes | yes | `stopHarness()` (idle) plus `withoutBindings(prefix)`. The next turn uses a fresh session with the canonical transcript. External on-disk sessions are abandoned, not deleted. | External edits are not journaled. "Revert files" covers journaled ids only and says so; it is disabled when there are none. |
| Side chats | own agent id | own | own | same | Only `agentId == side chat id`. Parent records are not cloned (`cloneFileContext` does not copy the journal). |

---

## 4. Slice 0: prerequisites

1. Wait for the full206 checkpoint.
2. Apply `private/question-priority-stage/question-priority.patch`. It changes only `AgentEngine::sendUserMessage` and adds `tests/QuestionFixture.h` and `tests/test_question_priority.cpp`. Add its CMake lines from `CMAKE-ADDITIONS.txt`.
3. Apply the F5 `settings-shortcuts.patch`. It changes `MainWindow.cpp:246-252` and `MainWindowWorkbench.cpp:96-106`, and adds `test_settings_shortcuts.cpp`. Do this before slice 2.
4. Build and run the focused gates (`question_priority`, `settings_shortcuts`, `navigation_shortcuts`, `main_chrome`). Record the checkpoint in `private/claude-main-handoff/PROGRESS.json`.

---

## 5. Slice 1: explicit Retry of a failed turn (no file undo)

### 5.1 Engine: `AgentEngine.h`, `AgentEngine.cpp`, new `src/core/AgentEngineHistory.cpp`

**Public API**
```cpp
QJsonObject turnFailure() const { return m_turnFailure; }
static bool validTurnFailureShape(const QJsonObject& record);                // also used by SessionStore
static bool validTurnFailure(const QJsonArray& history,const QJsonObject& record);
bool restoreTurnFailure(const QJsonObject& record);   // idle only; validates against m_messages
bool retryFailedTurn(const QString& userRowId,QString* error=nullptr);
enum class CommitKind { Update, AcceptUser, ReplaceAndAcceptUser, ReplaceHistory };
CommitKind commitKind() const { return m_commitKind; } // meaningful only inside QueueCommitter
```
No new signal. Hosts read `turnFailure()` during their existing displayChanged, finished and busyChanged refreshes.

**Private additions**
- `QJsonObject m_turnFailure`
- `QString m_turnUserRowId`
- `CommitKind m_commitKind = CommitKind::Update`
- A trailing private `startUserMessage` parameter: `const std::optional<QJsonArray>& replacementPrefix = std::nullopt`.

**Record**

`{version:1, userRowId:<^[A-Za-z0-9_-]{1,128}$>, error:<redacted string>, failedAt:<ms ≥0>}`.

`validTurnFailureShape` requires:
- exactly those keys and types;
- `error` of at most 2000 characters;
- compact JSON of at most 4096 bytes.

`validTurnFailure` additionally requires, for the last `role=="user"` raw row:
- `metadata.display.version==1` and `metadata.display.id == userRowId`;
- the projected id from `AgentPresentation::history(history)` for that row equals `userRowId`.

Legacy rows are therefore never retryable after a restart (honest limit).

**`startUserMessage` changes**
- When a prefix is present:
  - `recoverInterruptedHistory(*replacementPrefix, ...)`;
  - `remaining = {}`;
  - always call `commitQueue`.
- Set `m_commitKind` to `AcceptUser` for immediate or queued messages, or `ReplaceAndAcceptUser` for a prefix, around `commitQueue`. Reset it to `Update` on every exit (use an RAII guard).
- On acceptance:
  - `m_turnUserRowId = message.metadata.display.id`;
  - `m_turnFailure = {}`;
  - when a prefix is present and a committer exists, `m_mutationHistoryDirty=false`;
  - emit `queuedMessagesChanged` whenever `remaining != old queue`.
- Do **not** call `stopHarness` or `m_fallback.reset()` here.
- Preacceptance rejection leaves `m_turnFailure` untouched.

**Other changes in `AgentEngine.cpp`**
- `fail()`, after `settleOutstandingTools` and its guard, before `emit displayChanged()`:
  - `m_turnFailure = makeFailure(m_turnUserRowId, safeMessage, now)`;
  - `makeFailure` shrinks `error` until `validTurnFailureShape` holds;
  - skip if `m_turnUserRowId` is empty.
- `complete()` and the busy branch of `cancel()` (after `if(!m_busy) return;`): clear `m_turnFailure`.
- `setMessages()` clears it; `engineFor` restores it afterwards.

**`AgentEngineHistory.cpp::retryFailedTurn(row)`**

Refuse when any of these holds:
- `m_busy || m_committingQueue || isCompressing()`;
- `m_turnFailure` is empty or its `userRowId != row`;
- `!validTurnFailure(m_messages, m_turnFailure)`.

Then:
1. Let `i` be the index of the last user row, and `suffix = m_messages.mid(i+1)`.
2. Refuse ("File changes from the failed turn are still being recorded. Retry after they settle.") when the suffix contains a `mutationInvocations` entry with `agentId == m_mutationAgentId` that is either:
   - unsettled; or
   - without a tool result row, using the same predicate as `reconcileMutationHistory`.

   Also refuse when `m_mutationHistoryDirty` is set and the suffix contains any such entry.
3. While idle, call `stopHarness()` and then `m_fallback.reset()`. If `m_messages` changed, refuse with "The conversation changed; try again."
4. `prefix = HarnessHistory::withoutBindings(m_messages.mid(0, i))`.
5. Rebuild the user message:
   - copy `role`, `content`, `ade_compressedHistory` and all metadata except `display`;
   - set a new display envelope identical to `startUserMessage`: `{version:1, id:newUuid, parts:[{id:newUuid+"/0", type:"text", text:content, state:"done"}]}`.

   A fresh id is a documented native difference: upstream reuses the message id.
6. Set `m_queuePaused=false`, then call `startUserMessage(content, attachments, pathRefs, pathMeta, {}, rebuilt, prefix)`. If it is not accepted and the engine still exists, set `m_queuePaused=true`.

Never read the composer draft. Captured attachment bytes are resolved by the existing preparer at request time. A missing or corrupt snapshot therefore gives zero provider requests and a new failed turn, which is again retryable. This deviates from the handoff wording "refuses before dispatch"; the deviation is recorded.

### 5.2 Persistence: `src/ui/SessionStore.h/.cpp`

- Add `QJsonObject turnFailure` to `Conversation`.
- `load`: when the key is present, it must be an object passing `AgentEngine::validTurnFailureShape` (link-safe: implement the predicate in a small header-only `src/core/TurnFailure.h` so SessionStore does not need `AgentEngine`). Otherwise the existing strict "Original data was preserved" error applies.
- `writeState`:
  - refuse to save when a non-empty `turnFailure` fails the shape check;
  - write the key **only when non-empty**.
- `cloneConversation`, `MainWindowConversations.cpp:108`: `fork.turnFailure = {}`.

### 5.3 Host: `MainWindow.cpp`, `MainWindowControl.cpp`, new `src/ui/MainWindowHistory.cpp`, `MainWindow.h`

**`engineFor`:** right after `engine->setMessages(s->messages)` (MainWindow.cpp:382), call `engine->restoreTurnFailure(s->turnFailure)`.

**`finished` handler (MainWindow.cpp:413):** also set `s->turnFailure = engine->turnFailure()` before `saveSessions()`.

**QueueCommitter (MainWindowControl.cpp:199-235)**
- `kind = owner->commitKind()`.
- `newUser = heuristic || kind==AcceptUser || kind==ReplaceAndAcceptUser`.
- When `kind != Update`, set `chat.turnFailure = {}` in the same candidate.
- For the custom-model refusal with `kind != Update`, use: "The selected custom model changed or was removed. Select the intended model, then retry."

**`MainWindowHistory.cpp`**

`void connectHistoryControls(ConversationView* view)`
- Call it next to both `connectTurnFileControls` call sites.
- Connect `failedTurnRetryRequested(chat, row)` with these guards:
  - `visibleTranscript(chat, view, true)`;
  - the session exists and is not archived;
  - a side chat's parent exists and is not archived;
  - the main view accepts only `chat == m_currentId`; a side view accepts only its pane's id.

`bool retryFailedTurn(chat, row, view)`
1. Refuse when any of these holds:
   - `m_sessionTransition`;
   - `gitPathBusy(workspace)`;
   - `host && host->mutationBusy()`;
   - `m_mutationUndoAgent == chat`;
   - (slice 3) a pending `historyRevert` intent.
2. `engineFor(chat)`; re-check `self`, the session and the archived state.
3. Resolve the saved selection with `ModelSelection{providerId, model, presetId, {}, {}}.resolve(...)`. On failure: "Select an available model before retrying."
4. `setModelChain(selectedModelChain(...), presetId)`, then `setWorkspace`, `setAlwaysAllow`, and `host->setWorkspace` (return on error).
5. Call `engine->retryFailedTurn(row, &error)`.
6. On any refusal:
   - show the error in `m_status` (main) or `SideChatPane::setError` (side); this overwrites the "Queue:" text;
   - **always** call `view->setHistoryActions(chat, historyActionsFor(chat))` so the button re-enables.
7. Never touch `draft` or `draftAttachments`, and never clear the composer.

`ConversationView::HistoryActions historyActionsFor(chat) const`
- The record is `engine->turnFailure()` if an engine exists; otherwise `session->turnFailure` when `AgentEngine::validTurnFailure(session->messages, it)` passes.
- Fill `queuedCount`, `busy` and `archived`.
- Push it in `renderConversation()` after `setConversation` (MainWindow.cpp:369) and in `refreshSideChat` after `pane->setState`.

### 5.4 UI: `ConversationView.h/.cpp`

**Header**
```cpp
struct HistoryActions { QString failedUserRowId, failureText; int queuedCount=0; bool busy=false, archived=false;
    bool editable=false;                         // slice 2
    QHash<QString,QString> revertBlockedReason; };// slice 3: rowId -> reason
void setHistoryActions(const QString& conversationId,const HistoryActions& actions);
signals: void failedTurnRetryRequested(QString conversationId,QString userRowId);
```

**Error card**
- `QFrame#transcriptRuntimeError` is added to `d->layout` after `activity` and before `tail`. It holds:
  - `QLabel#transcriptRuntimeErrorText`, shown only when the last projected row has no `type=="error"` part (after a restart, or for an empty-output failure);
  - `QLabel#transcriptRetryQueueNote`: "Retry clears %n queued message(s)." when `queuedCount > 0`;
  - `QPushButton#transcriptRetryButton` "Retry" with `icon("reload")`.
- Visible when `!busy && !archived`, `failedUserRowId` is in `d->order` with role user, and no user row follows it.
- `setHistoryActions` always re-evaluates visibility and enabled state.

**Click handling:** capture `id` and `contextEpoch`, disable the button, emit the signal. Actions are dropped on conversation switch and on `clear()`.

**Out of scope:** Ctrl+Enter `ERROR_RETRY` is a recorded gap.

### 5.5 Slice 1 tests

**`tests/test_history_retry.cpp`** (`QuestionFixture.h` `Server`; links `ade_core` + `src/ui/SessionStore.cpp`)
- `retryAfterEmptyOutputFailureSendsAcceptedUserOnce_data/()`, 4 protocols.
  - The first turn finishes; then `sendUserMessage("RETRY_ME", atts, refs, meta)` and `reply(1, "{}", 400)`.
  - Assert:
    - `turnFailure().userRowId` equals the user display id;
    - Retry returns true;
    - the third request contains RETRY_ME exactly once and no error text;
    - the history has 2 user rows;
    - attachments, refs and meta are byte-identical;
    - the display id changed and the projected id equals the new display id;
    - `turnFailure()` is empty.
- `retryRemovesPartialSuffixAndPreservesSignedPrefix_data/()`, 4 protocols. Seed the prior native batch through `setMessages`. The failure is a stream with no completion marker (`AgentEngine.cpp:812-813`).
- `retryClearsQueueInSameCommit`: exactly one committer call after Retry, with `(prefix+user, [])` and `commitKind()==ReplaceAndAcceptUser`; `queuedMessagesChanged` emitted.
- `retryUnpausesQueueForLaterMessages` (new): queue a message during the retried turn, finish it, and assert the queued message auto-dispatches.
- `rejectedCommitLeavesEverythingUnchanged`: messages, queue, `turnFailure`, request count and `m_queuePaused` behaviour are unchanged; `queueError` fires once.
- `successfulCanceledAndPreacceptanceTurnsAreNotRetryable`; also `idleCancelKeepsFailure` (new).
- `staleRowBusyAndPostFailureSendRejected`.
- `unsettledSuffixMutationBlocksRetry`; also `dirtyPrefixOnlyDoesNotBlock` (new).
- `restoreTurnFailureValidatesShapeAndIdentity`: wrong id, a user row after it, oversize bytes (multibyte and control characters), non-object, a display envelope that does not match the content.
- `failureRecordFitsShapeForMultibyteError` (new): a provider error of 2000 non-ASCII and control characters still produces a record that passes the shape check.
- `ownerDeletedDuringRetryPublication`.
- `duplicateRetryIsIdempotent`.

**`tests/test_history_retry_harness.cpp`**
- Codex (`thread-error`), OpenCode (`session-error`), Claude (`ADE_CLAUDE_FIXTURE=error`), and Cursor (`missing-model`, or approve authentication and then `auth-error`, whichever CursorFixture verifies fails after acceptance).
- Steps: "KEPT_FIRST" succeeds, "FAILED_TURN" fails, unset the scenario, retry.
- Assert:
  - the last wire has `<conversation-history>` with KEPT_FIRST once and FAILED_TURN only as the new prompt;
  - the fixture log shows a new thread or session (no resume or load of the earlier one);
  - no `continuity` in the prefix.
- CMake block modelled on `CMakeLists.txt:867-874`, with `add_dependencies(test_history_retry_harness harness_fixture acp_fixture claude_fixture cursor_fixture)`.

**`tests/test_history_actions_host.cpp`** (`ade_ui`, offscreen, `test_side_chat_host` helpers)
- `mainRetryAppearsOnlyAfterAcceptedFailure`: draft "Unsent draft" is preserved, `requests.size()==2`, and the saved `turnFailure` is cleared.
- `successfulAndStoppedTurnsHaveNoRetry`.
- `restartRetainsRetryAndCapturedAttachment`: the original bytes are sent after `ws1/a.txt` is rewritten.
- `casConflictRefusesRetry`: the status contains "another window"; requests, history, queue and draft are unchanged; **the button is re-enabled**.
- `queueNoteAndAtomicClear`.
- `sideChatRetryIsOwnerScoped`.
- `staleSignalAfterArchiveOrDeleteIgnored`.
- `duplicateClickSendsOnce`.
- `customModelChangeRefusesRetryWithRetryWording` (new).

**Baseline absence of Retry:** run the same test source privately against the frozen206 cohort and save the result as evidence. It is not a CTest assertion.

**CMake**
- Add `history_retry` to the `control_test` list (CMakeLists.txt:192).
- Add `history_actions_host` to the `control_ui_test` list (199).
- Add the separate harness block above.

---

## 6. Slice 2: edit (replace) a user message

### Engine
- `static int AgentPresentation::sourceIndex(const QJsonArray& messages, const QString& rowId)`. It reuses `HistoryProjectionState` in AgentPresentation.cpp, records the source index as each row is appended, and maps legacy and `~N` ids exactly as the projection does.
- `bool AgentEngine::replaceUserMessage(rowId, text, keptAttachmentPaths, QString* error)` in AgentEngineHistory.cpp. It follows the same idle, stop and fallback steps as slice 1, plus the mutation-suffix rule and `withoutBindings`, and:
  - requires a user row at `sourceIndex`;
  - requires non-empty text or kept attachments;
  - requires `kept ⊆ original metadata.attachments[*].path`, and filters `pathReferences` and `pathMetadata` to the kept paths;
  - keeps `ade_compressedHistory` and `metadata.compression`;
  - builds a new display envelope;
  - sets `m_queuePaused=false` with restore-on-reject;
  - commits with `ReplaceAndAcceptUser`, which clears the queue and `turnFailure`.
- Adding new attachments while editing is deferred to slice 2b.

### UI: `ConversationView`
- A user `MessageRow` is editable when `actions.editable && !busy && !archived`.
- Entering edit mode: a mouse release on the bubble with no text selection (event filter on the row's text children) or `QToolButton#editUserMessage`.
- The editor is `QPlainTextEdit#userMessageEditor`, seeded with the raw content.
  - Chips get remove buttons.
  - Buttons: `#userMessageEditCancel` and `#userMessageEditSend`.
  - Enter submits, Shift+Enter inserts a newline, Esc cancels.
  - Trimmed text of 2 characters or fewer is ignored.
- Only one active editor (`d->editingRow`). It is dropped on switch, when the row disappears, or when the chat becomes busy.
- Signals and methods:
  - `userMessageEditSubmitted(chat, row, text, keptPaths, undoFiles)`, where `undoFiles` is always false in this slice;
  - `setUserEditError(chat, row, error)` keeps the editor open. This is a native improvement; upstream exits edit mode.
- `bool editNearestUserMessage()`:
  - returns false when busy or when an editor is already open;
  - among editable user rows whose bottom satisfies `top < viewportBottom && bottom > viewportTop && bottom <= viewportBottom`, picks the smallest `viewportBottom - bottom`.
- New ADE-drawn `edit` icon in Icons.cpp.

### Host
- `editUserMessage(...)` uses the same guards and model preflight as Retry. A refusal calls `setUserEditError` and re-pushes actions.
- `Ctrl+Shift+E`:
  - modify **only** the existing single QShortcut lambda at MainWindowWorkbench.cpp:98 (as moved by F5);
  - when `QApplication::focusWidget()` is inside `m_transcript`, `m_composer` or a visible `SideChatPane` and that view's `editNearestUserMessage()` returns true, stop there;
  - otherwise run the existing file-tree behaviour;
  - do **not** add a `bind("edit_nearest_user_message", ...)` QShortcut;
  - record this deliberate native resolution of the upstream conflict in the ledger.

### Tests
**`test_history_edit.cpp`**
- `replaceMiddleUserTruncatesAndSends` (4 protocols).
- `keptAttachmentSubsetAndForeignPathRejected`.
- `legacyAndCollisionRowIdsResolveToSource`.
- `rejectsBusyAssistantRowAndUnknownRow`.
- `rejectedCommitUnchanged`.
- `unsettledSuffixMutationBlocks`.
- `editPreservesCompressionBoundary`.

**`test_history_edit_harness.cpp`**: replacing SECOND gives a fresh session whose transcript has KEPT_FIRST once and no SECOND.

**`test_history_edit_ui.cpp`**
- `clickEntersEditWithOriginalTextAndChips`.
- `escCancels`.
- `shortTextIgnored`.
- `enterEmitsExactRowTextAndKeptPaths`.
- `chipRemovalExcludesPath`.
- `busyDisablesEditing`.
- `singleActiveEditor`.
- `editNearestPicksBottomMostVisible`.
- `editNearestDisabledWhileEditing`.
- `switchDropsEditState`.

**Host additions**
- `mainEditReplacesAndDispatches`.
- `sideEditOwnerScoped`.
- `ctrlShiftEEditsInChatAndTogglesTreeElsewhere`: also asserts `QShortcut::activatedAmbiguously` is never emitted. The existing `navigation_shortcuts`, `main_chrome` and `settings_shortcuts` suites must still pass.

---

## 7. Slice 3: revert or restore checkpoint, with optional journal-backed file undo

### Engine
`bool revertToUserMessage(rowId, QJsonObject* removedUser, QString* error)`
- Same guards, stop and fallback steps as slice 1, plus the mutation rule.
- `candidate = withoutBindings(m_messages.mid(0, i))`; `commitQueue(candidate, {})` with `ReplaceHistory`.
- After the commit, check `self` and the generation, then publish:
  - `m_messages = candidate`, `m_queuedMessages = {}`;
  - `m_display = {}`, `m_displayHistoryIndex = -1`, `m_responseRetained = true`;
  - clear `m_turnFailure`; when a committer exists, set `m_mutationHistoryDirty=false`;
  - emit `displayChanged` and `queuedMessagesChanged`.
- It never dispatches. `replaceUserMessage` gets an internal variant that is used once an undo has completed.

### Journal
- `FileMutationJournal::undo` uses `requestId` as the nonce when it matches the token regex; otherwise it generates one. Existing callers pass a brace-less UUID, so behaviour is unchanged.
- Add `QJsonObject FileMutationJournal::mutationState(nonce, QString*) const`, wrapping `FileMutationStore::describe`.
- Add the passthrough `AgentToolHost::mutationState`.

### Durable intent
`Conversation::historyRevert` = `{version:1, kind:"revert"|"replace", userRowId, mutationIds:[sorted, ≤100], undoRequestId, createdAt, replacement?{text, keptPaths}}`.
- A shared shape predicate (≤64 KiB compact) is enforced on write **and** load.
- The edit or revert UI rejects replacement text that would exceed it.
- Cleared in `cloneConversation`.
- While an intent is pending, send, retry, edit and revert are all refused.

### Host: undoable-id collection
1. From the removed suffix, collect:
   - tool-row `metadata.mutations` with `agentId==chat && committed && !recoveryRequired`;
   - settled `mutationInvocations[*].resultMetadata.mutations`, with the same filter.
2. Deduplicate.
3. Read `host->mutationHistory()` and **exclude ids that appear in the `undoOf` list of any committed undo record**, since originals stay `committed` after an undo.
4. Blocking reasons:
   - more than 100 ids: Revert disabled;
   - any record needing recovery: run `recoverFileChanges` first.
5. Disclosure: when the suffix contains `runCommand` calls, `metadata.harness` rows or other untracked tools, the dialog states "Changes made by shell commands or external coding agents are not reverted; use Git to restore them." Revert stays enabled for journaled ids and is disabled when there are none.

### Host: flow
1. Guards: the same as `undoFileChanges` (busy, queue, git, `m_mutationUndoAgent`), plus "Save or close unsaved editor changes" for modified editors on the affected paths.
2. Persist the intent through `commitConversationState`. If that save fails, abort with nothing changed.
3. Set `m_mutationUndoAgent` and `m_mutationUndoId`, and record `m_historyRevertRequests[chat] = undoRequestId`. Call `host->undoMutations(undoRequestId, ids)`.
4. Completion: extend the existing `connectMutationControls` handler (MainWindowMutations.cpp:93-110) so that, before clearing state, it calls `finishHistoryRevert(chat, request, output, error)` when `request` equals the pending intent's `undoRequestId`. Do not key on `m_mutationUndoId`.
   - **Error** (conflict or cancel): clear the intent with a CAS save, leaving history unchanged. Show "Files were not reverted: … The conversation was not changed."
   - **Success, revert:** `engine->revertToUserMessage`; the committer clears `historyRevert` in the same candidate when `kind==ReplaceHistory`.
   - **Success, replace:** `engine->replaceUserMessage`, which dispatches; the committer clears the intent when `kind==ReplaceAndAcceptUser`.
   - If the commit fails, keep the intent and show "Files were reverted; finishing the conversation change failed: … Retry finish." with an action.
5. Startup recovery, next to the `m_pendingMutationHydration` logic (MainWindow.cpp:344-366):
   - `mutationState(undoRequestId)` is `committed`: finalize the truncation. A `replace` intent is **not** auto-sent; its text goes into the draft with a status note.
   - missing, `rolled_back` or `failed`: clear the intent.
   - prepared, applying or recovery-required: run `recoverFileChanges` first, then re-evaluate.

### Restore into the composer (upstream `panel-footer.tsx:663-712`)
- After a successful revert, set the main `m_composer` (or the side composer) and `session->draftAttachments` from `removedUser`'s text and `metadata.attachments`.
- Upstream replaces the draft unconditionally. Native shows "Your current draft will be replaced." in the dialog when the draft is non-empty.

### UI
- Non-last, idle assistant **groups** (TranscriptGrouping) get `QToolButton#assistantRowActions` (new drawn `more` icon) with `#restoreCheckpointAction` "Restore checkpoint" (drawn `clock` icon).
- The target is the next user row after the group's last message id; if there is none, nothing happens.
- When file changes follow the row (`part.fileDiff` or committed `row.mutations` later in the projection), show `#revertConfirmDialog`:
  - "Keep or revert files?" / "Do you want to revert file changes made after this message?";
  - `Revert files` is the default, plus `Keep`;
  - `Revert files` is disabled with the reason from `revertBlockedReason[rowId]`;
  - the untracked-change disclosure is shown when applicable.
- The same dialog gates edit submit.
- Signal: `restoreCheckpointRequested(chat, assistantRowId, undoFiles)`.
- Restore while streaming is deferred; the menu is shown only when idle.

### Tests
**`test_history_revert.cpp`**
- `revertTruncatesClearsQueueNoDispatch`.
- `revertReturnsRemovedUserWithAttachments`.
- `revertStripsHarnessBindings`.
- `revertRejectedCommitUnchanged`.
- `commitKindIsReplaceHistory`.

**`test_file_mutations.cpp`** additions
- `undoUsesRequestIdAsNonce`.
- `mutationStateReportsCommittedUndo`.

**`test_history_revert_host.cpp`**
- `restoreCheckpointKeepFiles`.
- `restoreCheckpointRevertFiles`.
- `undoConflictLeavesHistory`.
- `previouslyUndoneMutationIsSkipped`.
- `crashBetweenUndoAndHistoryFinalizes`.
- `pendingIntentBlocksSendAndRetry`.
- `harnessSuffixDisablesRevertFiles`.
- `runCommandSuffixShowsUntrackedDisclosure`.
- `editWithRevertFilesUndoesThenDispatches`.
- `sideChatExcludesInheritedParentMutations`.
- `draftReplacementWarning`.

---

## 8. Risks

- **Custom-model bypass:** fixed by `commitKind`.
- **Late settlement:** mitigated by the unsettled-suffix refusal.
- **Duplicate error display:** the card hides its text when the last row has an error part.
- **`Ctrl+Shift+E`:** single-shortcut, focus-scoped dispatch.
- **Thread reset is history-level only:** external sessions are abandoned, not deleted.
- **Attachment snapshot loss:** fails after acceptance with zero provider requests.
- **Store strictness:** writer and loader must share the shape predicates, or a single bad record would refuse the whole store.
- **Untracked file changes:** `runCommand` and harness writes are disclosed, never claimed as reverted.

## 9. Out of scope / known gaps

- Ctrl+Enter `ERROR_RETRY` priority.
- Offline automatic retry (`agent-manager.ts:945-970`).
- Early-abort revert on Stop.
- Restore during streaming.
- New attachments or element selections while editing.
- Model selector inside the editor.
- Upstream memory snapshot.
- Undo of external-harness or `runCommand` edits.
- No visual or line-by-line parity is claimed.

## Critical files

- `D:/age/ADE/src/core/AgentEngine.cpp` / `AgentEngine.h` (+ new `D:/age/ADE/src/core/AgentEngineHistory.cpp`)
- `D:/age/ADE/src/ui/MainWindowControl.cpp` (QueueCommitter)
- `D:/age/ADE/src/ui/SessionStore.cpp` / `SessionStore.h`
- `D:/age/ADE/src/ui/ConversationView.cpp` / `ConversationView.h`
- `D:/age/ADE/src/ui/MainWindowMutations.cpp`
- `D:/age/ADE/src/mutations/FileMutationJournal.cpp` (slice 3)