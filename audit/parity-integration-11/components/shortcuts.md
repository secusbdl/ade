# Native shortcut ledger design: 12 shortcut gaps (corrected)

## 0. Ground rules and the pending Settings patch

**Settings patch** (`private/settings-shortcuts-stage/settings-shortcuts.patch`; `git apply --check` passes on HEAD 91ed23d):
- It re-parents `Ctrl+N/B/Shift+O/T/Alt+T/J/Shift+G` at `src/ui/MainWindow.cpp` 246-253 to `central` (= `m_pages->widget(0)`, the `appSurface` page created at MainWindow.cpp:106).
- It re-parents `Ctrl+Alt+B/Shift+B/Shift+E/W` at `src/ui/MainWindowWorkbench.cpp` 96-106 to `m_pages->widget(0)`.
- It adds `tests/test_settings_shortcuts.cpp`. Its helper `shortcut()` returns the first QShortcut whose keys contain the sequence, so each sequence must keep exactly one QShortcut.
- **Integration rule:** integrate this patch first. S2 edits its test and its patched lines.
- **Hash-pinned files:** the patch HANDOFF cohort pins `MainWindow.h`, `MainWindowControl.cpp`, `MainWindowConversations.cpp`, `MainWindowRecovery.cpp`, `MainWindowSideChats.cpp`, `SessionStore.*`. S1-S6 must not touch them. S7 lands only after the cohort is unfrozen.

**Binding rules**
1. **Global workbench actions** use `bind()` in `MainWindow::initializeNavigationShortcuts` (`src/ui/MainWindowShortcuts.cpp:32-45`).
   - Its guard `enabled()` = `m_pages->currentIndex()==0 && !activeModalWidget && !activePopupWidget`. The command center is a WindowModal `QDialog` (CommandCenter.cpp:45), so this also covers upstream's "disabled while command center open".
   - **Refactor:** give `bind` a 4th lambda parameter `bool dominant = true` that sets `adeDominantNavigation`. Today the value is hard-coded `true` at line 41. `TerminalWidget::event` (TerminalWidget.cpp:126-141) yields any enabled QShortcut that carries this property; Ctrl+` is already hard-coded there as an application key.
2. **Keys the patch covers (Ctrl+Shift+G, Ctrl+Shift+E)** stay parented to `m_pages->widget(0)`.
3. **Tab-content keys** (Ctrl+Shift+V, Ctrl+Alt+C/G/F) are `Qt::WidgetWithChildrenShortcut` QShortcuts owned by the tab content widget. Their owners are hidden in Settings, so they cannot fire there.
4. **Ctrl+C stop:**
   - The composers handle it in `keyPressEvent`.
   - The window fallback is a QShortcut on `m_pages->widget(0)` that is **enabled only while a stop target is enabled**.

**Windows key notes**
- Upstream `Mod` is Ctrl on Windows. Upstream matches physical `ev.code`, with special rules for `/` and `+/-` (hotkeys.ts:566-660). Qt matches by layout. AltGr is Ctrl+Alt.
- Ctrl+` and Ctrl+Alt+/ may be unreachable on non-US layouts. This is documented, not fixed.
- **WebView2 limit:** Qt QShortcuts never see keys while focus is inside WebView2 content, and no `AcceleratorKeyPressed` forwarding exists in `src/browser`. Every binding below is marked "not from web content". Follow-up slice S10 covers forwarding.

## 1. Upstream contract (pinned 17a0123a, apps/browser/src/shared/hotkeys.ts; all lines verified)

### Simple bindings

| id | Win key (line) | dominant | Handler and scope |
|---|---|---|---|
| toggle_context_selector | Ctrl+I (156-159) | yes | panel-footer.tsx:1451-1474. Only when `hasVisibleBrowsingTab` (434-445): active tab is browser, content not collapsed, not `stagewise://internal/`, not owned by another agent. Toggles element selection, then focuses the chat input. Listener enabled = `isActiveChatSurface` (404-406). |
| toggle_tab_pin | Ctrl+Alt+P (366-369) | yes | agent-hotkey-bindings.tsx:276-288. Requires openAgent, not collapsed, an active tab. Toggles `agentInstanceId` between null and openAgent. |
| toggle_file_tree_diff | Ctrl+Shift+G (469-472) | no | agent-hotkey-bindings.tsx:306-314: `setFileTreeVisible(true)` + mode `'diff'`. Not a toggle. Shares the key with FIND_PREV (418-421), which native does not have. TOGGLE_FILE_TREE_FILES is Ctrl+Shift+E, dominant (465-468, handler 296-304). |
| toggle_svg_code_mode | Ctrl+Shift+V (473-476) | yes | file-preview-tab-content.tsx:1891-1897. Enabled when `focusedPanel==='tab-content'`, in **either** mode. Mode store `markdownModeStore` (94), shared with Markdown, in memory, default `'preview'`. |
| center_image | Ctrl+Alt+C (477-480) | yes | zoomable-viewport.tsx:84-92: fit at `min(avail/size,1)` and center. Enabled only when tab content is focused, and for SVG only in preview mode (1660-1663, 1931-1934). |
| cycle_svg_bg | Ctrl+Alt+G (481-484) | yes | 1943-1956. Order: default→light→dark→checkerboard→custom. Enabled on tab-content focus **regardless of mode**. Checkerboard: 16px tile with 8px squares (1562-1572). Custom from `preferences.general.svgCustomBackground`, bare hex, default `'ffffff'` (1800-1808). |
| cycle_svg_fg | Ctrl+Alt+F (485-488) | yes | 1958-1965. default↔custom; focus only. Default colour: dark bg → base-50, light bg → base-900, else foreground. Custom from `general.svgCustomForeground`, default `'8b5cf6'`. Injected as `<style>:root{color}</style>` (1842-1868). |
| toggle_markdown_preview | Ctrl+Shift+V (489-492) | yes | 2259-2266. Same store as SVG. Markdown = `md` or `mdx` only (263-266). |

