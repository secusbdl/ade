# ADE reference implementation

ADE preserves Stagewise's important IDE and agent workflows as a native C++/Qt
implementation and a source reference for a future ADE application. The reference
is pinned to **Stagewise 1.31.1**, commit
`17a0123a0e896885465df36169457199d5225eb5`.

**This is a reference project, not a production release or a claim of full parity.**
Native implementations, experimental modules, upstream source and unresolved gaps
are recorded separately. Matching the provider and prompt does not guarantee
identical model reasoning or output.

## Start here

- [`audit/upstream-source-index.json`](audit/upstream-source-index.json) inventories
  **all 2,139 tracked files** at the pin. The companion
  [`audit/upstream-function-index.jsonl`](audit/upstream-function-index.jsonl)
  locates **17,331 function bodies** in 1,519 TS/JS files by exact source spans
  and hashes, with no parser errors. These include tests, callbacks and tooling;
  they are not 17,331 independent features or completed ports. Static indexing
  does not establish semantic review or native parity for any line.
- `audit/parity-review-core.json`, `audit/parity-review-ui.json` and
  `audit/parity-review-integrations.json` contain the current source review with
  upstream/native/test anchors and explicit unresolved behavior. Historical
  test evidence in those reviews is identified separately from new test runs.
- [`audit/parity-review-coverage.json`](audit/parity-review-coverage.json) joins
  those reviews and the complete 71-action shortcut review onto all 701 feature
  rows. Rows without individual reassessment remain explicit. These are review
  references, not completion percentages or proof of a line-by-line port.
- [`audit/reference-readiness.json`](audit/reference-readiness.json) maps 20
  important capability groups to native code, tests, source contracts and gaps.
- [`audit/upstream-feature-inventory.json`](audit/upstream-feature-inventory.json)
  preserves the complete 701-row scope. Its original status fields are the initial
  inventory, not current coverage percentages.
- [`reference/stagewise/REFERENCE_MANIFEST.json`](reference/stagewise/REFERENCE_MANIFEST.json)
  indexes the curated upstream source, with original Git blob IDs and SHA256 hashes.
  It is an offline code/contract reference. Excluded media and icon packages mean
  this snapshot is not a standalone buildable copy of Stagewise.
- [`reference/stagewise-supplement/REFERENCE_MANIFEST.json`](reference/stagewise-supplement/REFERENCE_MANIFEST.json)
  additionally preserves the public update-server and desktop build/storybook
  tooling as exact blobs. A native updater remains a separate implementation
  gap; the public update-server source is available at the pinned commit.
- [`reference/native-experiments`](reference/native-experiments) preserves native
  mutation-journal and worktree-usage prototypes with tests and integration limits.
  The preserved prototypes stay unlinked. Production mutation/undo now lives in
  `src/mutations`; production worktree usage and reviewed cleanup now live in
  `src/git`. The original experimental copies remain unchanged and unlinked.
- [`audit/feature-progress-update.json`](audit/feature-progress-update.json) maps
  the earlier continuation additions onto the same 701-row scope without changing the baseline.
- [`audit/plugin-integration.json`](audit/plugin-integration.json) records the
  latest token-plugin, Settings/import and packaged-control verification.
- [`audit/continuation-integration.json`](audit/continuation-integration.json) is
  the preceding checkpoint report: **80/80 CTest suites passed in one final run**.
  Its one offscreen-only skipped case passed in the separate Windows suite.
  [`audit/reference-integration.json`](audit/reference-integration.json) and
  [`audit/source-publication.json`](audit/source-publication.json) describe the
  earlier verified and published checkpoint.
- [`audit/parity-integration.json`](audit/parity-integration.json) records the
  preceding 91-suite checkpoint, package hashes and remaining limits.
  [`audit/parity-next-integration.json`](audit/parity-next-integration.json) records
  the subsequent provider-picker, coding-agent, changed-files and transcript work,
  including failures found during integration and the final verified scope.

- [`audit/parity-followup-integration.json`](audit/parity-followup-integration.json)
  records the subsequent **124-suite checkpoint**: OpenCode ACP, provider removal
  and model visibility, source-derived prompt/context updates, MRU agent cycling,
  grouped assistant replies and image lifecycle repairs. Exact package and GUI
  evidence are identified separately from earlier live-model runs.

