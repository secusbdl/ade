# ADE browser agent tools: source review against pinned Stagewise

> Read-only source review of this isolated snapshot. Nothing was built, run or tested.
> Scope: DOM reading, JavaScript evaluation, screenshots, console capture, element selection, and
> ownership/cancellation across tabs, navigation and conversation changes.
> Excluded (reviewed elsewhere): URL classification and history navigation.
> **This review does not establish parity.** Findings are source-supported only; runtime effects are marked untested.

## Native surface reviewed
- `src/browser/BrowserToolBridge.{h,cpp}`: the one shared bridge for every agent. Direct tools `getBrowserTabs`, `sendCDP`, `readConsoleLogs`, plus sandbox `API.sendCDP`/`API.onCDPEvent` brokering.
- `src/runtime/AgentToolHost.cpp`: per-conversation dispatch, cancellation, event subscriptions, dynamic browser context.
- `src/browser/browserwidget.cpp`: WebView2 lifecycle, `CallDevToolsProtocolMethod`, event receivers.
- `src/browser/BrowserInspector.cpp`, `src/ui/MainWindowBrowserTools.cpp`: user element selection and screenshot attachment.
- `src/browser/BrowserContext.cpp`: tab metadata rendered into each agent's system context.
- `src/sandbox/SandboxHost.cpp`, `src/sandbox/worker_main.cpp`: sandbox broker IDs and output limits.

Upstream reference: `reference/stagewise/apps/browser/src/backend/services/window-layout/{index.ts,browsing-tab-controller.ts}`, `services/sandbox/index.ts`, `services/toolbox/tools/browser/*.ts`, `env-domains/browser-domain-adapter{.ts,.prompt.md}`, `packages/agent-core/src/services/toolbox/utils.ts`.

---

## Findings (source-supported)

### B1 — High: direct browser tool results are uncapped (up to 2 MiB) and screenshots are steered into base64 text
- **Native:**
  - `BrowserToolBridge::definitions` (`src/browser/BrowserToolBridge.cpp:171`) tells the model to use `sendCDP` with `Page.captureScreenshot` "for base64 image data" and says results are limited to 2 MiB (`MaxResponseChars`, line 17; enforced at 98-100 and 293-295).
  - Direct results go to `AgentEngine::completeExternalTool` (`src/runtime/AgentToolHost.cpp:161-163`).
  - `AgentEngine::recordToolResult` stores the full text as a `tool` message (`src/core/AgentEngine.cpp:986-994`; `storeToolMessage`, `src/core/AgentEngineMutations.cpp:129-135`). It is never truncated, persisted to `sessions.json`, and replayed to the provider on every turn.
  - Native is inconsistent with itself: the sandbox path caps output at 65,536 characters (`src/sandbox/worker_main.cpp:34,325-326,801`) and offers `API.createAttachment` for images (`src/sandbox/SandboxHost.cpp:98`).
- **Upstream contract:**
  - "Access tabs only via the sandbox" (`env-domains/browser-domain-adapter.prompt.md:3-4`).
  - Screenshots must be saved with `API.createAttachment` so they reach the model as inline images (`:8-14`).
  - Every tool result goes through `capToolOutput`, default 40 KiB (`packages/agent-core/src/services/toolbox/utils.ts:71-75`; used by `read-console-logs.ts:142` and `execute-sandbox-js.ts:63`).
- **Trigger:** the agent follows the native `sendCDP` description and calls `Page.captureScreenshot` on a normal viewport.
  - The PNG base64 (hundreds of KB, up to 2 MiB) enters history as text.
  - The next request carries it as tokens rather than an image (likely a context-window failure; untested).
  - `sessions.json` grows by that size per call. About 32 such results reach the 64 MiB save cap (`src/ui/SessionStore.cpp:205`), after which every save fails and the close wedge from REPAIR-DESIGN.md F1 follows.
  - `readConsoleLogs` with `limit` 500 and long messages has the same exposure.
- **Regression idea:** route a direct `sendCDP Page.captureScreenshot` and a large `Runtime.evaluate` through `AgentToolHost` and assert the stored tool message is at most the sandbox cap (64 KiB) with a truncation marker. Assert the screenshot is either refused with guidance to use `executeSandboxJs` and `API.createAttachment`, or stored as an attachment (`completeExternalToolWithAttachments`). Assert `sessions.json` grows by less than 64 KiB.

