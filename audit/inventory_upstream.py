"""Reproducible source-surface inventory; never executes upstream code.

All native implementation states deliberately begin at not_started. Generated
source candidates are not claims of verified runtime behavior or parity.
"""
from __future__ import annotations

import collections
import hashlib
import json
from pathlib import Path
import re
import subprocess

AUDIT = Path(__file__).resolve().parent
ROOT = AUDIT.parent / "upstream" / "stagewise"
SHA = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
FILES = subprocess.check_output(["git", "ls-files"], cwd=ROOT, text=True).splitlines()
APP = "apps/browser/src/"
CORE = "packages/agent-core/src/"


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def evidence(path, needle=None, line=None):
    assert (ROOT / path).exists(), path
    if line is None:
        data = read(path) if (ROOT / path).is_file() else ""
        line = data[: data.index(needle)].count("\n") + 1 if needle and needle in data else 1
    return {"path": path, "line": line,
            "url": f"https://github.com/stagewise-io/stagewise/blob/{SHA}/{path}#L{line}"}


features = []


def add(category, key, title, source, acceptance, evidence_kind="source_confirmed", **extra):
    feature = {"id": f"{category}.{key}", "category": category, "title": title,
               "native_status": "not_started", "upstream_runtime_status": "not_run",
               "parity_status": "not_verified", "evidence_kind": evidence_kind,
               "acceptance": acceptance, "source": source, **extra}
    features.append(feature)