- [`audit/parity-harness-integration.json`](audit/parity-harness-integration.json)
  records the **144-suite checkpoint**: native Claude Code, OpenCode question MCP,
  source-derived ACP presentation, durable model inventory and fresh browser-tab
  context. Earlier-candidate live OpenCode question evidence and exact final-package
  checks identify their binaries separately.

- [`audit/parity-harness-followups-integration.json`](audit/parity-harness-followups-integration.json)
  records the **154-suite checkpoint**: canonical coding-agent session continuity,
  late ACP tool settlement, utility model fallback, narrow Claude resume recovery,
  question-wrapper presentation and durable external file attachments. Exact
  packaged UI and live smoke evidence are recorded with their binary identities.

- [`audit/parity-native-questions-integration.json`](audit/parity-native-questions-integration.json)
  records the **161-suite checkpoint**: compact native question cards, shared
  question MCP for direct Codex/Claude sessions, historical attachment handling,
  ACP MIME hints, selected-model label display and deferred Codex tool discovery.
  Exact candidate bytes passed native Windows shutdown and a real Codex form flow.

- [`audit/parity-custom-images-integration.json`](audit/parity-custom-images-integration.json)
  records the **173-suite checkpoint**: custom HTTP model definitions and typed
  options, encrypted private settings, stale queued-selection recovery, and
  original ACP images with bounded process writes. Exact candidate bytes passed
  three real API/CLI smokes and stdio MCP. Direct Windows UI inspection remains
  pending after the user stopped Computer Use; earlier control evidence is
  identified as a different binary.

- [`audit/parity-behavior-integration.json`](audit/parity-behavior-integration.json)
  records the **186-suite checkpoint**: Settings layout, watched workspace tree,
  model-aware context budgets, plugin ownership, transcript scrolling, Git state,
  Claude final-message reconciliation and browser history/navigation. The exact
  packaged executable passed live DeepSeek and Codex checks plus direct Windows
  Send, Settings, import-chooser, plugin-control and close checks. Two test cases
  have qualified skips. Invalid-store recovery and two display defects found in
  GUI testing remain open; the report identifies them explicitly.

## Native components to reuse

| Area | Working native foundation | Source |
| --- | --- | --- |
| Agent engine | Four provider wire protocols, streaming, cancellation, fallback, durable message queue and typed multi-step questions | `src/core` |
| Coding agents | Installed Codex app-server, OpenCode ACP and Claude Code stream-json; CLI-managed accounts, model discovery, streamed tools/approvals/questions and isolated fallback conversation files | `src/harness`, `src/core/AgentEngineHarness.cpp`, `src/runtime/HarnessFileContext*` |
| Transcript | Consecutive assistant grouping with raw provenance, incremental Markdown/code/tool cards, inline approval, document-owned trusted images, bounded file diffs and per-chat scroll state | `src/ui/ConversationView*`, `src/core/AgentPresentation*` |
| Transcript links | Conversation-scoped file, attachment and browser links; persisted/forked provenance remains checked against current mount identity | `src/links`, `src/ui/MainWindowLinks.cpp` |
| Conversations | Atomic persistence, drafts, fork, archive/restore, restart selection, durable deletion cleanup and isolated side-chat promotion/discard | `src/ui/SessionStore*`, `src/ui/MainWindowConversations.cpp`, `src/ui/MainWindowSideChats.cpp` |
| Files and context | Scoped reads/search, immutable read snapshots and reinjection; journaled write/edit/copy/move/delete and conflict-aware undo/recovery | `src/core/ToolExecutor*`, `src/mutations`, `src/attachments`, `src/runtime` |
| Tools and apps | Native QuickJS worker, correlated host calls, persistent sandbox state, agent-authored app previews | `src/sandbox`, `src/runtime`, `src/apps` |
| Context utilities | Four-protocol usage snapshots, automatic compression with a known model limit, manual `/compact`, configurable utility models and retained original history | `src/core/ProviderUsage*`, `src/core/AgentEngineCompression.cpp`, `src/core/UtilityService*` |
| Browser | WebView2 tabs, durable ordinary-tab restoration, CDP, selected-element and screenshot context, device preview, permissions, downloads and history | `src/browser`, `src/ui/MainWindowBrowser*` |
| Editor and terminal | Native text editor, encoding/EOL preservation, conflict-aware saving, ConPTY/libvterm terminal | `src/editor`, `src/terminal` |
| Agent shells | Persistent PowerShell sessions, commands/stdin/polls, process-local watchers, per-chat logs and readonly `shells/` mount | `src/shell`, `src/runtime/AgentToolHost*` |
| Git and worktrees | Branch/worktree actions, repository watches, setup settings/execution, durable usage tracking and explicitly reviewed cleanup with fresh identity/status checks | `src/git` |
| Changed files | Repository status tree and current Git diff tabs; per-turn native file snapshots, conflict-aware undo and durable mutation settlement | `src/ui/WorkspaceChanges*`, `src/ui/GitDiffTab*`, `src/ui/TurnFileEdits*`, `src/mutations` |
| Navigation/settings | Grouping/pins, command center and file search, rich slash/@ completion, direct tab/agent/focus shortcuts, MRU agent preview/commit and provider/plugin configuration | `src/ui` |
| Network/integrations | Credential redaction, per-host authorization and packaged integration instructions | `src/network`, `src/plugins`, `assets/plugins` |

