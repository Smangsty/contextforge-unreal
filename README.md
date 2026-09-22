# ContextForge Unreal

A thin third-party ContextForge Skill for Epic Games' official Unreal MCP server.

## What it does

Unreal Engine 5.8 exposes its official MCP server over local Streamable HTTP. By default it binds to:

`http://127.0.0.1:8000/mcp`

ContextForge uses stdio for admitted third-party Skills, so this package provides the minimum transport adapter between those two MCP transports.

The adapter deliberately exposes a stable three-tool surface:

- `list_toolsets`
- `describe_toolset`
- `call_tool`

That mirrors Unreal's own Tool Search model and prevents ContextForge from ingesting a huge, constantly changing engine tool manifest.

The upstream HTTP session is connected lazily and then kept warm across tool calls, avoiding the reconnect-and-relist cost on every operation.

## Prerequisites

- Unreal Engine 5.8 with the experimental **Unreal MCP** plugin enabled.
- The **AllToolsets** plugin enabled for engine toolsets.
- In **Editor Preferences > Model Context Protocol**, enable **Auto Start Server**, or run:
  `ModelContextProtocol.StartServer`

Epic's current documentation:
https://dev.epicgames.com/documentation/unreal-engine/unreal-mcp-in-unreal-editor

Version 1.x intentionally targets Unreal's documented default endpoint. Keeping that default means ContextForge needs no launch-time configuration.

## ContextForge installation

This Skill is not bundled with ContextForge.

1. Find the latest release from `Smangsty/contextforge-unreal`.
2. Download the release `.tgz` through ContextForge Discovery to `MCP_DOWNLOADS`.
3. Prepare the downloaded artifact with Discovery.
4. Review the exact candidate in ContextForge.
5. Admit and enable it only after human review.

Discovery resolves the pinned MCP SDK dependency with lifecycle scripts disabled and re-seals the prepared tree before review.

## Network containment

ContextForge must grant this adapter its `INTERNET_CLIENT` network profile because it opens an HTTP socket.

The adapter itself hardcodes and validates Epic's loopback-only default endpoint. It does not accept remote hosts, HTTPS endpoints, credentials, query strings, or URL fragments.

## Design constraints

- No Unreal Engine binaries are redistributed.
- Unreal's in-editor MCP server does the engine work.
- The adapter performs transport conversion and stable tool-surface projection only.
- The Skill can start even when Unreal is closed; tool calls then return a direct prerequisite error.
- A stale warm connection is reset and retried once.

## Development

`npm test` validates the stable tool surface and loopback-only endpoint rules.

`npm pack` runs tests and version checks before producing the release artifact.

## License

MIT