# Deliberate behavior groups supplement generated individual API/provider/tool rows.
GROUPS = [
    ("shell", "layout", "Main split layout, sidebar, chat and content panel", APP+"ui/screens/main/index.tsx", "Match panel geometry, resize/collapse behavior, focus, minimum sizes and persisted layout at the same DPI."),
    ("shell", "routing", "Loading, reconnect, main and onboarding screens", APP+"ui/screens/index.tsx", "Verify first launch, onboarding completion, reconnect retry/failure and restored main screen."),
    ("shell", "onboarding", "Provider, skills and theme onboarding", APP+"ui/screens/onboarding/index.tsx", "Complete every onboarding route, back/next behavior and restart persistence."),
    ("shell", "menu", "Application menus and native lifecycle", APP+"backend/services/app-menu.ts", "Compare platform menus, window actions and keyboard dispatch."),
    ("shell", "notifications", "Completion/question/error notifications and sound", APP+"backend/services/notification.ts", "Compare notification policy, focus behavior, sound selection and click routing."),
    ("shell", "power", "Agent running power-save policy", APP+"backend/services/agent-power-save-blocker.ts", "Verify power-block lifetime starts/stops with relevant agents."),
    ("shell", "command-center", "Command center, file/content search and actions", APP+"ui/screens/main/command-center/command-center.tsx", "Compare search modes, keyboard navigation, filtering, action menus and focus restoration."),
    ("shell", "tutorial", "Tutorial overlay and first-run guidance", APP+"ui/components/tutorial/tutorial-overlay.tsx", "Verify every tutorial step and completion persistence."),
    ("agents", "sidebar", "Agent grouping, pinning, ordering and attention state", APP+"ui/screens/main/sidebar/index.tsx", "Compare age/workspace groups, pinned agents, unread state, previews and context menus."),
    ("agents", "lifecycle", "Create, resume, stop, fork, side chat and archive", CORE+"services/agent-manager/agent-manager.ts", "Exercise all lifecycle transitions concurrently and across application restart."),
    ("agents", "queue", "Queued user messages and interruption", CORE+"services/agent-manager/state-mutations/queue.ts", "Send while running; reorder, delete, clear and flush queued messages without losing content."),
    ("agents", "stream", "Streaming history and tool-call state", CORE+"services/agent-manager/state-mutations/streaming.ts", "Compare text/reasoning/tool events, partial chunks, retry, cancellation and stream termination."),
    ("agents", "approvals", "Tool approval modes and per-call decisions", CORE+"services/agent-manager/state-mutations/approvals.ts", "Verify every mode, pending decisions, denial reason, cancellation and persisted approval state."),
    ("agents", "history-edit", "Retry, edit, revert and undo tool changes", CORE+"services/agent-manager/state-mutations/history.ts", "Edit/retry a prior message with and without undo; verify file and history consistency."),
    ("agents", "context", "Environment domains, prompts, memory and plans", CORE+"env/index.ts", "Compare mounted workspace, agent instructions, skills, browser/shell/log snapshots and incremental updates."),
    ("agents", "compression", "History compression and context accounting", CORE+"agents/shared/history-compression/index.ts", "Exercise threshold crossing, summary errors, tool-pair preservation and resumption."),
    ("agents", "titles", "Automatic and locked custom titles", CORE+"agents/shared/title-generation/index.ts", "Verify automatic title generation, model selection and user title preservation."),
    ("agents", "attachments", "Attachment storage, image processing and previews", CORE+"services/attachments/service.ts", "Compare images/files, file-name collision handling, persistence and provider payloads."),
    ("agents", "composer", "Rich message composer, slash commands and mentions", APP+"ui/screens/main/agent-chat/chat/_components/rich-text", "Compare text, paste, drag/drop, attachments, file/workspace/tab mentions and send shortcuts."),
    ("agents", "questions", "Multi-step user question forms", APP+"backend/services/toolbox/tools/user-interaction/ask-user-questions.ts", "Exercise all question field kinds, back/next/cancel and interrupt-with-message behavior."),
    ("providers", "routing", "Provider instance and model identity", APP+"backend/agents/model-provider.ts", "Keep providerInstanceId/modelId paired; compare discovery, thinking, overrides and utility-model fallbacks."),
    ("providers", "acp", "External agent ACP process integration", APP+"backend/agents/acp", "Verify process lifecycle, tool mapping, permissions, questions, cancellation and reconnect for each supported agent."),
    ("providers", "credentials", "Credential storage and connection lifecycle", APP+"backend/services/credentials", "Compare add/update/delete, masked display, OS encryption, missing credentials and redacted errors."),
    ("workspace", "mounts", "Multiple workspace mounts and permissions", CORE+"services/mount-manager/index.ts", "Compare mount/unmount, read-only access, path resolution, workspace identity and watcher cleanup."),
    ("workspace", "tree", "File tree, selection and file tabs", APP+"ui/screens/main/file-tree/file-tree-sidebar.tsx", "Exercise traversal, selection, search, new/rename/delete and synchronized previews."),
    ("workspace", "file-backend", "File tree service, filesystem change observation", APP+"backend/services/file-tree", "Compare file operations, external changes, excludes and large-directory responsiveness."),
    ("workspace", "search", "Filename fuzzy search and content search", CORE+"services/mount-manager/path-fuzzy-search.ts", "Compare path ranking, ignored-file switch, regex matches and result navigation."),
    ("workspace", "instructions", "AGENTS.md and skills discovery", CORE+"services/mount-manager/workspace-info/agents-md.ts", "Verify nested instructions, enabled skill directories, refresh and workspace scope."),
    ("workspace", "skills", "Global and workspace skills", APP+"backend/services/toolbox/global-skills.ts", "Compare discovery, enable/disable, context inclusion and read-only mounts."),
    ("editor", "controller", "Native code editor, save and external changes", APP+"ui/screens/main/file-tree/use-file-editor-controller.ts", "Compare syntax/diagnostics, undo/redo, dirty state, save, external edits and unsaved-close decisions."),
    ("editor", "diff", "Diff view and file-change review", APP+"ui/screens/main/file-tree/file-tree-diff-view.tsx", "Compare additions/deletions, binary/large files, staged/worktree changes and navigation."),
    ("editor", "undo", "Persistent file diff history and tool-batch undo", CORE+"services/diff-history/index.ts", "Undo sequential and directory operations safely; verify external modifications and restart restoration."),
    ("editor", "media", "Image, audio, video, PDF and font previews", APP+"ui/components/file-preview", "Compare preview selection, zoom/fullscreen, fallback, playback and corrupt-file behavior."),
    ("editor", "markdown", "Markdown editing and rendered preview", APP+"ui/components/markdown-editor", "Compare source/rendered modes, formatting, links and persisted edits."),
    ("git", "workflows", "Repository status, branches, commits and remotes", APP+"backend/services/git/index.ts", "Exercise every exposed Git command against disposable fixture repos; compare dirty/staged/untracked/conflict states."),
    ("git", "worktrees", "Create/switch/delete/clean worktrees and setup scripts", APP+"backend/services/worktree-setup-settings.ts", "Compare branch/worktree policy, setup logs/failure, dirty safeguards and agent attachment."),
    ("terminal", "interactive", "Interactive terminal tabs and persistent display state", APP+"backend/services/terminal", "Verify PTY I/O, resize, ANSI/VT behavior, keyboard, clipboard, exit and restart semantics."),
    ("terminal", "shell-engine", "Agent shell sessions, watchers and command execution", "packages/agent-shell/src/engine/session-manager.ts", "Compare persistent cwd/environment, command completion, background watchers, cancellation and cleanup."),
    ("terminal", "smart-approval", "Shell command classification and smart approval", APP+"backend/services/toolbox/tools/shell/smart-approval/index.ts", "Compare known safe/unsafe/unknown commands, explanations, policy and classification failures."),
    ("terminal", "servers", "Local server discovery and stop actions", APP+"backend/services/local-ports-scanner.ts", "Compare process ownership, listening ports, preview opening and stop behavior."),
    ("browser", "tabs", "Browser tabs, per-agent/global assignment and sessions", APP+"backend/services/window-layout/browsing-tab-controller.ts", "Compare creation/navigation/history/close/reorder/pin, profile persistence and agent visibility."),
    ("browser", "omnibox", "URL bar, search engines, suggestions and history", APP+"backend/services/omnibox-suggestions.ts", "Compare URL detection, search templates, history matches and keyboard selection."),
    ("browser", "cdp", "DevTools, console capture, screenshots and device emulation", APP+"backend/services/window-layout/browsing-tab-controller.ts", "Exercise CDP features at matched viewport/DPI and verify page-target lifecycle/reconnect."),
    ("browser", "dom", "DOM selection, overlays and selected element context", APP+"backend/services/window-layout/selected-element-tracker/index.ts", "Compare nested frames, hover/selection, bounds changes, React metadata and context inclusion."),
    ("browser", "toolbar", "Developer toolbar widgets", APP+"web-content-preload", "Verify console, DOM inspector, device/color/font/performance/accessibility tools and per-origin preferences."),
    ("browser", "permissions", "Website permissions and host exceptions", APP+"backend/services/window-layout/tab-permission-handler", "Compare request UI, allow/deny/persist, session-only grants and per-host reset."),
    ("browser", "authentication", "Browser HTTP authentication and certificates", APP+"backend/services/window-layout/tab-authentication-handler/index.ts", "Compare HTTP auth prompt/abort and certificate error handling without logging secrets."),
    ("browser", "history", "Browsing history, favicon and web data", APP+"backend/services/history", "Compare visit recording/filtering/deletion and tab/title/favicon state."),
    ("browser", "protocols", "Internal app, plans, workspace and attachment protocols", APP+"backend/services/app-protocol.ts", "Verify scoped content serving, MIME/range handling, traversal resistance and linked navigation."),
    ("browser", "sandbox", "Agent browser JavaScript execution sandbox", APP+"backend/services/sandbox", "Compare exposed operations, timeouts, isolation and browser/session resource cleanup."),
    ("integration", "plugins", "Plugins and plugin skill resources", APP+"ui/screens/settings/sections/plugins-section.tsx", "Compare discovery/install/remove/enable flows, dependencies and resource access."),
    ("integration", "devtool-api", "Developer tool API and IDE integration", APP+"backend/services/dev-tool-api", "Compare documented API requests, editor routing and scope checks."),
    ("integration", "deep-links", "Custom URI and startup URL handling", APP+"backend/services/uri-handler.ts", "Compare cold/warm launch links, authentication callbacks and invalid URI handling."),
    ("integration", "research", "Library documentation research services", APP+"backend/services/toolbox/tools/research", "Compare list/search schemas, returned context, authorization and service errors."),
    ("integration", "log-ingest", "Log ingestion and agent log context", APP+"backend/services/log-ingest", "Compare channel creation/read/filtering, ownership and running-agent notifications."),
    ("persistence", "agents", "SQLite agent history, migrations and recovery", CORE+"services/agent-persistence", "Verify schema migrations, WAL/restart behavior, corrupt-row handling and archive retention."),
    ("persistence", "preferences", "Versioned preferences and provider migration", APP+"backend/services/preferences.ts", "Round-trip every schema field, defaults, legacy migration and concurrent updates."),
    ("persistence", "caches", "File read, processed image, asset and favicon caches", CORE+"services/file-read-cache", "Verify invalidation, storage bounds, cleanup and no stale context after edits."),
    ("persistence", "restore", "Window, tabs, workspaces and application data paths", APP+"backend/utils/paths.ts", "Compare clean close, crash restore, tab state, workspaces and data-reset boundaries."),
    ("account", "auth", "Stagewise account sign-in/session/sign-out", APP+"backend/services/auth", "Compare valid/expired/cancelled auth flows; record hosted-service availability separately."),
    ("account", "usage", "Provider usage limits, cost and account panels", APP+"ui/screens/settings/sections/account-section.tsx", "Compare usage retrieval, refresh, unknown/no-account states and provider-specific limits."),
    ("distribution", "updates", "Update checking, download, install and release notes", APP+"backend/services/auto-update.ts", "Use own signed update endpoint; compare state transitions, cancellation and recovery without impersonating upstream."),
    ("distribution", "packaging", "Windows/Linux/macOS packaging and protocol registration", "apps/browser/forge.config.mts", "Reimplement installer/uninstaller, resource loading and protocol integration for native distribution."),
    ("distribution", "telemetry", "Telemetry preferences and event policies", APP+"backend/services/telemetry.ts", "Honor off/anonymous/full preference and own service endpoint; test no unwanted transmission."),
]
for cat, key, title, path, acceptance in GROUPS:
    add(cat, key, title, [evidence(path)], acceptance)