### B2 — Medium: user-opened tabs are global, so every agent can read and drive them and sees their URLs and titles
- **Native:**
  - `canAccessTab` grants access whenever the browser has no `adeOwnerAgentId` (`BrowserToolBridge.cpp:184-187`).
  - Only mini-app browsers get an owner (`src/apps/MiniAppHost.cpp:73`). `MainWindow::addBrowserTab` (`src/ui/MainWindow.cpp:668-746`) never sets one.
  - `snapshotForAgent` therefore lists every user tab for every agent (`BrowserToolBridge.cpp:188-208`), and it is injected into every engine's dynamic system context (`AgentToolHost.cpp:29-36`). That includes background conversations and side chats.
- **Upstream contract:**
  - A tab opened by the user goes to the open agent: `createTab(undefined, undefined, openAgent)` (`ui/screens/main/index.tsx:182`).
  - Popups inherit the parent tab's agent scope (`window-layout/index.ts:1550-1556`).
  - Agent-created tabs are assigned to the creator (`services/sandbox/index.ts:343-357`).
  - Access and prompt projection are limited to global tabs plus the agent's own (`window-layout/index.ts:3355-3369`; `browser-domain-adapter.ts:42-83`).
- **Trigger:** with chat A current, open a tab to a private or internal page. Chat B, running in the background or as a side chat, receives that URL and title in its system prompt, possibly sent to a different provider, and can run `Runtime.evaluate` in it.
- **Regression idea:** open a tab while chat A is current. `snapshotForAgent("B")` must not list it, and `executeForAgent(id,"B","sendCDP",…)` must fail with "belongs to another agent". The fix needs a tab-scoping design (per-agent strips or explicit "pin globally"), not a one-liner.
- **Missing evidence:** no `audit/` inventory in this snapshot, so I could not check whether this is a tracked native gap.

### B3 — Medium: agents cannot use tabs that were never shown (restored background tabs, collapsed content pane)
- **Native:**
  - WebView2 starts only from `showEvent`, or from `navigate`/`reload` while visible (`src/browser/browserwidget.cpp:596-598,887-905,917-921,1075-1085`).
  - The bridge refuses an unready tab with "Browser tab is still starting." (`BrowserToolBridge.cpp:258`).
  - Session restore adds every saved tab but selects one, and restores the content-collapse preference (`src/ui/MainWindowBrowserSession.cpp:32-47`).
  - The bridge copies the URL from `currentUrl()`, which is empty until initialized (`BrowserToolBridge.cpp:77`; `browserwidget.cpp:319`), and the title only from `titleChanged` (`BrowserToolBridge.cpp:79-81`). Agents are shown blank `url=""`/`title=""` tabs they cannot use.
- **Upstream contract:** restored global tabs and the open agent's tabs are created eagerly as live controllers. Only other agents' tabs are deferred, and those are created when that agent opens (`window-layout/index.ts:3140-3244`, `ensureAgentTabsCreated` 2449-2520). CDP works regardless of visibility (`browsing-tab-controller.ts:2334-2345`).
- **Trigger:** open three tabs, restart ADE, and ask the agent to read the DOM of a non-active restored tab: every call returns "Browser tab is still starting." With the content pane collapsed at restart, even the active tab never starts.
- **Regression idea:** restore two tabs; `executeForAgent(...,"sendCDP",{tabId:<background tab>,method:"Runtime.evaluate",…})` succeeds after the host starts hidden tabs (on demand or eagerly), or returns an actionable error. The snapshot shows the restored URL and title.
- **Untested:** that WebView2 can create a controller for a never-shown child window with `IsVisible=FALSE`.