### Bindings with detailed rules

**edit_nearest_user_message**
- Key: Ctrl+Shift+E on Windows (173-177), Cmd+Shift+I on mac. Dominant.
- Handler: chat-history.tsx:895-935. The listener is enabled while there is no active edit.
- While the agent is working, or when no message qualifies, the handler returns `undefined`. That counts as handled, so the key is still swallowed.
- Geometry: skip rows with `bottom>visibleBottom || bottom<=visibleTop || top>=visibleBottom`. Pick the minimum `visibleBottom-bottom`.

**cycle_model_thinking_effort**
- Key: Ctrl+Alt+/ (187-190). Dominant. Slash matches the produced `/`, NumpadDivide, or the physical Slash key when Alt is held.
- Handler: model-select.tsx:813-867, registered at 879-882 with **no enabled gate**, so it is not blocked while busy.
- Returns `false` (key passes through) for: no model, an alias, an active preset, or no display state.
- Otherwise it writes `agent.modelThinkingOverrides[instanceId][modelId]={enabled:true,provider,value}`.

**stop_agent**
- Key: Ctrl+C (241-244), literal Ctrl on all platforms. Not dominant.
- Handler: panel-footer.tsx:1494-1505. Enabled when `isActiveChatSurface && canStopAgent`. If a side-chat tab is active, only the side chat's footer is active.
- `shouldPreserveNativeCopy` (798-816): keeps native copy if the input/textarea has a selection, or the document has any non-collapsed selection.
- `use-hotkey-listener.tsx:53-60` exempts STOP_AGENT from the editable guard.
- It calls `abortAgent` (769-788), which includes an early-abort revert.

