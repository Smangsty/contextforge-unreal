# ContextForge Unreal

A thin third-party ContextForge adapter for Epic Games' official Unreal Engine MCP server.

## What it does

Unreal Engine 5.8 exposes its official MCP server over local Streamable HTTP at:

`http://127.0.0.1:8000/mcp`

ContextForge Unreal bridges ContextForge's admitted stdio transport to that loopback endpoint. Version 2 exposes Unreal's native eager MCP tool catalog directly instead of wrapping it behind Tool Search.

In the Unreal project used to validate v2, the Editor exposed 830 direct tools in one `tools/list` response. ContextForge sees those exact Unreal names, descriptions, input schemas, and output schemas.

## Prerequisites

- Unreal Engine 5.8 with the experimental **Model Context Protocol** plugin enabled.
- **Enable Tool Search disabled** in **Editor Preferences > Model Context Protocol**.
- The **AllToolsets** plugin enabled for the full engine tool collection, or the specific toolset plugins you want.
- **Auto Start Server** enabled, or run `ModelContextProtocol.StartServer` in the Editor console.

After changing **Enable Tool Search**, restart the Unreal MCP server.

## Authority metadata

Unreal 5.8 eager tools currently do not provide MCP read/write annotations.

ContextForge Unreal therefore carries a reviewed exact-name READ manifest in `src/read-only-tools.mjs`. Listed tools are exposed with read-only authority hints. Every unlisted or newly introduced Unreal tool defaults to WRITE.

This is intentionally conservative. The adapter never uses runtime naming heuristics as security authority.

## Tool changes and hot reload

Unreal advertises `tools.listChanged: true`. ContextForge Unreal forwards native `notifications/tools/list_changed` notifications downstream, so ContextForge can refresh the direct tool catalog without a Skill restart.

Run `ModelContextProtocol.RefreshTools` in Unreal when tool registrations change.

## Connection behavior

The upstream HTTP session is connected lazily and kept warm. Unreal Editor tool calls are serialized because Editor MCP work runs on the game thread.

Reviewed READ calls may retry once when a previously warm connection has gone stale. WRITE calls are never automatically retried after dispatch because their outcome may be ambiguous after a transport failure.

## Faster updates

The release package contains a bundled adapter with no runtime npm dependencies. ContextForge Discovery can inspect and seal the package without performing a cold dependency hydration step.

Build and test dependencies remain development-only and are not required by the installed Skill.

## ContextForge installation

This adapter is not bundled with ContextForge.

1. Open the latest release from `Smangsty/contextforge-unreal`.
2. Download the release `.tgz` through ContextForge Discovery.
3. Prepare and review the exact candidate.
4. Admit and enable it only after human review.

## Network containment

ContextForge grants this adapter its `INTERNET_CLIENT` profile because the adapter opens an HTTP socket.

The adapter accepts loopback HTTP only. Endpoint validation refuses remote hosts, HTTPS endpoints, credentials, query strings, and URL fragments.

## Design constraints

- No Unreal Engine binaries are redistributed.
- Unreal owns engine behavior and the native tool schemas.
- The adapter owns transport conversion, loopback containment, reviewed authority annotations, serialized calls, and connection recovery.
- Native eager tools are required. Tool Search mode is rejected with a direct prerequisite error.
- The adapter can start while Unreal is closed; tool requests then report the missing Editor MCP endpoint.
- Epic's optional `unreal_mcp_proxy` is not required for ContextForge Unreal.

## Development

`npm run build` produces the bundled executable at `bin/contextforge-unreal.mjs`.

`npm test` validates endpoint containment, eager-mode detection, authority annotation behavior, and tool-list-change forwarding.

`npm run check-version` verifies package, lockfile, source, and server manifest versions stay aligned and prevents runtime dependencies from sneaking back into the release.

`npm pack` builds, tests, and validates the self-contained release artifact.

## License

MIT
