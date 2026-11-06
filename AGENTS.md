# ADE native port

The user requires a native C++/Qt 6 MinGW static application at this directory,
matching Stagewise 1.31.1 at upstream commit
17a0123a0e896885465df36169457199d5225eb5. Preserve the complete feature scope.
Partial implementation or passing tests never establish full parity.

The full local reference checkout is under upstream/stagewise and remains unchanged. Inventory
and evidence belong under audit. Implementation belongs under src; native tests
under tests. Build products and private runtime data are ignored.

The user now intends this repository as a reference for a later ADE application.
Keep the 701-row scope and report native gaps honestly. The curated pinned Git
blobs under reference/stagewise must remain byte-exact; verify them with
python scripts/verify-reference.py. Experimental native modules under
reference/native-experiments are opt-in test targets, not application features.
Use scripts/build.ps1 path parameters or the documented ADE_* environment
variables rather than assuming another machine has the original toolchain paths.

Do not print, embed, commit, or copy the user's API keys into source or evidence.
Keys may be read from D:/age/api_keys.md only for authorized runtime validation,
or stored using Windows DPAPI. Test with small bounded requests and use dedicated
scratch workspaces. Do not change the user's existing upstream conversations.

Keep source attribution and license notices. Do not copy proprietary Nucleo icons
or Stagewise trademarks into ADE's own UI. Use drawn icons and ADE branding.

Track gaps honestly. The native executable must not depend on Electron or a Node
sidecar. Qt libraries and compiler runtimes are statically linked. A system browser
runtime may be needed; record and verify the actual deployment dependencies.