**focus_or_toggle_terminal**
- Key: Ctrl+` (255-259). Dominant.
- Handler: agent-hotkey-bindings.tsx:158-201, using visible tabs only (117-128) and `getNextTerminalId` (134-150). Steps in order:
  1. No visible terminal: un-collapse and create one.
  2. Content collapsed: un-collapse and focus the active terminal, else the next one.
  3. Active terminal has focus: collapse.
  4. Active tab is a terminal: focus it.
  5. Otherwise: focus the next terminal after the active tab, wrapping.
- Disabled while the command center is open (31).

## 2. Current native state (verified)

**Shortcuts**
- `MainWindowShortcuts.cpp` binds none of the 12 ids.
- `MainWindow.cpp:253`: Ctrl+Shift+G → `showGit()`.
- `MainWindow.cpp:184`: `sourceControlButton` tooltip reads "Source control (Ctrl+Shift+G)".
- `MainWindowWorkbench.cpp:98-102`: Ctrl+Shift+E shows the tree and focuses `m_files`, but does not change mode.

**Terminal**
- `MainWindow::showTerminal` (816-830) starts `ConPtyProcess::defaultShell()`: pwsh or powershell, without -NoProfile, in the workspace, else `QDir::homePath()`.
- `TerminalWidget::keyPressEvent` (106-115) sends Ctrl+C to the process.
- An unstarted `new TerminalWidget` owns a model and an unstarted process. It accepts every ShortcutOverride except application or dominant keys.
- Agent shells are `agentShellPage` QWidgets (MainWindowShells.cpp:42-46), not TerminalWidget tabs.

**Splitter**
- `WorkbenchSplitter`: `contentVisible()` = hasContent && !collapsed. Also `contentCollapsed()`, `setContentCollapsed()` (persists to window.ini), `showContent()`, `exclusiveContent()` (<520px), `setFileTreeVisible()`.

**Element selection**
- `MainWindowBrowserTools.cpp:63-196`: checkable `selectBrowserElement`. Its `clicked(bool)` handler refuses when there is no session or the session is archived, then calls `BrowserInspector::setSelecting`.
- The button is enabled when `isReady && !inspector->isBusy()`. Busy is true only during setup (BrowserInspector.cpp:197-243), so Ctrl+I can turn an active selection off.
- `adeOwnerAgentId` is set only on mini-app browsers (MiniAppHost.cpp:73). Mini-app browsers also carry `miniAppBrowser`.

**Stop**
- `Composer` is a QPlainTextEdit (Composer.h:18). Escape → `stopRequested`. MainWindow.cpp:193-194 wires it to `m_stop`, which cancels `m_engines[m_currentId]`.
- `m_stop` is enabled only while busy (MainWindow.cpp:470-472).
- SideChatPane has `sideChatStopButton`, enabled when `m_busy` (SideChatPane.cpp:33, 37, 52).
- The transcript is read-only: QTextBrowser (MarkdownParts.cpp:156-163), read-only CodeEditor, selectable QLabels.

**Image preview**
- `ImagePreview` has a private `fit()`, `zoom()` and `imageFit`, and no shortcuts.
- `test_image_preview` compiles ImagePreview.cpp alone (CMakeLists.txt:213-219, offscreen).

**File tabs**
- `MainWindow::openFile` (780-791) always creates an `EditorWidget`. `CodeEditor` is added directly to the layout (EditorWidget.cpp:56).
- The command center open-at-line path sets the cursor and focuses `editor->editor()` (MainWindowCommandCenter.cpp:97).
- `test_editor` links only ade_editor and Qt6::Test, runs on the native platform and never shows a window (CMakeLists.txt:528-532).

**File tree modes**
- `initializeWorkspaceChanges` (MainWindowWorkbench.cpp:160-189): `fileTreeFilesMode` and `fileTreeChangesMode` in an exclusive group. The mode is persisted as window.ini `fileTreeViewMode`.
- The controller reverts to files mode only on `NotRepository`; an empty workspace is `NoWorkspace`.

**Thinking effort**
- `ModelSelection::resolve` (ModelSelection.cpp:51-85) applies overrides.
- `cleanModelCatalog` keeps `reasoningEfforts` and `defaultReasoningEffort` for external harnesses only.
- `selectedModelChain` call sites: MainWindow.cpp:432, 521; MainWindowControl.cpp:151, 210 (comparison only); MainWindowSideChats.cpp:88, 121.
- `SettingsPage::setPreference` (648-656) accepts unknown keys unchecked and emits `preferencesChanged` → `MainWindow::applyPreferences` (MainWindow.cpp:268).

**Tab ownership and history edit**
- Tabs are global. There is no per-agent visibility.
- There is no user-message history edit.

## 3. Classification

| id | Class |
|---|---|
| focus_or_toggle_terminal | a (S1) |
| toggle_context_selector | a, with limits: not from web content (S1) |
| toggle_file_tree_diff | a, rebind (S2) |
| stop_agent | a (S3a composer, S3b window fallback) |
| center_image | a for ImagePreview (S4); b for SVG (S6) |
| toggle_markdown_preview | b (S5) |
| toggle_svg_code_mode, cycle_svg_bg, cycle_svg_fg | b (S6) |
| cycle_model_thinking_effort | b (S7, discovered harness efforts only) |
| toggle_tab_pin | c (S8, blocked on per-agent tabs) |
| edit_nearest_user_message | c (S9, blocked on history edit) |

## 4. Slices

### S1: Ctrl+` and Ctrl+I (MainWindowShortcuts.cpp only)

**Files and includes:** `src/ui/MainWindowShortcuts.cpp`. Add includes for `terminal/TerminalWidget.h`, `browser/browserwidget.h` and `<QToolButton>`. No header change.

1. **`bind` gets `bool dominant = true`.** It sets `shortcut->setProperty("adeDominantNavigation", dominant)`. Existing callers are unchanged.

2. **`bind("focus_or_toggle_terminal", {"Ctrl+`"}, cb)`, dominant.**
   - Build `terms` = indices `i` with `qobject_cast<TerminalWidget*>(m_tabs->widget(i))`. Set `cur = m_tabs->currentIndex()` and `curTerm = qobject_cast<TerminalWidget*>(m_tabs->currentWidget())`.
   - `next` = the first entry of `terms` that is `> cur`, else `terms[0]`.
   - Then follow the upstream steps in order:
     - `terms` is empty → `showTerminal()`.
     - `m_splitter->contentCollapsed() || !m_splitter->contentVisible()` → target = `curTerm ? cur : next`. Call `setCurrentIndex(target)`, `revealContent()`, then focus that TerminalWidget with `Qt::ShortcutFocusReason`.
     - `curTerm && curTerm->hasFocus()` → `setContentCollapsed(true)`. If the composer is visible (exclusive layout), focus it.
     - `curTerm` → focus it.
     - Otherwise → `setCurrentIndex(next)`, `revealContent()`, focus.
   - Guard with `QPointer<MainWindow>` / `QPointer<TerminalWidget>` after every observer-running call.