# Settings detail requirements called out by the user's reference screenshots.
SETTINGS_DETAIL = [
    ("general-awake", "Agent General: keep awake while agents work", "general-settings-section.tsx", "Verify active-work versus idle/question/approval policy and persisted toggle."),
    ("general-sounds", "Agent General: notification loudness, sound pack, preview and import", "general-settings-section.tsx", "Verify completion/question/error sound, loudness levels, sound preview and custom import errors."),
    ("provider-instances", "Models & Providers: add, rename, configure and disconnect provider instances", "models-providers-section.tsx", "Verify each provider type's required credentials/options, model discovery/search/filtering, enable/disable, bulk actions and stable instance identity."),
    ("utility-title", "Utility models: ordered title generation fallbacks", "model-presets-section.tsx", "Add/remove/reorder provider-model entries; configure thinking per entry; verify ordered fallback on failure and persisted order."),
    ("utility-compression", "Utility models: ordered context compression fallbacks", "model-presets-section.tsx", "Compare default/main-model resolution, thinking overrides and ordered fallback when compression model requests fail."),
    ("presets", "Presets: main model, fallbacks and utility overrides", "model-presets-section.tsx", "Create/rename/edit/delete presets; reorder main/fallback models; verify per-model thinking and preset-specific title/compression overrides."),
    ("skills-context", "Skills & Context files: workspace/global sources and enablement", "skills-context-section.tsx", "Compare tabs, discovered skill/context files, enabled state, file viewing and refresh on changes."),
    ("worktree-scripts", "Worktrees: setup scripts and environment variables", "agent-settings.worktree-setup.tsx", "Compare per-workspace scripts, source/target/main path variables, editing/saving, run logs, failure state and deletion safeguards."),
    ("plugin-cards", "Plugins: enabled cards, detail pages and credentials", "plugins-section.tsx", "Compare plugin names/descriptions/status, enable/disable, open detail, credential management and install instructions."),
    ("archived-chats", "Archived chats: search, restore and permanent deletion", "archived-agents-section.tsx", "Compare pagination/search, title/time details, restore destination and confirmed deletion."),
    ("personalization", "Personalization: UI size, appearance and color scheme", "personalization-settings-section.tsx", "Compare scale, system/light/dark appearance, every theme, previews and persistence."),
    ("search-engine", "Browsing General: search engine add/default/remove", "browsing-settings-section.tsx", "Verify name/keyword/URL fields, %s URL validation, radio/default selection, custom deletion and omnibox use."),
    ("new-tab-startup", "Browsing General: new-tab and startup pages", "browsing-settings-section.tsx", "Compare home/custom URL choices, validation, live new-tab behavior and restart behavior."),
    ("permission-defaults", "Browsing General: default permissions and exceptions", "browsing-settings-section.tsx", "Verify each configurable permission's default, host overrides, detail navigation and requests made by test pages."),
    ("clear-data", "Browsing: clear-data options and confirmation", "clear-data-section.tsx", "Compare categories and time range, counts/results, completion errors and preservation of unselected data."),
    ("history", "Browsing History: grouped entries, search and deletion", "history-section.tsx", "Compare date groups, filtering, URL open, selection/deletion, pagination and empty states."),
    ("account", "Account: Google, GitHub, email and session management", "account-section.tsx", "Compare supported login methods, email flow, cancelled/failed auth, sign-out, profile/usage and service availability."),
    ("subscriptions", "Account/provider setup: own API keys and existing subscriptions", "models-providers-section.tsx", "Verify BYOK onboarding and all listed subscription/plan or coding-agent connections against the native implementation."),
    ("about", "About: version, update state, licenses and app data", "about-section.tsx", "Compare version/release info, update checks, source/license/third-party links, data location and reset actions."),
]
for key, title, filename, acceptance in SETTINGS_DETAIL:
    add("settings_detail", key, title,
        [evidence(APP+"ui/screens/settings/sections/"+filename)], acceptance,
        requested_reference="User-supplied Stagewise settings screenshots")