On Windows, the Git/worktree service passes `-c core.longpaths=true` per command
so tracked paths can exceed 260 characters without changing repository or global
Git configuration. Git still enforces a separate explicit `GIT_DIR` root-length
limit; this does not remove every path constraint. See the
[worktree validation evidence](audit/native-git-worktree-live-verification.json).

CMake exports reusable static targets including `ade_core`, `ade_attachments`,
`ade_file_transformer`, `ade_transcript_links`, `ade_network`, `ade_plugins`, `ade_host`, `ade_editor`,
`ade_terminal`, `ade_shell`, `ade_git_ui`, `ade_harness`, and `ade_ui`. The application and its UI tests share
`ade_ui`, so they exercise the same compiled implementation. These are internal
components, not a versioned SDK. Preserve their cancellation, ownership, mount
validation and persistence boundaries when reusing them.

## Build and test

The verified toolchain is Windows x64, **Qt 6.11.2 static**, **GCC 16.2**, CMake and
Ninja, plus static OpenSSL. Qt modules used by the build are declared in
`CMakeLists.txt`; dynamic Qt and non-MinGW builds are rejected. A clean-machine
toolchain installation is not automated.

```powershell
.\scripts\build.ps1 -Test `
  -QtPrefix 'C:\Qt\6.11.2-mingw64-static' `
  -ToolchainDirectory 'C:\mingw64\bin' `
  -OpenSslRoot 'C:\Qt\openssl-3.5.8-mingw64-static' `
  -BuildDirectory build -Jobs 3
```

All three dependency paths are configurable. You can instead set
`ADE_QT_PREFIX`, `ADE_TOOLCHAIN_DIRECTORY` (the directory containing the tools),
and `ADE_OPENSSL_ROOT`; the `mingw-static` CMake preset uses these variables.
The PowerShell script retains compatibility defaults for the original machine,
supports `-ConfigureOnly`, and restores PATH after execution. Test reports are
written to `<build directory>/Testing/native_*.txt`.

Add `-Experiments` to build and test the isolated mutation-journal and worktree
usage prototypes. Their `reference_*` tests remain separate from application
features; enabling them never links their libraries into ADE. Verify preserved
source bytes with `python scripts/verify-reference.py` (Python 3.9 or newer).

To reproduce the function audit, install the pinned TypeScript parser as a
development tool and run:

```powershell
npm install --prefix private/parity-tools --no-save --ignore-scripts typescript@5.9.3
node scripts/index-upstream-functions.cjs --self-test
node scripts/index-upstream-functions.cjs --verify
```

The index reads Git blobs from `upstream/stagewise` at the pinned commit, including
files omitted from the curated reference. It never executes upstream code.
Neither ADE nor its native sandbox worker depends on this Node-based audit tool.
`python scripts/preserve-reference-supplement.py` reproduces the supplement and
refuses to overwrite modified preserved files.

