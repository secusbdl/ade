# Native core implementation evidence

Baseline: Stagewise 1.31.1, upstream commit
`17a0123a0e896885465df36169457199d5225eb5`. This is an incremental native
implementation, **not evidence of full application or agent parity**.

## Implemented

- `src/core/AgentEngine.*`: Qt Network OpenAI-compatible streaming chat and
  sequential function-call loop; tool approval/denial; cancellation; bounded
  pre-output transient retries; request deadlines; partial response retention;
  correlated tool results; a 40-model-step turn bound. Existing source contracts
  inform tool naming and behavior; this code is native C++.
- `src/core/ProviderProtocol.*`: native Responses, Anthropic Messages and Google
  streamGenerateContent requests, streamed tool calls, provider-native signed
  reasoning blocks retained in conversation history, and explicit reasoning/
  thinking/output-token settings. Actual response model IDs are exposed.
- `src/core/ProviderService.*`: authenticated model discovery for the four
  protocols, bounded pagination/deduplication, cancellation/timeouts, and a
  bounded no-tool validation request.
- `src/core/ModelConfig.h`, `ModelFallback.h`: resolved provider-instance/model
  chains, ordered overload fallback, five-minute fallback persistence and
  invalidation on preset/configuration change or explicit cancellation.
- `src/core/UtilityService.*`: actual title and context-briefing generation with
  ordered utility then main-model fallbacks, instance/model deduplication,
  per-attempt 15/30-second deadlines, settled abort before fallback, and no tools.
  Prompts are ported from pinned upstream with source attribution. Compression
  markers preserve original session history and trim only future model input.
- External browser tools can register schemas and receive unique dispatch IDs;
  stale replies, cancellations, and external tool deadlines are handled by the
  engine. The browser implementation itself is owned by another subagent.
- `src/core/SseParser.*`: incremental byte-based SSE parser, CR/LF boundaries,
  multiline data and per-event size bound.
- `src/core/ToolExecutor.*`: workspace-scoped `read`, `ls`, `write`, `multiEdit`,
  `mkdir`, `glob`, `grepSearch`, and approved `runCommand`. Atomic text writes;
  canonical ancestor checks; Windows final-handle path resolution for NTFS
  junctions; Windows job-object cleanup for command processes.
- `src/core/ProviderStore.*`: atomic configuration storage with Windows DPAPI
  credential encryption; dotenv API-key import. Eight known provider mappings
  are drawn from upstream metadata, with Google OpenAI compatibility URL from
  its official documentation. Unrecognized providers retain an empty endpoint;
  credentials are never sent to guessed hosts. Initial model selections are
  unverified starting points, not claims about account availability.

## Regression evidence

`test-results/core-tests.txt` initially found a real junction traversal defect:
Qt's canonical path alone preserved a Windows junction. The implementation now
uses `GetFinalPathNameByHandleW` and that case passes. A process teardown warning
was also fixed by settling still-running child process objects on destruction.

The latest independent core run had 40 passes, zero failures, zero skips (38 cases
plus setup and cleanup), using Qt 6.11.2 static and GCC 16.2.0. The additional
observer-lifetime and canceled/replaced file/process regressions are documented
in `cancellation-reentrancy-evidence.md`. It covers SSE fragmentation, scope
escapes including a real NTFS junction, failed multi-edit atomicity, DPAPI storage,
key import without guessed endpoints, actual loopback HTTP model/tool round trips,
approval denial, network/approval cancellation, truncated streams, transient
retries, actual PowerShell execution/cancellation, invalid submission settlement,
native Anthropic signed thinking/tool round trips, native Google thought-signature
and function-ID round trips, model discovery pagination, bounded validation,
external-tool cancellation, malformed credential record rejection, verified
provider routing metadata, discovery failures, exact raw UTF-8 reads, and native
Responses signed reasoning/tool round trips, explicit no-tool validation,
terminal snapshot deduplication, incomplete/error and nonstream responses.
New fallback/utility coverage exercises overload versus authentication failures,
fallback expiry/config invalidation, invalid model configuration, duplicate utility
entries, title cleanup/minimum validation, cancellation, compression boundaries,
original-history preservation and exclusion of compressed prefixes from requests.

A further actual defect was reproduced before fixing: the UTF-8 decoder buffered
an unfinished trailing sequence without flagging an error, allowing a raw read to
silently truncate. Whole-file decoding now uses Stateless plus ConvertInitialBom;
raw read and multiEdit both reject unfinished sequences, preserving original bytes.
Before-fix evidence: `test-results/core-utf8-before-fix.txt`.