for plugin in ["Figma", "GitHub", "PostHog", "Supabase", "Vercel"]:
    add("plugin", plugin.lower(), plugin,
        [evidence(APP+"ui/screens/settings/sections/plugins-section.tsx"), evidence(APP+"shared/plugins.ts")],
        "Match the reference enabled card and plugin-specific details/credentials; verify a permitted read operation against its actual service before claiming integration parity.",
        evidence_kind="user_reference_requires_runtime_verification")

for path in FILES:
    if path.startswith("apps/browser/bundled/plugins/") and path.endswith("/metadata.json"):
        metadata = json.loads(read(path))
        slug = Path(path).parent.name
        add("bundled_plugin", slug, metadata.get("displayName", slug), [evidence(path)],
            "Match bundled skill discovery, required credential definitions, enabled state and plugin operations; verify each live integration separately.",
            description=metadata.get("description"), required_credentials=metadata.get("requiredCredentials", []))

# All registered provider kinds, preserving runtime identity separately from model names.
path = APP + "backend/agents/providers/registry.ts"
data = read(path)
block = data[data.index("PROVIDER_TYPE_REGISTRY"):data.index("/**", data.index("PROVIDER_TYPE_REGISTRY"))]
for m in re.finditer(r"^  (?:'([^']+)'|([\w-]+)): (\w+),", block, re.M):
    key = m.group(1) or m.group(2)
    add("provider", key, key, [evidence(path, m.group(0))],
        "Connect/disconnect; discover models; stream a native session with tools/cancel/errors; compare provider-instance settings and credentials.",
        implementation_symbol=m.group(3))