### B4 — Low/Medium: one consumer's CDP unsubscribe or disable breaks others (inspector events, console capture)
- **Native:**
  - `BrowserWidget` keeps one WebView2 receiver per event name with no reference count (`browserwidget.cpp:988-1019`).
  - `BrowserToolBridge::unsubscribeEvent` protects only its own three console events (`BrowserToolBridge.cpp:146-152`). It does not protect the inspector's `Overlay.inspectNodeRequested`/`Overlay.inspectModeCanceled` (`src/browser/BrowserInspector.cpp:91-92`; the destructor comment at 114-115 acknowledges the sharing).
  - Console domains are enabled only at registration and the first ready signal (`BrowserToolBridge.cpp:85-88,307-316`). They are never re-enabled after navigation, and native code has no other `Runtime.enable` call.
- **Upstream contract:**
  - CDP events fan out per subscriber, each with its own unsubscribe (`browsing-tab-controller.ts:2355-2392`).
  - Console domains are re-enabled after every top-level load (`browsing-tab-controller.ts:1478-1491,1525,2624-2676`).
- **Trigger A:** sandbox code `const off=API.onCDPEvent(tab,'Overlay.inspectNodeRequested',()=>{}); off();`. The user's "Select element" then never receives the click, and selection stays armed.
- **Trigger B:** direct `sendCDP Runtime.disable` (or `Log.disable`), then navigate and log. `readConsoleLogs` returns 0 for that tab for the rest of its life.
- **Regression idea:** (A) bridge subscribe/unsubscribe of an Overlay event, then the inspector still receives `Overlay.inspectNodeRequested`; (B) disable, navigate, `console.log`, read: at least one log. Fix: per-consumer, reference-counted event receivers, and re-send `Runtime.enable`/`Log.enable` after each top-level navigation.

### B5 — Low: element selection or screenshot is silently lost if the user switches chats mid-capture
- **Native:**
  - The capture's conversation is fixed when the button is clicked (`src/ui/MainWindowBrowserTools.cpp:173,179`; `BrowserInspector.cpp:200,247-248,461`).
  - `attachBrowserContext` returns silently when that ID is no longer current (`MainWindowBrowserTools.cpp:26-27`).
  - Selection mode is not cancelled on conversation switch; `setSelecting` is called only from `MainWindowBrowserTools.cpp`.
- **Upstream contract:** `elementSelected` adds the element to the currently active message (`window-layout/index.ts:1615-1618`).
- **Trigger:** chat A, click "Select element", switch to chat B, click a page element. Nothing is attached anywhere and there is no status message.
- **Regression idea:** with chat B current, emit `elementSelected(element,"A")`. Expect either attachment to the current chat (upstream behaviour) or a visible notice. Alternatively, assert that switching chats cancels selection.

### B6 — Low: user "Attach a screenshot" fails on large or high-DPI viewports
- **Native:** `BrowserInspector::captureScreenshot` asks CDP for a viewport PNG (`BrowserInspector.cpp:468-469`). Replies over 2 MiB are rejected as "Inspection response is too large." (`MaxReply`, lines 11, 80-81), so any PNG over about 1.5 MiB fails. The 8 MiB decoded allowance (483) can never be reached. `BrowserWidget::requestScreenshot` (`CapturePreview`, 64 MiB cap, `browserwidget.cpp:1021-1055`) exists but nothing uses it.
- **Upstream contract:** element screenshots use `capturePage`, then compress to JPEG under 5 MB rather than failing (`browsing-tab-controller.ts:1086-1099`). The upstream full-page attach path was not fully traced.
- **Trigger:** a 2560×1440 viewport at 150 % scaling with an image-heavy page, then the camera button: error.
- **Regression idea:** a viewport producing a PNG over 1.6 MiB attaches successfully (via `requestScreenshot` or a JPEG fallback).

### B7 — Low: one global cap of 64 pending browser requests is shared by all agents
- **Native:** `executeScoped` rejects new requests once `m_pending.size() >= 64` (`BrowserToolBridge.cpp:234`). One bridge serves every `AgentToolHost` (`MainWindow.cpp:370`), while each sandbox allows 256 requests (`SandboxHost.cpp:238`).
- **Upstream contract:** no shared cap on `sendCDP` (`window-layout/index.ts:3371-3387`; `sandbox/index.ts:336-375`).
- **Trigger:** agent A runs `Promise.all` over 80 `API.sendCDP` calls. During the burst, A's extra calls and agent B's direct tool calls fail with "too many pending browser requests".
- **Regression idea:** fill 64 requests for agent "A"; a request for agent "B" is still accepted (per-agent quota).

