# Native context loader

Reference: Stagewise 1.31.1, commit `17a0123a0e896885465df36169457199d5225eb5`.
Implementation: `src/core/ContextLoader.h` and `ContextLoader.cpp`.
Regression source: `tests/test_context.cpp`.

## Source-backed discovery behavior

- `packages/agent-core/src/services/mount-manager/workspace-info/agents-md.ts`
  reads the workspace-root AGENTS.md, capped at 40 KiB of UTF-8. The native
  default also reads only that root file; it never walks above the workspace.
- `workspace-info/skills.ts` discovers immediate child skill directories,
  including linked directories, and requires string name/description frontmatter.
  Workspace `.stagewise/skills` wins over `.agents/skills` by exact skill name.
- `apps/browser/src/backend/services/toolbox/index.ts`, `getSkillsList`, resolves
  workspace skills before global skills, suppresses disabled names, and assigns
  independent IDs to plugin skills. Builtin commands have separate command IDs.
- `services/toolbox/global-skills.ts` enables `.stagewise/skills` and
  `.agents/skills` by default; `.codex/skills` and `.claude/skills` require opt-in.
  The native host must explicitly supply the home directory, so tests and isolated
  profiles cannot accidentally read the real user's skill folders.
- `env/adapters/enabled-skills.ts` excludes agent-invocable=false entries from
  the agent roster. Native explicit user invocations also honor user-invocable.
  Plugin bodies and supporting references are read lazily through readContext.

## Scope and trust

The loader performs local, read-only operations. It does not load credentials,
contact a service, execute scripts, or implement plugin service operations.
Rendered instructions retain source paths, scopes and respected flags; content
is subordinate to conversation instructions. XML attributes are escaped and
instruction/explicit-skill bodies use safely split CDATA sections.

Optional relevantPaths discovers nested AGENTS.md ancestors within the workspace,
from broad to narrow scope. This is an explicit native extension, not a claimed
upstream default. Unrelated sibling instructions and parent-directory instructions
are not injected. Disabled instructions retain a manifest entry but their contents
are omitted from the prompt and readContext access.

Discovered symlink/junction skill directories resolve to their actual directory,
matching upstream linked-skill discovery. Supporting-file reads are restricted to
that resolved skill directory with Windows final-handle path validation. Traversal,
absolute paths and junction escapes into unrelated directories are rejected.

## Bounds and current differences

- Skill/reference files are capped at 1 MiB; instruction files use the upstream
  40 KiB cap with UTF-8-safe truncation. Discovery caps include 512 skills,
  2,048 entries per directory, 64 instruction files and 1 MiB rendered characters.
- Frontmatter supports the pinned bundled metadata and common plain, quoted,
  boolean, literal and folded scalar fields. Complex YAML aliases/tags and some
  uncommon YAML scalar forms are rejected with diagnostics. This is not a full
  YAML parser and is not claimed as complete frontmatter parity.
- Builtin/plugin directories are supplied by the native host. Upstream plugin
  documents describing unavailable native APIs must not be presented as proof of
  implemented capabilities. In particular, the upstream JavaScript sandbox skill
  describes Node.js behavior that differs from the current QuickJS worker.
- Rich upstream environment diff snapshots, multiple workspace mounts, runtime
  filesystem watchers and automatic relevant-path inference are not provided by
  this loader. Calling load again refreshes files and yields a prompt fingerprint.

Fixture validation: `test-results/context-tests.txt` records 9 passes, zero
failures and zero skips (7 cases plus setup/cleanup), including a real NTFS
junction. The test source covers isolated precedence/opt-in,
metadata flags, scoped instructions, UTF-8 caps, markup escaping, virtual reads,
real NTFS junctions, refresh fingerprints and all 14 pinned bundled skills.

AgentEngine::setSystemContext appends this rendered context under distinct
local_workspace_context markers and preserves it throughout a running turn.
readContext is registered as a read-only external tool. A loopback provider/tool
round-trip verifies context preservation, no approval prompt for the reader,
and native-runtime capability wording. Root runtime integration is recorded
separately; parsing all bundled metadata does not mean those plugin services
or their source runtime APIs have been implemented or enabled.