# Every explicit settings destination, including currently hosted-service surfaces.
path = APP + "shared/settings-route.ts"
data = read(path)
for section in re.findall(r"\| '([^']+)'", data.split("export type SettingsRoute")[0]):
    add("settings", section, section, [evidence(path, "'"+section+"'")],
        "Match all visible fields, validation, reset/default values, navigation and restart persistence for this settings section.")

# Every current browser-facing tool name, plus the universal definitions as evidence.
path = APP + "backend/services/toolbox/index.ts"
data = read(path)
tool_block = data[data.index("case 'write'"):data.index("case 'write'")+4200]
for tool in dict.fromkeys(re.findall(r"case '([^']+)'", tool_block)):
    add("tool", tool, tool, [evidence(path, "case '"+tool+"'")],
        "Match input/output/error schemas and behavioral fixtures; test approval, mounted-path limits, cancellation and effect persistence.")

# Every declared shortcut action and its exact original accelerator fields.
path = APP + "shared/hotkeys.ts"
data = read(path)
enums = dict(re.findall(r"^  (\w+) = '([^']+)'", data, re.M))
for symbol, key in enums.items():
    match = re.search(r"\[HotkeyActions\."+re.escape(symbol)+r"\]: \{(.*?)\n  \}", data, re.S)
    bindings = {}
    if match:
        for name, value in re.findall(r"(accelerator|mac): '([^']+)'", match.group(1)):
            bindings[name] = value
        for name, content in re.findall(r"(aliases|macAliases): \[([^\]]+)\]", match.group(1)):
            bindings[name] = re.findall(r"'([^']+)'", content)
        for name, value in re.findall(r"(captureDominantly|preserveNativeInput): (true|false)", match.group(1)):
            bindings[name] = value == "true"
    add("shortcut", key, key, [evidence(path, symbol+" =")],
        "Verify the action with focus in chat, native editor, terminal, browser and dialogs; apply platform override and alias rules.", bindings=bindings)