```powershell
.\build\bin\ADE.exe --data-dir .\private\profile --workspace .\private\workspace
```

Deploy `ADE.exe`, `ADE-sandbox-worker.exe`, and `WebView2Loader.dll` together,
along with the corresponding licenses/notices. Qt, QuickJS and compiler runtimes
are static. Browser pages require an installed Microsoft WebView2 Evergreen
Runtime. Git/worktree actions use installed Git; terminals and setup scripts use
the selected system shell. Persistent agent shell sessions currently use PowerShell
and have the current user's OS permissions; mounted file checks do not sandbox
shell commands. There is no Electron or Node sidecar. This is not a
single dependency-free executable.

Configure API keys in Settings. Keys are protected with Windows DPAPI in the
selected private profile. Never commit profiles, imported key files, or live
conversation exports. Automated tests use local fixtures; optional `--smoke`
provider validation must use disposable workspaces and ignored report paths.

## Configure providers and plugins

Use the labeled **Settings** button or **Ctrl+,**. In **Models & Providers**,
**Import API key file…** reads dotenv-style entries such as
`PROVIDER_API_KEY=value`; **Add Provider** opens a searchable provider picker with
API, coding-plan, local Ollama, custom-endpoint and Codex choices. Selecting a
template fills its endpoint and protocol; it does not inherit another provider's
key or model. A configured card is not a claim that its credentials were verified.
Imports report their result on the Settings page and refresh the model selector.
Providers without a known endpoint or model require configuration before use.

The **Codex** coding-agent choice uses an installed native `codex.exe` and its
existing sign-in, or opens its supported sign-in flow. Subscription credentials
remain managed by that CLI. ADE uses the app-server protocol for models, sessions,
streamed messages, tools and approvals. Inherited Codex desktop launch identity is
removed from the child environment while retaining the CLI's auth-home location.
Its own tools run in the connected workspace or a private per-conversation fallback
directory. Fork/delete operations copy or remove only ADE-owned fallback files;
they do not copy or delete the connected workspace. Native ADE mutation journals
do not yet capture every filesystem change made by an external coding agent.
Other coding-agent adapters remain separate gaps unless an integration report
explicitly records their implemented and tested scope.

The **Claude Code** choice uses its installed CLI and native stream-json control
protocol. **Fetch Claude models** performs initialization only; it does not verify
account entitlement or spend a model request. Questions, approvals, cancellation
and streamed tool state use correlated native protocol handling.

OpenCode and direct Codex/Claude sessions can call ADE's
**stagewise_request_user_input** MCP tool. The same ADE executable has an early
stdio-helper mode; it opens the owner chat's native form and returns completed
steps, answers and cancellation status. Human-wait deadlines pause independently
from command approvals. Canonical wrapper records remain stored while the UI
shows one compact question card. Declared password answers are masked in that
card; independently authored assistant text is unchanged.

The real OpenCode/DeepSeek GUI smoke is recorded in
`audit/harness-candidate-gui-native.json`. The 161-suite checkpoint adds a real
managed Codex native-form round trip using the same selected model before and
after a deferred-tool discovery repair. Direct Claude has actual-host fixture
coverage; paid Claude inference is not established by that fixture.

External coding-agent attachments now keep owner-scoped copies of captured
original bytes. OpenCode receives native ACP file links; direct Codex and Claude
receive real local-path descriptors. Existing runtime permissions still apply.
Original files can be moved or changed after capture without changing the stored
input. Forks get independent copies, deletion includes owned cleanup, and context
replay excludes removed or pre-summary references. Inline images retain the
native size/encoding limits described in the checkpoint.

Coding-agent resumes now require a binding to the immediately preceding completed
canonical history. Provider switches, edits and interrupted legacy history use a
fresh session with bounded replay. Utility tasks can use configured HTTP models
even when the main conversation uses a coding-agent CLI; fallback order and exact
provider identity are retained.

When a retained historical attachment is provably absent from both its cache and
original location, replay omits it with a bounded logical-label notice. Invalid
captured bytes and missing current attachments remain errors. ACP captured-file
links carry optional extension-derived image MIME hints. ACP now sends exact
captured originals for supported images up to20MiB when the actual coding agent
advertises image input. Oversized/unsupported images and images that exceed the
64MiB aggregate frame limit remain captured resource links. Preparation is
asynchronous; process writes retain attachment grants until Qt drains the frame.
This policy is specific to ACP, and cannot retract bytes already sent.