3. **`bind("toggle_context_selector", {"Ctrl+I"}, cb, false)`.**
   - Not dominant: Ctrl+I is Tab inside the terminal, and the action can never apply while a terminal is current. This is a documented deviation; upstream swallows the key.
   - Callback, in order:
     - `session=current()`; return if null or archived.
     - Return if `!m_splitter->contentVisible()`.
     - `page = m_tabs->currentWidget()`; return if it has no `browserTabId`.
     - `browser = page->findChild<BrowserWidget*>()`; return if null, if `miniAppBrowser` is set, or if `adeOwnerAgentId` is non-empty and differs from `m_currentId`.
     - `select = page->findChild<QToolButton*>("selectBrowserElement")`; return if null or disabled.
     - `select->click()`, then, if `self` is alive and the composer is visible, focus the composer.

**Tests:** `tests/test_navigation_shortcuts.cpp`, all using `activate()`.

- `focusOrToggleTerminalCreatesTerminalWhenNone`
  - Seed agent0 with a workspace in a `QTemporaryDir`.
  - Ctrl+` from the composer → one more tab; the current widget is a `TerminalWidget` named `workspaceTerminal`; `contentVisible()`; focus is the terminal.
  - Then `terminal->stop()` and `QTRY_VERIFY(!terminal->isRunning())`. This is the only live-process test in the slice.
- `focusOrToggleTerminalRevealsCollapsedContentAndFocusesActive`
  - Add two unstarted terminals and a plain QWidget tab. Make the second terminal current and collapse the content.
  - Ctrl+` → content visible, index unchanged, focus is that terminal.
- `focusOrToggleTerminalCollapsesWhenFocusedAndCyclesFromOtherTab`
  - Focused current terminal + Ctrl+` → `contentCollapsed()`.
  - Current plain tab between two terminals → the next terminal becomes current.
  - Current last tab → wraps to the first terminal.
- `terminalYieldsCtrlBackquoteButKeepsCtrlI`
  - Focused unstarted terminal. Ctrl+I: the `shortcut_toggle_context_selector` spy stays 0 and focus stays on the terminal.
  - Ctrl+`: that shortcut's spy is 1.
- `contextSelectorClicksSelectButtonOnlyForVisibleOwnBrowserTab`
  - Open `openBrowserButton`; get `select` and a `QSignalSpy` on `QToolButton::clicked`.
  - **Negative cases first**, each preceded by `select->setEnabled(true)`: collapsed content → 0; `adeOwnerAgentId="other"` → 0 (then reset); `miniAppBrowser=true` → 0 (then reset); `setEnabled(false)` → 0.
  - **Positive case last:** `setEnabled(true)`, Ctrl+I from the composer → spy 1 and focus is the composer.
- `contextSelectorIgnoresNonBrowserTabsAndArchivedChats`
  - Archived: select the "archived" chat.
  - Non-browser: a QWidget tab.
- Extend `mainNavigationIsSuppressedInSettingsAndModalDialogs`: emit `activated()` for each new shortcut with Settings shown and then with a modal QDialog open. The tab count and the select spy must be unchanged.

### S2: Ctrl+Shift+G → changed-files mode; Ctrl+Shift+E → files mode (after the Settings patch)

**Edits**
- `src/ui/MainWindow.cpp`:
  - Delete the patched `new QShortcut(QKeySequence("Ctrl+Shift+G"),central,...showGit...)`.
  - Change the `sourceControlButton` tooltip (line 184) to "Source control".
  - The workbench menu's "Source control" entry and the command center keep `showGit` reachable.
- `src/ui/MainWindowWorkbench.cpp`, `initializeWorkbench`:
  - Patched Ctrl+Shift+E lambda, in this order: `m_splitter->setFileTreeVisible(true)`; click `fileTreeFilesMode` (looked up lazily from `m_fileTreePanel`); `if (m_files->isVisible()) m_files->setFocus()`. Set `objectName("shortcut_toggle_file_tree_files")` and `setProperty("adeDominantNavigation", true)` (upstream is dominant).
  - New `auto* diff = new QShortcut(QKeySequence("Ctrl+Shift+G"), m_pages->widget(0), [this]{ m_splitter->setFileTreeVisible(true); if (auto* b = m_fileTreePanel->findChild<QToolButton*>("fileTreeChangesMode")) b->click(); });` with `diff->setObjectName("shortcut_toggle_file_tree_diff")`. Not dominant.
  - `initializeWorkspaceChanges()` runs later (line 107); the lookups are lazy, so the order is safe.

**Tests**
- `tests/test_settings_shortcuts.cpp`, `returnRestoresActualShortcut`, Ctrl+Shift+G branch:
  - `QCOMPARE(tabs->count(), count)`
  - `fileTreeChangesMode` is checked
  - `WorkbenchSplitter::fileTreeVisible()`
  - The draft is unchanged.
  - The other two test functions stay as they are.