# Internal document/page routes.
for path in FILES:
    if path.startswith(APP+"pages/routes/") and path.endswith(".tsx"):
        for match in re.finditer(r"createFileRoute\('([^']+)'\)", read(path)):
            route = match.group(1)
            add("page_route", route, route, [evidence(path, match.group(0))],
                "Compare navigation, direct open, content rendering, not-found/error states and accessibility.")

# Static registered procedure names: the concrete product action surface.
procedures = collections.defaultdict(list)
for path in FILES:
    if not path.startswith(APP+"backend/") or not path.endswith((".ts", ".tsx")) or ".test." in path:
        continue
    data = read(path)
    for match in re.finditer(r"registerServerProcedureHandler\(\s*'([^']+)'", data):
        procedures[match.group(1)].append(evidence(path, line=data[:match.start()].count("\n")+1))
for key, sources in sorted(procedures.items()):
    add("procedure", key, key, sources,
        "Implement the corresponding native command; match the pinned contract arguments/results, errors and state transitions against upstream.")

# Core command names are routed through a helper instead of direct Karton registration.
path = CORE+"services/agent-manager/agent-manager.ts"
for match in re.finditer(r"wrapAgentRpc\(\s*'([^']+)'", read(path)):
    add("agent_command", match.group(1), match.group(1), [evidence(path, match.group(0))],
        "Match this agent command's inputs, state changes, persistence, errors and cancellation; verify through native UI and backend fixtures.")

# Declared API operations also catch actions registered through generic dispatch.
path = APP+"shared/karton-contracts/ui/index.ts"
active = False
scopes = []
for line_no, line in enumerate(read(path).splitlines(), 1):
    if line == "  serverProcedures: {":
        active = True
        continue
    if not active:
        continue
    if line.startswith("  };"):
        break
    group = re.match(r"(\s+)(\w+): \{\s*$", line)
    if group:
        indent = len(group.group(1))
        scopes = [entry for entry in scopes if entry[0] < indent]
        scopes.append((indent, group.group(2)))
    function = re.match(r"(\s+)(\w+): \(", line)
    if function:
        indent = len(function.group(1))
        scopes = [entry for entry in scopes if entry[0] < indent]
        name = ".".join([entry[1] for entry in scopes]+[function.group(2)])
        add("contract_operation", name, name, [evidence(path, line=line_no)],
            "Compare the complete pinned argument/result declaration, documented error cases and all state effects with the native command.",
            evidence_kind="declared_contract_requires_behavior_mapping")

# Every schema field declaration is indexed as a configuration/data contract candidate.
# This intentionally includes non-preference schemas and is not a claim of visible UI.
schema_fields = []
schema_path = APP+"shared/karton-contracts/ui/shared-types.ts"
schema_name = None
for line_no, line in enumerate(read(schema_path).splitlines(), 1):
    decl = re.match(r"(?:export )?const (\w+Schema)\b", line)
    if decl:
        schema_name = decl.group(1)
    match = re.match(r"\s+([\w-]+): (z\.|\w+Schema|.*\.default\()", line)
    if match and schema_name:
        schema_fields.append({"schema_candidate": schema_name, "field": match.group(1),
                              "native_status": "not_started", "mapping_status": "requires_semantic_review",
                              "source": evidence(schema_path, line=line_no)})