ProviderStore save validation now rejects duplicate/empty IDs, negative numeric
options and output over the loader's 4 MiB limit before touching the destination.
The duplicate-ID overwrite defect was reproduced first in
`test-results/core-store-before-fix.txt`; the regression verifies the original
encrypted file remains byte-for-byte unchanged for every rejected save. The
engine also exposes lastTurnSucceeded, tested against normal completion,
incomplete streams and cancellation after partial text, for accurate title gating.

ContextLoader has a separate 9-pass suite for local discovery, scoped instructions,
frontmatter, disabled sources, references and NTFS junction boundaries. The engine
context API and readContext loop have a real loopback round-trip regression.
See `audit/context-loader-evidence.md` for source references and remaining gaps.

Eight isolated invocations of the existing native ADE executable were run against
the user's configured providers with a short no-tool prompt and a 45-second
timeout. Five returned the requested token: OpenAI, DeepSeek, xAI, Moonshot, and
ZAI. Google compatibility returned HTTP 404, MiniMax rejected the retired starter
model, and Qwen returned HTTP 401. No alternative endpoint was tried. Evidence:
`audit/provider-live-evidence.json`, `test-results/providers/*.json`. These live
requests used the earlier compatibility implementation. Follow-up authenticated
discovery at the same Google and MiniMax endpoints returned current models;
gemini-3.1-flash-lite and MiniMax-M3.1-Flash then passed bounded validation.
NVIDIA discovery returned 85 models but the selected listed model returned 404
on generation; Sakana discovery returned 8 models but fugu required a subscription;
TheRouter discovery returned 258 models but generation required account balance.
You.com discovery returned HTTP 402, so no generation was sent. Qwen was not
retried at another endpoint. Catalog availability does not establish generation
entitlement. Full evidence is appended in the same audit JSON.

The native Responses adapter supports streamed text, reasoning summaries,
function argument fragments, completed output snapshots, encrypted reasoning
replay, function outputs, cancellation and terminal error handling. Requests
disable storage and explicitly set tools to an empty array and tool_choice to
none for validation. Perplexity imports its officially documented Responses
endpoint with no guessed model. Anthropic, Google-native and Responses currently
have fixture evidence. Perplexity additionally passed a live native Responses
validation on the explicitly discovered google/gemini-3.1-flash-lite model after
an authenticated 54-model catalog response. This no-tool request is recorded in
`audit/provider-live-evidence.json` responses_followup, with binary digest and
per-request reports.

## Known incomplete or unverified behavior

- Bedrock, Azure auth modes, full provider-specific reasoning
  controls and multimodal content conversion remain
  outstanding. Anthropic and Google native implementations have fixture coverage
  but have not yet been live-validated here.
- The native system prompt is a minimal implementation prompt; upstream agent
  prompts, all tools, skills/MCP/ACP, subagents, automatic compression, attachments and
  context engineering are not fully ported.
- Main-model fallback currently advances only before assistant text/reasoning/tool
  output, preventing duplicate partial responses or repeated effects. Full upstream
  mid-stream recovery is not reproduced. Utilities have fixture coverage; their
  UI workflows and live requests require separate integration evidence. Compression
  converts the native text/tool subset; upstream rich attachment/environment state
  and automatic context thresholds are not implemented by this core slice.
- File operations are bounded UTF-8 text operations. Binary/document/image
  transformers, AST previews, all mount types, exact ripgrep/glob behavior,
  delete/copy/move undo and persisted diff history remain incomplete.
- Search currently uses bounded synchronous Qt regex matching; complete upstream
  performance and responsiveness under adversarial regexes are not established.
- File checks do not constitute an OS sandbox; approved shell commands run with
  user permissions. Full filesystem race resistance and Windows job-assignment
  failure behavior still need further hardening and tests.
- `runCommand` is a bounded noninteractive process, not upstream's persistent PTY
  system. Interactive input, background sessions, shell capture and full terminal
  parity remain outstanding.
- Conversation persistence to disk belongs to the parent UI/session store; the
  engine exposes JSON messages and preserves protocol pairing on cancellation.
- Full native-versus-upstream session comparison remains outstanding here. The
  parent's separate DeepSeek read/write/read test is not proof of complete agent
  parity or every provider's supported feature set.

No secret values were written into source, audit documents, or test output.
