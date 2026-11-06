# Stagewise source baseline and parity audit

Pinned baseline: **Stagewise 1.31.1**, commit `17a0123a0e896885465df36169457199d5225eb5` (release commit dated 2026-11-04). Checkout: `D:\age\ADE\upstream\stagewise`. The installed application reports the same product/file version and was already running from `C:\Users\secus\AppData\Local\stagewise\app-1.31.1\stagewise.exe` when inspected on 2026-11-05.

The requested implementation is native C++/Qt UI and backend, with Qt 6 MinGW static requested. Electron, React and a Node backend sidecar are not the final architecture. This inventory records the entire requested parity target; it does not reduce the target to the initial implementation.

## Inventory artifacts

- `upstream-feature-inventory.json`: 701 inventory rows, 219 schema-field candidates, and a hashed manifest of all 1,698 tracked files in the declared application/runtime/design-system scopes. The manifest contains 156 upstream test files. Rows overlap deliberately: behavior groups, declared API contracts and implementation registrations are different evidence for the same product behavior; **701 is not a count of independent verified features**.
- `provider-endpoints.json`: non-secret provider destinations, protocol types, upstream validation probes, subscription-plan endpoints and 54 catalog model IDs. A validation probe model is not a default chat model. Current model availability and the user's key product/region remain live-verification concerns.
- `upstream-baseline.json`: version, SHA, installed executable and initial verification status.
- `inventory_upstream.py`: repeatable extraction from the pinned checkout. Regenerating resets this baseline inventory's native statuses; preserve subsequent execution evidence separately.

| Surface | Enumerated evidence |
|---|---:|
| UI/backend contract operations | 236 |
| Static registered backend procedures | 203 |
| Core agent commands | 33 |
| Keyboard actions and platform bindings | 71 |
| Registered provider types | 26 |
| Browser-host tool names | 18 |
| Settings destinations | 14 |
| Screenshot-specific settings requirements | 19 |
| Bundled plugin metadata records | 8 |
| Internal page routes | 4 |

Every row begins `native_status: not_started`, `parity_status: not_verified`, `upstream_runtime_status: not_run`. These are baseline audit statuses, not a live view of concurrent native development. No upstream dependencies were installed and no provider request was made by the inventory task. No credential file was read.

## Exact source boundaries

The manifest includes all `apps/browser/` files (including bundled plugins, resources and build definitions), `packages/agent-core/src/`, `packages/agent-shell/src/`, `agent/runtime-node/src/`, `packages/karton/src/`, `packages/stage-ui/src/`, `packages/icons/src/`, and `apps/update-server/`. Each manifest entry records path, SHA-256, size and source/test/story classification. Root `AGENTS.md` was read; the upstream checkout remains unmodified.

The desktop implementation reference is `apps/browser`; extracted agent lifecycle/state/tools/persistence are in `packages/agent-core`; PTY sessions, watchers and shell approvals are in `packages/agent-shell`; filesystem/glob/grep/watch runtime is in `agent/runtime-node`. `apps/stagewise-cli` is a minimal smoke-test host and does not establish browser or full desktop parity. The marketing website and deprecated CLI are not desktop UI requirements. Hosted account, research, update and third-party plugin services are separate integration dependencies, not services recreated automatically by copying this source.

The extraction covers source surfaces, not every semantic behavior. Dynamic registrations, nested schema ownership, asynchronous races and platform-specific behavior still require manual review and runtime fixtures. Passing an inventory structural check does not prove feature parity.

## Settings reference requirements

The navigation order is sourced from `apps/browser/src/ui/screens/settings/settings-route.tsx`:

- **Agent:** General; Models & Providers; Skills & Context files; Worktrees; Plugins; Archived chats.
- **Personal:** Personalization.
- **Browsing:** General; Clear data; History.
- **Account** and **About**.

Custom Providers and Website Permissions are subordinate routes. Models & Providers includes provider instances and model enablement, utility title-generation/context-compression ordered fallbacks, and presets with main/fallback model order and per-model thinking overrides. A single API-key text box does not satisfy that screen.

The user's Figma, GitHub, PostHog, Supabase and Vercel cards correspond to bundled plugin metadata. Their credential types are respectively `figma-pat`, `github-pat`, `posthog-pat`, `supabase-pat`, `vercel-pat`. Required destination origins are defined in `apps/browser/src/shared/credential-types/`; credential values must remain scoped to those origins. A saved enable toggle is configuration coverage only until the integration's permitted live operation is verified.

Browsing General includes search engine creation/default/removal, new-tab and startup page selection, permissions defaults and host exceptions. Agent General includes keep-awake policy and notification sound loudness/pack/preview/import. Personalization covers UI scale, appearance and color scheme. Account covers the actual Google/GitHub/email and subscription/BYOK flows, subject to upstream hosted service availability. About covers version/update/license/third-party/app-data actions.

## Comparison sessions

1. Use the already installed 1.31.1 application as the release reference. Do not start a second instance expecting an isolated profile: the release code sets its user-data path explicitly and takes a single-instance lock (`apps/browser/src/backend/index.ts`). A generic Chromium `--user-data-dir` flag is not evidence of isolation here.
2. If a source reference build is needed, first coordinate dependency installation. Upstream requires Node >=22.12.0 and pnpm 10.30.3. From the pinned root, the documented command is `pnpm -F stagewise start:isolated`; it sets `STAGEWISE_DEV_INSTANCE=auto`. The build derives a worktree hash for a distinct `stagewise-dev-<hash>` profile (`apps/browser/build-constants.ts`). On first run, this can seed selected settings and credentials from the default **development** profile (`seed-isolated-dev-profile.ts`), so isolation is not synonymous with an empty credential store.
3. Create separate disposable, identically initialized workspaces for reference and native sessions. Never use the source checkout or the user's real project as the destructive tool/Git fixture. Match model ID, provider instance, reasoning settings, tool permissions, prompt, workspace content and browser viewport.
4. Start with deterministic file tasks: read/search, bounded edit, new file, undo/retry, shell tests, restart/resume. Record the prompt, resulting diff, command exit codes, tool events, final file hashes and error/cancel outcomes. Compare outcomes and protocol behavior; model wording is nondeterministic.
5. Exercise browser parity with controlled local pages: navigation/history, nested frames, DOM selection, console errors, permission requests, screenshots and device sizes. Compare screenshot layouts at equal DPI/window size. Browser/control completion must not be claimed solely from a URL being displayed.
6. Exercise providers independently. Only send each credential to its verified provider endpoint. The labels PORTKEY, YOU, SAKANA, THEROUTER, PERPLEXITY and NVIDIA have no direct registered endpoint in this upstream baseline and remain unconfigured in the metadata. QWEN and ZAI ordinary API and subscription endpoints differ. Record HTTP/model availability without logging secrets.
7. Exercise Git/worktree features in fixture repositories, then credentials/plugins/account/update features with the corresponding services. A successful provider completion does not verify tool loops, browser integration, terminal behavior or any other provider.

No native/upstream comparison session has been passed by this audit task. Record each executed comparison in a separate result file, with date, native build identifier, upstream SHA/version, feature IDs, fixture and observed outcome. Keep unresolved items visible until both implementation and comparison evidence exist.