# Full tracked-source manifest ensures no implementation directory is silently omitted.
# File rows are a review checklist, not independent user-facing feature claims.
source_prefixes = ("apps/browser/", CORE, "packages/agent-shell/src/", "agent/runtime-node/src/",
                   "packages/karton/src/", "packages/stage-ui/src/", "packages/icons/src/",
                   "apps/update-server/")
manifest = []
for path in FILES:
    if not path.startswith(source_prefixes):
        continue
    blob = (ROOT/path).read_bytes()
    kind = "test" if re.search(r"\.(test|spec)\.", path) else "story" if ".stories." in path else "source_or_asset"
    manifest.append({"path": path, "sha256": hashlib.sha256(blob).hexdigest(), "bytes": len(blob),
                     "kind": kind, "review_status": "not_started", "native_status": "not_started"})

ids = [row["id"] for row in features]
assert len(ids) == len(set(ids)), "duplicate feature ids"
inventory = {
    "schema_version": 1,
    "baseline": {"repository": "https://github.com/stagewise-io/stagewise", "sha": SHA,
                 "version": json.loads(read("apps/browser/package.json"))["version"],
                 "captured_date": "2026-11-05", "checkout": str(ROOT)},
    "target": {"interface": "native C++/Qt", "backend": "native C++/Qt", "toolchain": "Qt 6 MinGW static requested",
               "node_sidecar": False, "electron": False},
    "status_semantics": {
        "not_started": "No native implementation/parity evidence claimed by this initial inventory.",
        "source_confirmed": "Symbol or module exists in pinned source, not runtime-verified.",
        "requires_semantic_review": "Mechanical extraction; exact field owner/behavior still needs review.",
        "not_run": "No upstream live session was executed for this row.",
        "not_verified": "No native/upstream comparison has passed for this row."},
    "coverage_limits": [
        "Source manifest covers every tracked file in listed prefixes; this is not a proof that every semantic behavior is enumerated.",
        "Static procedure extraction does not include dynamically constructed registration names.",
        "Shared schema rows are lexical candidates; nested schemas and field aliases require semantic mapping.",
        "Marketing website and deprecated CLI are outside desktop feature parity; hosted dependencies are tracked as integration risks.",
        "Provider/model availability and third-party authentication must be verified live without printing credentials.",
        "Native implementation statuses are baseline values, not a live reflection of concurrent development."
    ],
    "counts": {"features": len(features), "categories": dict(sorted(collections.Counter(x["category"] for x in features).items())),
               "schema_field_candidates": len(schema_fields), "source_manifest_files": len(manifest),
               "tests": sum(x["kind"] == "test" for x in manifest)},
    "features": features, "schema_field_candidates": schema_fields,
    "source_manifest": manifest,
}
(AUDIT/"upstream-feature-inventory.json").write_text(json.dumps(inventory, indent=2, ensure_ascii=False)+"\n", encoding="utf-8")
(AUDIT/"upstream-baseline.json").write_text(json.dumps({
    **inventory["baseline"], "installed_executable": r"C:\Users\secus\AppData\Local\stagewise\app-1.31.1\stagewise.exe",
    "installed_running_observed": True, "installed_file_version": "1.31.1", "source_dependencies_installed": False,
    "upstream_build_run": False, "live_provider_session_run": False,
    "credentials_read_by_inventory": False, "counts": inventory["counts"]
}, indent=2)+"\n", encoding="utf-8")
print(json.dumps(inventory["counts"]))