- `tests/test_navigation_shortcuts.cpp`:
  - `fileTreeDiffShortcutSelectsChangedFilesMode`: tree visible, changes mode checked, tab count unchanged, no `gitPanel` tab, and the `sourceControlButton` tooltip does not contain "Ctrl+Shift+G".
  - `fileTreeFilesShortcutSelectsFilesMode`: from changes mode, Ctrl+Shift+E checks files mode.
  - `fileTreeFilesShortcutWorksFromTerminal`: dominant, so it works from an unstarted terminal.
  - `fileTreeDiffShortcutDoesNotStealFromTerminal`: the mode is unchanged and focus stays on the terminal.

### S3a: Ctrl+C in composers

**`src/ui/Composer.h`**
- Add the signal `stopShortcutRequested()`.
- In `keyPressEvent`, after `handleCompletionKey` and before Escape: `if (event->key()==Qt::Key_C && event->modifiers()==Qt::ControlModifier && !textCursor().hasSelection()) { emit stopShortcutRequested(); event->accept(); return; }`.
- Use the exact Ctrl+C test, not `QKeySequence::Copy`, which also matches Ctrl+Insert.
- The editable QPlainTextEdit accepts the Copy ShortcutOverride, so the key reaches `keyPressEvent`.

**Wiring**
- `src/ui/MainWindow.cpp`, next to line 194: `connect(m_composer, &Composer::stopShortcutRequested, m_stop, &QToolButton::click);`. `click()` does nothing while `m_stop` is disabled, which matches `canStopAgent`.
- `src/ui/SideChatPane.cpp:37`: connect `m_composer->stopShortcutRequested` to `m_stop->click()`, so it is gated by the pane's busy state.

**Documented deviations**
- No early-abort revert: native stop has none, and Escape and the button behave the same way.
- Ctrl+C from the source editor (CodeEditor) with nothing selected does not stop. Upstream stops from Monaco.

### S3b: window-level Ctrl+C fallback

**Shortcut and target resolver** (in `initializeNavigationShortcuts`)
- `auto* stop = new QShortcut(QKeySequence("Ctrl+C"), m_pages->widget(0))`, objectName `shortcut_stop_agent`.
- `WindowShortcut`, `autoRepeat=false`, **no** `adeDominantNavigation`, so the terminal and editable text keep Ctrl+C.
- `stopTarget()` returns:
  - the pane's `sideChatStopButton`, if `m_splitter->contentVisible()` and `qobject_cast<SideChatPane*>(m_tabs->currentWidget())` (upstream `isActiveChatSurface`);
  - otherwise `m_stop`.
- `refreshStopShortcut()`: `stop->setEnabled(enabled() && target && target->isEnabled())`. Disabled while idle, so Ctrl+C copying in item views and read-only text is untouched.

**Refresh hooks** (none in pinned files)
- `m_tabs` `currentChanged`
- `m_splitter` `visibilityChanged`
- the end of `MainWindow::updateComposerState` (MainWindow.cpp), via `findChild<QShortcut*>("shortcut_stop_agent")`
- a new `SideChatPane::stopAvailabilityChanged(bool)` signal, emitted from `updateActions()`, connected on tab change with `Qt::UniqueConnection`

**`activated` callback**
1. Re-check `enabled()`.
2. If the focus widget has a text selection, copy it and return:
   - `QTextEdit` / `QPlainTextEdit` with `textCursor().hasSelection()` → `copy()`
   - `QLineEdit::hasSelectedText()` → `copy()`
   - `QLabel::hasSelectedText()` → `QGuiApplication::clipboard()->setText(selectedText())`
3. Otherwise `target->click()`.

**Verify item views:** if Qt 6.11's QAbstractItemView accepts the Copy override, the agent-list test below fails. In that case, replace the QShortcut with a ShortcutOverride event filter on `m_pages->widget(0)`.

**Tests:** `tests/test_agent_control_ui.cpp`, existing hanging `Provider`. Every key test uses an `activate()` helper (WA_DontShowOnScreen, setActiveWindow).
- `ctrlCInComposerWithoutSelectionStopsBusyAgent`: QTRY not busy; the queue is preserved.
- `ctrlCWithComposerSelectionCopiesAndKeepsAgentRunning`: the clipboard equals the selection and the engine is still busy.
- `ctrlCFromTerminalDoesNotStopAgent`: unstarted TerminalWidget tab.
- `ctrlCFromAgentListStopsBusyAgent`
- `ctrlCWithTranscriptSelectionCopiesWhileBusy`: select text in a `TranscriptText`; the clipboard has it and the engine is still busy.
- `ctrlCWhenIdleDoesNotActivateFallback`: `shortcut_stop_agent` is disabled; an activated spy stays 0; no engine is created.
- `ctrlCInSideChatTargetsSideChat`: if side-chat fixtures are too heavy for this file, put it in `test_side_chat_host`.

`registeredNavigationWorksFromTerminalWithoutStealingCopy` keeps passing.

### S4: center_image for ImagePreview

