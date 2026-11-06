# Native sandbox HTTP and credential broker

The native implementation is `src/network/NetworkBroker.{h,cpp}`. It uses Qt Network in the application host. Credential values remain in host memory; sandbox IPC receives opaque handles and bounded HTTP results. This module does not load model-provider configuration or the user's API-key file.

## Pinned source contract

Reference: Stagewise 1.31.1, commit `17a0123a0e896885465df36169457199d5225eb5`.

- `apps/browser/src/backend/services/credentials/index.ts:167-234`: resolve credential fields to agent-safe placeholders, retain secret metadata separately.
- `apps/browser/src/backend/services/sandbox/utils/credential-fetch.ts`: inspect placeholders in URL, headers and text body, require every referenced credential's destination-origin policy, then substitute. Lines 413-418 force manual redirects whenever credentials are present.
- `apps/browser/src/shared/credential-types/{figma,github,posthog,supabase,vercel}-pat.ts`: five plugin credential types, field names and exact service origins. Model-provider credentials and Stagewise authentication are deliberately absent from this registry.

The native broker retains manual credential redirects, exact scheme/host/port matching and whole-label wildcard matching for host-configured extensions. It additionally uses per-agent allowlists, 128-bit random handles, rejects unknown/revoked/foreign handles, percent-encodes substitutions in URLs, and keeps all substitution outside the worker. Header/cookie/authentication session reuse is disabled. Credential handles in binary request bodies or URL authority are rejected.

## Public host contract

`fetch(requestId, agentId, request)` emits `completed(requestId, response, error)`. Both identities are host-owned. Request fields are `url`, optional `method`, `headers` (object or string pairs), either `body` (text) or `bodyBase64`, and optional `redirect` (`follow`, `manual`, `error`). Responses contain `status`, `statusText`, string-pair `headers`, `bodyBase64`, `url`, `redirected`, and `ok`. HTTP 4xx/5xx are normal responses with `ok=false`; transport, cancellation and policy failures return an empty response plus a generic error.

`credential(agentId, typeId)` returns null unless host configuration allows that registered type and supplies its exact fields. `setCredential`, `registerCredentialType` and allowlist setters are host-only. `cancelAgent` cancels requests; `resetAgent` additionally revokes handles. Reset preserves the host's allowlist. Credential mutation/revocation invalidates existing handles before emitting cancellation signals, including reentrant callers.

Defaults: 30-second total deadline, 1 MiB upload, 2 MiB response body, 64 KiB/128 response headers, 8 concurrent requests and 5 public redirects. Expansion and base64 input are checked before constructing oversized bodies. Credential requests never follow redirects. Public cross-origin redirects strip caller headers except Accept, Accept-Language and Content-Type; HTTPS downgrade is rejected.

Known raw, percent-encoded, JSON-escaped and base64 representations of stored secrets are rejected if reflected in response bodies/headers/URLs. This bounded check is defense in depth, not a guarantee against every encoding or malicious permitted service. The primary isolation is host-held credentials plus explicit origin policies and manual redirects.

## Verification

Independent MinGW/static Qt 6.11.2 build: `private/network-probe-project` → `private/network-probe-build/test_network.exe`. No shared integrated build was configured during this check.

`test-results/network-tests.txt`: **13 passed, 0 failed, 0 skipped** (11 cases plus setup/cleanup). All requests use local loopback fixture servers and invented tokens. Coverage includes opaque substitution, forbidden origins, cross-agent handles, stale handles, credential redirect refusal, public redirect limits and header stripping, binary round-trip, HTTP errors, ambient-cookie isolation, response and upload limits, timeouts, cancellation/reused IDs, in-flight revocation, reentrant revocation, expansion limits, secret reflection and truncated responses.

`test-results/network-revocation-before-fix.txt` records the failing regression before the invalidation-order fix. That case now passes in the full suite.

This establishes the broker's tested contract only. The worker Fetch/Headers/Response adapter, plugin credential UI/storage integration and live plugin entitlement/actions require separate integration evidence. The broker buffers bounded responses; it does not implement streaming web bodies, browser cookie sessions, Node networking or arbitrary Node modules.