Custom HTTP model definitions now support exact-provider add/edit/move/rename/
delete, context metadata, supported text/image and tool capabilities, and
advanced JSON options/headers. Private configuration uses Windows encryption;
custom-containing files use format2 so known old readers refuse them instead of
silently erasing definitions. Ordinary format1 configurations remain supported.

The four HTTP adapters translate a documented subset of provider options while
protecting native history/tool/system/stream construction. Unsupported fields
fail explicitly. Active replies keep their original configuration; stale queued
custom selections pause until explicitly resumed with current settings. The
smoke command uses the same selection resolver. Full SDK-option coverage and
context-derived read budgets remain separate gaps in this checkpoint.

Discovered models and visibility choices now save atomically with their provider.
Eligible pinned provider templates apply source-derived defaults on first discovery;
manual enable/disable choices survive refresh and restart. A complete inventory
with no enabled models can be saved, then enabled through **Models**. Endpoint or
protocol changes invalidate stale inventory; incomplete pagination preserves the
previous choices and reports an error. Custom-model CRUD and advanced options
remain separate gaps.

The provider editor's optional **Model context limit** applies to that
provider's default model. Changing its model, endpoint or protocol clears the
limit; another discovered model must supply its own matching catalog limit.
Zero means unknown and keeps compression manual. Automatic compression uses
the last completed provider usage snapshot, pauses at a completed tool-batch
boundary, and retains original saved messages. Usage is not a billing estimate;
the exact upstream tokenizer, asynchronous compaction and UI remain separate gaps.

In **Plugins**, open a card, save its token, then use **Check saved token** for
an explicit read-only connection check. Saving a token never starts a request.
The check validates one endpoint; it does not establish every permission needed
for that plugin. Supabase requires a **Management API personal access token**,
not a project's anon/publishable/service-role key. PostHog uses a personal API
key and the US/EU region selected for the check. Figma, GitHub and Vercel use their
personal access tokens. Each screen links to token setup help.

Tokens are encrypted in the separate plugin credential store and remain masked.
Disabling a plugin retains its token but removes access from agent skills;
replacing/removing a token revokes existing worker handles and pending requests
when configuration reloads. The native host inserts tokens only for the pinned
service origins. Agent scripts use `API.getCredential` and global `fetch` with
opaque handles, as the bundled Stagewise skills specify. GitHub API access is
separate from the system Git authentication used for local repository operations.

The five bundled integrations are GitHub, Supabase, Figma, PostHog and Vercel.
Their actual worker flows are tested with synthetic tokens and exact service
URLs through a simulated transport. This is not a claim that a user's live
account permissions or every remote API operation have been tested.

`--smoke --data-dir <profile>` uses that profile's configured provider, encrypted
`plugin-credentials.json` and `preferences.json`. It honors plugin enable/disable
settings and loads the same bundled skill roster, `readContext` access and invoked
skill context as a normal chat. Configure the profile in Settings first; there is
no plugin-token command-line argument. Invalid plugin storage or malformed
preferences stop the run before contacting the model.

Smoke runs use the real native agent, QuickJS worker and network broker without
opening the main window. Use `--provider <id>`, `--prompt`, `--timeout` and
`--report` to bound and record a run. Approval-gated tools are denied unless
`--always-allow` is explicitly supplied. Interactive questions, chat UI and
mini-app tabs are not exercised; settings are loaded at startup. Reports contain
conversation and tool output and should remain private. The subprocess regression
uses a loopback model and synthetic credential-state checks, with no external
plugin request.

## Evidence and remaining work

Native HTTP requests now include fresh registered browser-tab metadata owned by
the current chat or shared globally, with explicit active selection and MRU order.
This is current host context, not DOM contents or user instructions. Historical
environment snapshots/deltas and automatic external-harness browser metadata
remain separate gaps.