---

## Checked, no defect found
- **Request-ID collisions across agents through the shared `completed` signal:** direct IDs are UUIDs (`src/core/AgentEngine.cpp:901-904`). Sandbox IDs are `sandbox-<context UUID>-<seq>` (`worker_main.cpp:677`; `SandboxHost.cpp:135`), and listener IDs are likewise UUID-scoped (`worker_main.cpp:290`).
- **Cancellation:**
  - Engine cancel removes the ID before cancelling the bridge request (`AgentToolHost.cpp:92-96`).
  - Sandbox broker cancellation works (`:232-244`).
  - The host destructor cancels pending requests and ends subscriptions (`:281-303`).
  - Closing a tab settles its pending requests and subscriptions (`BrowserToolBridge.cpp:109-123`).
  - Timeouts settle once.
  - Tests: `tests/test_browser_tools.cpp:264-325`.
- **Ownership checks:**
  - The tab is checked at execution (`BrowserToolBridge.cpp:256`) and before subscription (`AgentToolHost.cpp:257`).
  - Mini-app owners are set before registration (`MiniAppHost.cpp:73`, then `MainWindowApps.cpp:53`).
  - A foreign active tab is hidden (`BrowserToolBridge.cpp:193,217`), matching `browser-domain-adapter.ts:74-82`.
  - Tests: `tests/test_sandbox_apps.cpp:120-124`, `tests/test_browser_context*.cpp`.
- **Inspector:** resets on navigation and verifies the URL is unchanged before emitting a capture (`BrowserInspector.cpp:54-57,291-295`).

## Intentional native policies (not defects by themselves)
- Direct `getBrowserTabs`/`sendCDP` tools exist alongside the sandbox (`BrowserToolBridge.h:17-18`). Upstream is sandbox-only. B1 is about the uncapped output and the screenshot guidance, not the existence of the tools.
- Bounded resources: 2 MiB per response, 64 pending requests, 20 s per request (`BrowserToolBridge.h:80`; `setRequestTimeoutMs` is unused outside tests).
- Console logs are cleared on every load start (`BrowserToolBridge.cpp:89-91`). This follows the upstream tool's stated contract (`read-console-logs.ts:14`); upstream's implementation only clears on an explicit API call (`browsing-tab-controller.ts:3021`).
- `NewWindowRequested` navigates the same tab (`browserwidget.cpp:748-757`) instead of opening a scoped new tab (upstream `window-layout/index.ts:1550-1556`). Consequence: a page or agent `window.open` replaces the current page.

## Suspicions (untested)
- **S1:** `sendCDP Target.createTarget` is passed raw to WebView2's page session, with no interception anywhere in `src`. Upstream intercepts it to create an owned tab (`sandbox/index.ts:343-357`). On WebView2 it may fail, or create an unmanaged target.
- **S2:** hidden but initialized tabs (`IsVisible=FALSE`, `browserwidget.cpp:1086-1091`) may throttle timers or painting, so `awaitPromise` or `Page.captureScreenshot` stall until the 20 s timeout.
- **S3:** the 20 s per-request timeout is shorter than upstream's roughly 180 s sandbox budget (`sandbox/index.ts:303`), so long `Runtime.evaluate awaitPromise` waits fail natively.
- **S4:** tab IDs restart at `t_1` each launch, as upstream's do (`window-layout/tab-id.ts`). Upstream also emits a `browser-restarted` change entry (`browser-domain-adapter.ts:121-126`); native only re-renders the current snapshot, so old IDs in history may silently name different tabs.
- **S5:** the native browser context omits upstream's `consoleErrors`/`consoleLogs`/`error` tab attributes and tab opened/navigated/closed deltas (`browser-domain-adapter.ts:94-115,117+`).

## Missing source evidence
- No `audit/` inventory in this snapshot, so tracking status of B2 and B3 is unknown.
- Runtime WebView2 behaviour is unverified: whether CDP domain enablement survives renderer swaps, whether the Target domain is available on the page session, and whether a never-shown controller can start.
- The upstream full-page "attach screenshot" UI path was not fully traced; only element region capture was confirmed.
