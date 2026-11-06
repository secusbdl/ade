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

- [`audit/reference-readiness.json`](audit/reference-readiness.json) maps 20
  important capability groups to native code, tests, source contracts and gaps.
- [`audit/upstream-feature-inventory.json`](audit/upstream-feature-inventory.json)
  preserves the complete 701-row scope. Its original status fields are the initial
  inventory, not current coverage percentages.
- [`reference/stagewise/REFERENCE_MANIFEST.json`](reference/stagewise/REFERENCE_MANIFEST.json)
  indexes the curated upstream source, with original Git blob IDs and SHA256 hashes.
  It is an offline code/contract reference. Excluded media and icon packages mean
  this snapshot is not a standalone buildable copy of Stagewise.
- [`reference/native-experiments`](reference/native-experiments) preserves native
  mutation-journal and worktree-usage prototypes with tests and integration limits.
  These modules are not enabled in the application.
- [`audit/reference-integration.json`](audit/reference-integration.json) records
  this reference checkpoint's build, test and deployment evidence.

## Native components to reuse

| Area | Working native foundation | Source |
| --- | --- | --- |
| Agent engine | Four provider wire protocols, streamed text/reasoning/tools, cancellation, fallback, utility models | `src/core` |
| Transcript | Ordered stable rows, incremental rendering, disclosure state, inline approval, per-chat scroll state | `src/ui/ConversationView*`, `src/core/AgentPresentation*` |
| Transcript links | Conversation-scoped file, attachment and browser links with bounded reads and provenance checks | `src/links`, `src/ui/MainWindowLinks.cpp` |
| Conversations | Atomic persistence, drafts, attachments, fork, archive/restore, restart selection, durable deletion cleanup | `src/ui/SessionStore*`, `src/ui/MainWindowConversations.cpp` |
| Files and context | Scoped reads/search/write/edit, retained original read snapshots, later-round context reinjection | `src/core/ToolExecutor*`, `src/attachments`, `src/runtime` |
| Tools and apps | Native QuickJS worker, correlated host calls, persistent sandbox state, agent-authored app previews | `src/sandbox`, `src/runtime`, `src/apps` |
| Browser | WebView2 tabs, JavaScript/CDP bridge, permissions, downloads and history | `src/browser` |
| Editor and terminal | Native text editor, encoding/EOL preservation, conflict-aware saving, ConPTY/libvterm terminal | `src/editor`, `src/terminal` |
| Git and worktrees | Branch/worktree actions, repository watches, setup settings/execution and active-use guards | `src/git` |
| Navigation/settings | Grouping, persistent pins, workspace/content controls, appearance, provider and plugin configuration | `src/ui` |
| Network/integrations | Credential redaction, per-host authorization and packaged integration instructions | `src/network`, `src/plugins`, `assets/plugins` |

CMake exports reusable static targets including `ade_core`, `ade_attachments`,
`ade_file_transformer`, `ade_transcript_links`, `ade_network`, `ade_plugins`, `ade_host`, `ade_editor`,
`ade_terminal`, `ade_git_ui`, and `ade_ui`. The application and its UI tests share
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

```powershell
.\build\bin\ADE.exe --data-dir .\private\profile --workspace .\private\workspace
```

Deploy `ADE.exe`, `ADE-sandbox-worker.exe`, and `WebView2Loader.dll` together,
along with the corresponding licenses/notices. Qt, QuickJS and compiler runtimes
are static. Browser pages require an installed Microsoft WebView2 Evergreen
Runtime. Git/worktree actions use installed Git; terminals and setup scripts use
the selected system shell. There is no Electron or Node sidecar. This is not a
single dependency-free executable.

Configure API keys in Settings. Keys are protected with Windows DPAPI in the
selected private profile. Never commit profiles, imported key files, or live
conversation exports. Automated tests use local fixtures; optional `--smoke`
provider validation must use disposable workspaces and ignored report paths.

## Evidence and remaining work

Tests establish the cases they exercise. The audit directory retains dated
before/after failures, local protocol fixtures and limited live-service evidence.
The paired read-context check used the same configured DeepSeek model in both
apps, forked and restarted chats, removed the original generated probe and
verified retained values. See `audit/paired-read-context-native.json`; this does
not establish identical model output or full application parity.

Important remaining work includes queued/side chats and durable undo, multiple
workspace and other filesystem mounts, complete shell-session semantics, rich
PDF/Office/media transformation, selected-element browser tools, advanced
editor/LSP/diff behavior, every provider/subscription authentication flow,
plugin installation/OAuth, hosted accounts/billing, ACP/research integrations,
complete browser/session restoration, persisted transcript-link provenance, updater/installer and full visual parity.
The readiness map gives the implementation boundary for each area. Preserving
an upstream implementation does not mark its native counterpart complete.

Historical audit screenshots, including the original app and desktop, are kept
in the local development archive; audit raster images are omitted from the publication tree. Their
references in older audit files describe historical local evidence.

This is an independent project, not managed, released, endorsed, sponsored, or
approved by Stagewise GmbH. The ADE project is supplied under AGPL-3.0; retained
upstream packages also carry their own notices. Preserve [`LICENSE`](LICENSE), [`NOTICE`](NOTICE), and component
licenses. Restricted Nucleo assets and Stagewise product branding are not bundled
in ADE's interface.