**`src/ui/ImagePreview.h/.cpp`**
- Add `public: void fitToView() { m_fitting = true; fit(); }`. Make the `imageFit` button call it.
- In the constructor: `auto* center = new QShortcut(QKeySequence("Ctrl+Alt+C"), this); center->setContext(Qt::WidgetWithChildrenShortcut); center->setObjectName("imageCenterShortcut");` connected to `fitToView`.
- No MainWindow change.

**Optional, not part of the parity claim:** Ctrl+= / Ctrl+- / Ctrl+0 in the same scope.

**Tests:** `tests/test_image_preview.cpp`, shown with `QApplication::setActiveWindow` and `qWaitForWindowActive`.
- `centerShortcutRefitsAfterManualZoom`: click `imageZoomIn` twice, focus `imageCanvas`, Ctrl+Alt+C → `imageScale` equals the fresh-fit text and the transform m11 is ≤ 1.
- `centerShortcutRequiresFocusWithinPreview`: focus an active top-level QLineEdit → the scale is unchanged.

### S5: EditorWidget preview host plus Markdown preview, Ctrl+Shift+V

**`src/editor/EditorWidget.h/.cpp`** (ade_editor)
- **Layout:** replace `layout->addWidget(m_editor,1)` with `QStackedWidget* m_body` (objectName `editorBody`) holding `m_editor`. Without a pane, `editor()` and focus behaviour stay identical.
- **New API:**
  - `void setPreviewPane(QWidget*)`: adds the pane to `m_body`, creates `QShortcut("Ctrl+Shift+V", this)` with WidgetWithChildrenShortcut and objectName `editorPreviewShortcut`, and shows the toolbar button.
  - `bool hasPreview() const`, `bool previewVisible() const`, `void setPreviewVisible(bool)`, `void showSource()`.
  - Signal `previewVisibilityChanged(bool)`.
- **Toggle behaviour:**
  - Toggling focuses the newly visible page.
  - The shortcut works in both modes (EditorWidget scope), as upstream does.
  - Toolbar button `editorPreviewToggle`: text "Source" or "Preview"; tooltip "Show source (Ctrl+Shift+V)" or "Show preview (Ctrl+Shift+V)"; visible only with a pane.
- **Find:** `showFind()` calls `showSource()` first.
- **Mode cache:** `static QHash<QString,bool>` keyed by canonical path, in memory only (upstream `markdownModeStore`). Default is preview. Shared by Markdown and SVG.

**New `src/ui/FilePreviews.h/.cpp`** (UI_SOURCES glob)
- `class MarkdownFilePreview : public QScrollArea` hosts `MarkdownParts` (MarkdownParts.h:54-62).
  - Renders from `editor->editor()->toPlainText()`, so unsaved edits are shown.
  - Re-renders only while visible: a 150 ms single-shot QTimer on `textChanged`, plus once on show.
  - Above 2 MiB of text it shows the label "File too large to preview".
  - `imageRequested` and `linkActivated` are left unconnected. TranscriptText and TranscriptDocument already block resource loads (MarkdownParts.cpp:34-45, 212).
- `void attachFilePreview(EditorWidget*)`: lower-case suffix `md` or `mdx` → Markdown; `svg` → SVG (S6).

**Call sites**
- `MainWindow::openFile`: after a successful `editor->openFile`, call `attachFilePreview(editor)`.
- `MainWindowCommandCenter.cpp:97`: call `editor->showSource()` before positioning when `line>0`. That file is not pinned.

**Tests**
- `tests/test_editor.cpp`, non-keyboard only: `previewHostApiPreservesUndoAndModified` (dummy QLabel pane; `setPreviewVisible` toggles; undo stack and modified flag preserved; `hasPreview()` is false without a pane).
- **New** `tests/test_file_previews.cpp` (offscreen, activated window):
  - `previewShortcutTogglesOnlyWhenFocusInsideEditor`
  - `markdownDefaultsToPreviewAndRendersUnsavedEdits`
  - `markdownModeIsRememberedPerFileWithinProcess`
  - `mdxIsPreviewedAndDotMarkdownIsNot`
  - `markdownPreviewDoesNotLoadImagesOrActivateLinks`: a temp PNG beside the file; `![x](pic.png)` and `[l](https://example.test)`; `QDesktopServices::setUrlHandler` for http, https and file records nothing after clicking the anchor; no QNetworkAccessManager children; `blockedResourceLoads` ≥ 0 and no image rendered.
  - `twoMarkdownTabsOnlyFocusedOneToggles`
  - `openAtLineSwitchesToSource`
  - `closingModifiedPreviewTabPrompts`: through MainWindow; QTRY-find the modal QMessageBox, reject it, and the tab remains modified.

### S6: SVG preview (Ctrl+Shift+V via S5; Ctrl+Alt+C, G, F)