# Non-secret endpoint metadata for the native provider implementation.
provider_meta_path = APP+"shared/karton-contracts/ui/shared-types.ts"
provider_meta = read(provider_meta_path)
provider_meta = provider_meta[provider_meta.index("export const PROVIDER_TYPE_DISPLAY_INFO"):provider_meta.index("export const thinkingProviderSchema")]
endpoints = {}
for match in re.finditer(r"^  (?:'([^']+)'|(\w+)): \{(.*?)(?=^  (?:'[^']+'|\w+): \{|\Z)", provider_meta, re.M|re.S):
    key = match.group(1) or match.group(2)
    urls = re.search(r"defaultBaseUrl: '([^']+)'", match.group(3))
    if urls:
        endpoints[key] = {"base_url": urls.group(1), "source": evidence(provider_meta_path, urls.group(0))}
validation_path = APP+"backend/agents/providers/official-api.ts"
validation = read(validation_path)
validation = validation[validation.index("const VENDOR_VALIDATION_MODEL:"):validation.index("// ============================================================================", validation.index("const VENDOR_VALIDATION_MODEL:"))]
validation_models = {(m.group(1) or m.group(2)): m.group(3) for m in re.finditer(r"^  (?:'([^']+)'|(\w+)): '([^']+)'", validation, re.M)}
mapping = {"ZAI":"z-ai", "GEMINI":"google", "XAI":"x-ai", "OPENAI":"openai", "DEEPSEEK":"deepseek", "MINIMAX":"minimax", "MOONSHOT":"moonshotai", "QWEN":"alibaba"}
key_metadata = {}
for label in ["PORTKEY", "YOU", "SAKANA", "THEROUTER", "ZAI", "GEMINI", "XAI", "OPENAI", "DEEPSEEK", "PERPLEXITY", "MINIMAX", "MOONSHOT", "QWEN", "NVIDIA"]:
    if label not in mapping:
        key_metadata[label] = {"status": "not_configured", "reason": "No direct upstream provider endpoint found; do not infer destination from credential label.", "base_url": None}
        continue
    vendor = mapping[label]
    key_metadata[label] = {"status": "upstream_source_confirmed_not_account_verified", "provider_type": vendor+"-api", **endpoints[vendor+"-api"],
                           "api_spec": "openai-responses" if vendor == "openai" else "google" if vendor == "google" else "openai-chat-completions",
                           "upstream_validation_model": validation_models[vendor], "default_model": None,
                           "model_note": "Validation probe model is not the default chat model and its current availability has not been checked.",
                           "validation_source": evidence(validation_path, "'"+validation_models[vendor]+"'")}
plan_path = APP+"shared/coding-plans.ts"
plan_text = read(plan_path)
plans = []
for match in re.finditer(r"^  '([^']+)': \{(.*?)(?=^  '[^']+': \{|^\};)", plan_text, re.M|re.S):
    fields = {name: value for name, value in re.findall(r"\b(provider|displayName|baseUrl|validationBaseUrl|validationModelId):\s*'([^']+)'", match.group(2))}
    plans.append({"id":match.group(1), **fields, "source":evidence(plan_path, "'"+match.group(1)+"':")})
model_path = APP+"shared/available-models.ts"
catalog = []
for match in re.finditer(r"^  \{\n(.*?)(?=^  \{|^\] as)", read(model_path), re.M|re.S):
    model = re.search(r"^    modelId: '([^']+)'", match.group(1), re.M)
    vendor = re.search(r"^    officialProvider: '([^']+)'", match.group(1), re.M)
    if model and vendor:
        catalog.append({"id":model.group(1), "provider":vendor.group(1), "source":evidence(model_path, "modelId: '"+model.group(1)+"'")})
assert catalog, "Model catalog extraction returned no models"
(AUDIT/"provider-endpoints.json").write_text(json.dumps({
    "baseline_sha": SHA,
    "scope": "Metadata extracted from pinned Stagewise source; no credentials read and no network calls made.",
    "credential_labels": key_metadata, "upstream_endpoints": endpoints, "coding_plans":plans,
    "catalog_models":catalog,
    "notes":["Never send a provider key to another host to test compatibility.",
             "QWEN and ZAI subscription-plan tokens have distinct endpoints; a key label alone does not prove which account product it belongs to.",
             "Discovery/catalog model IDs require availability verification before use; a validation model is not a default model."]
}, indent=2)+"\n", encoding="utf-8")
