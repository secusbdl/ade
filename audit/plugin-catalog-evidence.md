# Native bundled plugin catalog and credential adapter

`src/plugins/PluginCatalog.{h,cpp}` supplies pinned plugin metadata, immutable-resource extraction for ContextLoader, and configuration of the native NetworkBroker from the separate encrypted plugin store. It performs no remote requests or service actions.

The five packaged skills are GitHub, PostHog, Supabase, Vercel and Figma. Their `SKILL.md` and `metadata.json` files are byte-identical to Stagewise 1.31.1 at commit `17a0123a0e896885465df36169457199d5225eb5`, from `apps/browser/bundled/plugins`. The source copyright/license and every packaged file's SHA-256 are recorded in `assets/plugins/PROVENANCE.json`; the source LICENSE is preserved. The Figma app retains its source Figma brand graphic inside its own HTML; ADE's surrounding UI does not claim that identity.

The first four skills require `fetch`, `API.getCredential`, the existing sandbox JavaScript runtime, timers and `API.output`. Catalog readiness describes required native APIs; it does not establish live account permissions, API endpoint behavior, or all service operations. Worker/broker integration is tested separately by the runtime owner.

Figma was activated after `test-results/sandbox-app-integration.txt` passed **4/4** through the actual native QuickJS worker, CDP event bridge, MiniAppHost and WebView iframe. A local fake Figma selection reached the unchanged skill recipe, updated the packaged app badge and returned an acknowledgement to a later script invocation. Its required `API.openApp`, `API.closeApp`, `API.sendMessage`, `API.onMessage` and `API.onCDPEvent` native APIs are now present. This fixture does not establish real Figma account/session behavior. The packaged app has one documented hardening patch: custom payloads use a `pre` element's `textContent` instead of interpolating JSON into `innerHTML`. The fixture verifies literal `<img onerror>` text with no created image or injected execution; upstream source remains unchanged. Both index hashes and the patch are in provenance.

## Host API

- `definitions()` returns five runtime-ready metadata records.
- `materialize(dataDir,error)` extracts resources into `dataDir/bundled-plugins/stagewise-1.31.1-<SHA256>`. Resource hashes are verified first. Existing files must match exactly; modified files, unexpected entries, symbolic links and junctions are rejected. Existing user-owned content is never overwritten. The extracted tree includes provenance/license, all five skill directories, and `figma/apps/figma-app/index.html` plus `styles.css`. Every allowed nested parent directory is derived from the fixed resource list.
- `context(dataDir,preferences,error)` returns `PluginContext {pluginsDirectory, disabledPluginIds, enabledCredentialTypes}` for ContextLoader. Plugins are enabled unless `plugins[id].enabled` explicitly says false, matching upstream `plugins-section.tsx`.
- Instance `configureAgent(broker,hostAgentId,dataDir,preferences,error)` reads only `dataDir/plugin-credentials.json` through ProviderStore's Windows DPAPI format. Records must use one of the known plugin IDs and `protocol="credential"`. Model-provider configuration is never read. Stored URLs cannot select credential destinations: the source NetworkBroker registry owns exact allowed origins and field names.

The catalog caches encrypted-file revisions separately for each broker using QPointer lifetime tracking. Repeated unchanged calls preserve handles; a changed/deleted/corrupt store revokes prior plugin credentials. Preferences update each agent's allowed credential types independently. Failed loading or malformed configuration clears credentials rather than retaining stale secrets. Root must invoke configuration when preferences/credentials change so in-flight calls are revoked promptly; this module does not poll or watch files.

## Verification

Independent native MinGW/static Qt 6.11.2 probe in `private/plugins-probe-project`, without configuring the shared application build. The QRC is linked and initialized by the production code, so the test exercises actual embedded resources.

`test-results/plugins-tests.txt`: **11 passed, 0 failed, 0 skipped** (9 cases plus setup/cleanup). Coverage includes:

- Exact resource hashes and requiredCredentials metadata; five default-enabled skills; actual ContextLoader reading of enabled GitHub/Figma skills and denial after explicit Figma disabling.
- Idempotent nested Figma app extraction, patch provenance hashes, modified app preservation and rejection of an unexpected app script without deleting it.
- Corrupt and extra extracted files rejected with their contents preserved, and a real Windows NTFS junction escaping the data directory rejected without writing outside it.
- Temporary-file/flush/no-replace rename installation: a real destination collision preserves the existing bytes and removes only the operation's temporary file. Successful installation leaves no temporary file. This fixes the reviewed partial-final-file recovery issue; disk-full behavior itself was not injected.
- Actual DPAPI encryption of invented tokens; no plaintext token in the stored file; agent-specific access and exact source-origin routing despite an untrusted stored base URL.
- Stable handles for unchanged configuration, new handles after reset/rotation/allowlist changes, denial of old Figma handles after disabling, corrupt/empty/deleted store handling, rejection of provider-protocol records and malformed preferences, and cache invalidation when a broker is destroyed. The new allowlist-change fixture initially exposed a stale test expectation that an unchanged GitHub token handle survived a changed agent scope; the existing broker correctly revokes all handles on scope change. The revised test explicitly requires that revocation.

No actual plugin credentials were read, no user repository or service was modified, and no external HTTP request was made by this suite. These tests establish this local adapter's tested behavior, not full Stagewise plugin parity or a universal security guarantee.