The renewed source review adds ordinary browser restart restoration, model usage
and guarded automatic compression, explicit worktree cleanup review, and 26
direct navigation actions. See `audit/browser-session-native.json`,
`audit/usage-compression-native.json`, `audit/parity-review-shortcuts.json` and
the current integration checkpoint. Native keyboard tests cover draft retention,
filtered sidebar order, tab index/wrap behavior, address/model focus, narrow
layouts, terminal focus and deletion during focus callbacks. Ctrl+Tab's preview
switcher is still absent; the immediate Ctrl+PageDown/PageUp aliases are present.

**Source control → Review unused worktrees** lists eligibility before any action.
Only clean, unmounted, merged managed worktrees with trustworthy activity older
than seven days can be selected. Removal rechecks the physical instance,
branch/HEAD, status, active use and review revision; branches are retained.
Imported or damaged usage history stays protected, and nothing is removed
automatically. Ordinary browser restoration covers HTTP/HTTPS/about:blank tabs
with saved order and selection; per-agent associations and editor/terminal/app
restoration remain unported.

Older ADE profiles that omit the optional conversation cleanup field now load
without resetting history. The regression covers typing and sending with the
button or Enter, receiving a local streamed reply, and reopening the saved chat.
Invalid cleanup records still refuse writes. See
`audit/composer-legacy-storage-fix.json` and `audit/composer-send-verification.json`.

The November 6 live GitHub check used an actual native ADE chat to read identity
and repository state, create two isolated validation branches and one marker,
open and merge [test PR #1](https://github.com/secusbdl/ade/pull/1), and remove
both test branches. Main remained unchanged throughout that test. The token
needed selected-repository **Contents: read/write** and **Pull requests:
read/write** access; a successful identity check alone did not establish write
permission. See `audit/native-github-live-verification.json` for the bounded
results and `audit/native-smoke-plugin-verification.json` for local regressions.
This does not establish live access for the other four services.

The same executable's native titlebar X was checked while idle and while a
chat sandbox worker was active. The app and its owned worker/console processes
exited in the tested cases; see `audit/native-close-process-verification.json`.

Tests establish the cases they exercise. The audit directory retains dated
before/after failures, local protocol fixtures and limited live-service evidence.
The paired read-context check used the same configured DeepSeek model in both
apps, forked and restarted chats, removed the original generated probe and
verified retained values. See `audit/paired-read-context-native.json`; this does
not establish identical model output or full application parity.

The current continuation adds durable mutation/undo, message queues and questions,
persistent agent shells/watchers, richer composer/transcript controls, browser
context capture, link provenance and side-chat/command-center UI. Their focused
reports are linked from the readiness map. The final all-target build and 80-suite
run passed on the documented Windows toolchain; the retained first run was 75/80
before the documented fixes. Publication/export verification is separate. Shell logs
survive restart, while live shell processes and watchers are not restored.
Watcher events whose queue save fails receive bounded in-memory retries; a failed
filesystem write is not crash-durable.

Important remaining work includes simultaneous workspace mounts, the remaining
sandbox Node/importModule APIs, transcript retry/edit/revert, source-equivalent
automatic context compression and token accounting,
smart shell approval and local-server discovery, complete upstream cleanup policy/UI,
rich PDF/Office/audio/video previews, every provider/subscription authentication flow,
plugin installation/OAuth, hosted accounts/billing, remaining ACP and research integrations,
complete browser/session restoration, updater/installer and full visual parity.
The journal has explicit size/scope bounds and preserves conflicting user edits;
its crash fixtures do not establish universal power-loss durability. UI-only file
diffs do not imply full upstream diff-summary or history-import compatibility.
The main chat handles `/compact` locally; side-chat panes currently send that text
as an ordinary message instead of invoking the compression action.
Preserving an upstream implementation does not mark its native counterpart complete.

Historical audit screenshots, including the original app and desktop, are kept
in the local development archive; audit raster images are omitted from the publication tree. Their
references in older audit files describe historical local evidence.

This is an independent project, not managed, released, endorsed, sponsored, or
approved by Stagewise GmbH. The ADE project is supplied under AGPL-3.0; retained
upstream packages also carry their own notices. Preserve [`LICENSE`](LICENSE), [`NOTICE`](NOTICE), and component
licenses. Restricted Nucleo assets and Stagewise product branding are not bundled
in ADE's interface.