**`class SvgFilePreview : public QWidget`** in FilePreviews.
- **Sanitizing:** the text passes through `QXmlStreamReader` → `QXmlStreamWriter`.
  - Drop `href` and `xlink:href` unless the value starts with `#` or `data:image/`.
  - Drop `<script>` and `<foreignObject>`.
  - This is required: QtSvg's `<image>` handler appears able to load local raster files by path for untrusted documents. That could not be confirmed against source (not shipped with the Qt 6.11.2 install), so the sanitizer is required regardless, and the test proves it.
  - The same pass rewrites the **root** `<svg>` start tag: replace or insert `color="#rrggbb"` and keep any inline `style`. This mirrors upstream's `:root{color}` override.
- **Rendering:**
  - `QSvgRenderer` with `setOptions(QtSvg::DisableAnimations)`; never `AssumeTrustedSource`.
  - Render to a QImage capped at 4096² inside a QGraphicsView with fit and zoom, like ImagePreview.
  - Re-render only while visible, debounced 150 ms. Above 5 MiB, show "File too large to preview".
- **Background:** `Default|Light|Dark|Checkerboard|Custom`.
  - Checkerboard: 16px tile with 8px squares (Window and AlternateBase palette roles).
  - Custom: `general.svgCustomBackground`, stored as bare hex, default `ffffff`.
  - Normalized by an exact port of `normalizeHexColor`: 3 or 6 hex digits without `#`, otherwise the fallback.
- **Foreground:** `Default|Custom`.
  - Default: Dark bg → light text; Light bg → dark text; otherwise palette Text.
  - Custom: `general.svgCustomForeground`, default `8b5cf6`.
- **Preferences:** add both keys to `SettingsPage::defaultPreferences()` as strings, so `matchingKnownTypes` type-checks them. There is no colour picker UI in this slice.
- **Shortcuts** (WidgetWithChildrenShortcut on the preview pane):
  - `Ctrl+Alt+C` fit and center: preview only, matches upstream.
  - `Ctrl+Alt+G` / `Ctrl+Alt+F` cycle: preview only. This is a **documented deviation**; upstream also fires them in source mode. Scoping them to the pane keeps AltGr letters typable in the source editor.
- **Invalid SVG:** an inline label "Cannot render this SVG". The editor stays usable.

**Tests** (`test_file_previews`)
- `svgDefaultsToPreviewAndCtrlShiftVTogglesSource`
- `svgBackgroundCyclesFiveStatesInUpstreamOrder`, via an exposed `background()` accessor
- `svgForegroundCyclesAndAppliesCurrentColor`: a `fill="currentColor"` rect; the pixel is `#8b5cf6` in custom mode
- `svgForegroundOverridesExistingRootColorAttribute`
- `svgCustomColorsReadBareHexPreferences`
- `svgHotkeysInactiveInSourceModeOrWithoutFocus`
- `svgCenterShortcutRefits`
- `invalidSvgShowsErrorAndKeepsEditor`
- `externalImageReferenceIsNotLoaded`: a red PNG in a temp dir referenced by absolute path and by file:/// URL → no red pixels in the render
- `animatedSvgDoesNotAnimate`

### S7: cycle_model_thinking_effort, Ctrl+Alt+/ (after the cohort is unfrozen)

**New `src/ui/ModelThinking.h/.cpp`** (pure functions)
- `QStringList thinkingOptions(const ProviderConfig&, const QJsonObject& catalogEntry)`: the discovered harness `reasoningEfforts` only. Empty for API providers; the upstream capability table is not ported.
- `QString nextThinkingOption(const QStringList&, const QString& current, const QString& fallbackDefault)`: wraps around and starts from the default when the current value is unknown.
- `QJsonObject validThinkingOverrides(const QJsonValue&)`: structural validation. Objects of objects, entries `{enabled: bool, provider: string, value: string ≤ 32}`, at most 256 instances × 1024 models.
- `ModelSelection withThinkingOverride(ModelSelection, const QJsonObject& overrides)`: sets `overrides.reasoningEffort` only for non-preset selections.

**`src/ui/ModelSelection.h/.cpp`**
- `selectedModelChain(..., const QJsonObject& thinkingOverrides = {})`, applied per non-preset entry.
- Pass `validThinkingOverrides(m_settings->preference("agent.modelThinkingOverrides"))` at MainWindow.cpp:432 and 521, MainWindowControl.cpp:151 (and 210 for consistency), and MainWindowSideChats.cpp:88 and 121.

**SettingsPage defaults:** `agent.modelThinkingOverrides: {}`.

**Shortcut:** `bind("cycle_model_thinking_effort", {"Ctrl+Alt+/"}, cb)`, dominant.
- **No-op when:** no session, an archived session, a preset, or no options.
- **Not blocked while busy:** upstream has no gate. The value takes effect at the next `setModelChain`.
- **Write:** build the whole object and call `setPreference("agent.modelThinkingOverrides", obj)` once. Never use a dotted per-model path.
- **On false:** show the storage error and stop.
- **On success:** update the current `m_models` item text suffix ` · <effort>` without a full `refreshModels()`, and set the status "Reasoning effort: <effort>". Never send.

