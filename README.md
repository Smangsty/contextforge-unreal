# ContextForge Unreal

A thin third-party ContextForge adapter for Epic Games' official Unreal Engine MCP server.

## What it does

Unreal Engine 5.8 exposes its official MCP server over local Streamable HTTP. The documented default endpoint is:

`http://127.0.0.1:8000/mcp`

ContextForge admits third-party MCP packages over stdio, so ContextForge Unreal bridges stdio to Unreal's local Streamable HTTP endpoint. It deliberately does not implement Unreal Editor operations itself.

Unreal 5.8 Tool Search is the source of truth. The adapter exposes the same stable native surface:

- `list_toolsets`
- `describe_toolset`
- `call_tool`

Those calls are forwarded directly to Unreal. The adapter does not cache, synthesize, or maintain a second catalog of engine tools or schemas.

The upstream HTTP session is connected lazily and kept warm. Calls are serialized because Unreal executes MCP work on the game thread. If a warm connection goes stale after an Editor restart, the adapter resets it. Read-only Tool Search discovery may retry once; `call_tool` dispatches are never automatically retried because their effects may be ambiguous after a transport failure.

## Prerequisites

- Unreal Engine 5.8 with the experimental **Model Context Protocol** plugin enabled.
- **Enable Tool Search** enabled in **Editor Preferences > Model Context Protocol**. This is the Unreal 5.8 default and is required by this adapter.
- The **AllToolsets** plugin enabled for the full engine toolset collection, or the specific toolset plugins you want to expose.
- **Auto Start Server** enabled, or run `ModelContextProtocol.StartServer` in the Editor console.

Epic's current documentation:
https://dev.epicgames.com/documentation/unreal-engine/unreal-mcp-in-unreal-editor

Version 1.x targets Unreal's documented loopback endpoint. Keeping that default means ContextForge needs no launch-time configuration.

## Tool changes and hot reload

When Unreal tool registrations change, run `ModelContextProtocol.RefreshTools` in the Editor. Because ContextForge Unreal forwards native Tool Search calls instead of caching the engine catalog, subsequent discovery requests come from Unreal's current registry.

If the Editor MCP session itself becomes stale, restart the MCP server or the Editor. The adapter automatically reconnects after transport-level failures.

## ContextForge installation

This adapter is not bundled with ContextForge.

1. Open the latest release from `Smangsty/contextforge-unreal`.
2. Download the release `.tgz` through ContextForge Discovery to `MCP_DOWNLOADS`.
3. Prepare the downloaded artifact with Discovery.
4. Review the exact candidate in ContextForge.
5. Admit and enable it only after human review.

Discovery resolves the pinned MCP SDK dependency with lifecycle scripts disabled and re-seals the prepared tree before review.

## Network containment

ContextForge must grant this adapter its `INTERNET_CLIENT` network profile because the adapter opens an HTTP socket.

The adapter itself accepts loopback HTTP only. The default endpoint is hardcoded to `127.0.0.1`, and endpoint validation refuses remote hosts, HTTPS endpoints, credentials, query strings, and URL fragments.

## Design constraints

- No Unreal Engine binaries are redistributed.
- Unreal's in-editor MCP server owns engine behavior and tool schemas.
- The adapter owns transport conversion, loopback containment, call serialization, and connection recovery only.
- Mutating dispatches are never automatically retried after a connection failure.
- Native Unreal Tool Search is required. Eager mode is intentionally not reimplemented.
- The adapter can start while Unreal is closed; tool calls then return a direct prerequisite error.
- Epic's optional `unreal_mcp_proxy` is not required for ContextForge Unreal. Do not stack both proxies for the same connection.

## Development

`npm test` validates endpoint containment and the required native Tool Search surface.

`npm run check-version` verifies package, lockfile, adapter, and server manifest versions stay aligned.

`npm pack` runs tests and version checks before producing the release artifact.

## License

MIT