**Tests**
- `tests/test_model_selector_display.cpp`:
  - `thinkingOptionsUseOnlyDiscoveredEfforts`
  - `nextThinkingOptionWrapsAndStartsFromDefault`
  - `presetSelectionIgnoresThinkingOverride`
  - `selectedModelChainAppliesOverrideReasoningEffort`
  - `malformedOverridesAreIgnored`
- `tests/test_navigation_shortcuts.cpp`:
  - `cycleThinkingEffortPersistsOverrideWithoutSending`: harness-kind provider whose `harnessExecutable` is a nonexistent path; a catalog built with `cleanModelCatalog`; efforts `["low","medium","high"]`. Expect the preference written, the combo text ending `· medium`, no AgentEngine, and the draft unchanged.
  - `cycleThinkingEffortNoOpForApiProviderAndPreset`
  - The Settings/modal suppression loop includes this shortcut.

### S8 (blocked): toggle_tab_pin, Ctrl+Alt+P

**Prerequisite: per-agent content tabs**
- An `ownerAgentId` on each page.
- `QTabWidget::setTabVisible` filtering on agent switch.
- Visible-only iteration for next/prev, focus_tab_N and S1 terminal cycling.
- `BrowserWidget::adeOwnerAgentId` kept in sync for `BrowserToolBridge::canAccessTab`.
- Owner persisted in BrowserSessionStore.

**Shortcut once the prerequisite exists:** `bind(..., {"Ctrl+Alt+P"})`, active when `contentVisible()` and a current page exists. It sets the owner to `owner==m_currentId ? "" : m_currentId`.

**Tests:** `tabPinMovesTabBetweenGlobalAndAgentScope`, `pinnedTabHiddenForOtherAgentAndToolAccessDenied`, `tabNavigationSkipsHiddenTabs`.

### S9 (blocked): edit_nearest_user_message, Ctrl+Shift+E on Windows

- **Dependency:** the history-edit design.
- **Selection:** `ConversationView::nearestEditableUserMessage()` applies the exact upstream geometry rule.
- **Conflict resolution:** this is a native-only choice. Upstream lets listener registration order decide, and swallows the key even while working. Native rule: if focus is in the transcript or composer and an editable message is visible and the agent is idle, edit it; otherwise run files mode.
- **Ledger:** record this as a deviation.
- **Tests:** `editNearestUserMessagePicksBottomMostVisibleEditableRow`, `editNearestDisabledWhileBusyOrEditing`, `ctrlShiftEPrefersMessageEditFromChatAndFileTreeElsewhere`.

### S10 (follow-up, not designed here): WebView2 accelerator forwarding

Forward Qt shortcuts marked `adeDominantNavigation` (Ctrl+I, Ctrl+`, Ctrl+Alt+/, Ctrl+L, …) from WebView2 `AcceleratorKeyPressed` to the host. Until then, the ledger marks these shortcuts "not from web content".

## 5. CMake

`test_file_previews`:
- `qt_add_executable(test_file_previews tests/test_file_previews.cpp)`
- `target_link_libraries(... PRIVATE ade_ui Qt6::Test Qt6::QOffscreenIntegrationPlugin)`
- `qt_import_plugins` offscreen
- `WIN32_EXECUTABLE FALSE`, `RUNTIME_OUTPUT_DIRECTORY bin`
- `add_test(NAME native_file_previews ...)` with `QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=...`, timeout 90

New production files are picked up by the globs (CMakeLists.txt:91, 94). `ade_ui` already links Qt6::Svg (line 125). The patch's `CMAKE-ADDITIONS.txt` (`test_settings_shortcuts`) must be integrated before S2.

## 6. Risks
- Layout-dependent matching and AltGr. Widget-scoped Ctrl+Alt letters avoid stealing typing in editors.
- No shortcuts from inside WebView2 (S10).
- The S1 live ConPTY test uses a temp-dir workspace and stops explicitly. It still runs the user's PowerShell profile; this is documented.
- The S3b fallback depends on the enabled-state refresh hooks. If item views accept the Copy override, fall back to an event filter.
- QtSvg differs from browser SVG; mitigated by sanitizing, disabled animations and pixel tests.
- The EditorWidget stack refactor must keep `test_editor` green.
- Ctrl+Shift+G no longer opens Source control. The tooltip is updated and the change is recorded in the ledger.

## 7. Out of scope
- FIND_PREV and browser find.
- Raster image file previews.
- The API-provider thinking capability table.
- mac aliases (Cmd+J).
- SVG colour pickers.
- Early-abort revert on stop.
- Ctrl+C stop from the source editor.
- S8, S9 and S10.

### Critical Files for Implementation
- D:/age/ADE/src/ui/MainWindowShortcuts.cpp
- D:/age/ADE/src/ui/MainWindowWorkbench.cpp
- D:/age/ADE/src/ui/Composer.h
- D:/age/ADE/src/editor/EditorWidget.cpp
- D:/age/ADE/src/ui/ModelSelection.cpp