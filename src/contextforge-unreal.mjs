#!/usr/bin/env node

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";
import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
  ToolListChangedNotificationSchema
} from "@modelcontextprotocol/sdk/types.js";
import { pathToFileURL } from "node:url";

import { REVIEWED_READ_ONLY_TOOLS } from "./read-only-tools.mjs";

export const ADAPTER_VERSION = "2.0.1";
export const DEFAULT_UNREAL_MCP_ENDPOINT = "http://127.0.0.1:8000/mcp";
export const NATIVE_TOOL_SEARCH_TOOLS = Object.freeze([
  "list_toolsets",
  "describe_toolset",
  "call_tool"
]);

const REVIEWED_READ_ONLY_TOOL_SET = new Set(REVIEWED_READ_ONLY_TOOLS);

export function parseLoopbackEndpoint(value = DEFAULT_UNREAL_MCP_ENDPOINT) {
  if (typeof value !== "string" || value.length < 8 || value.length > 2048) {
    throw new Error("UNREAL_MCP_ENDPOINT_INVALID");
  }

  let url;
  try {
    url = new URL(value);
  } catch {
    throw new Error("UNREAL_MCP_ENDPOINT_INVALID");
  }

  const host = url.hostname.toLowerCase();
  if (
    url.protocol !== "http:" ||
    !["127.0.0.1", "localhost", "::1", "[::1]"].includes(host) ||
    url.username !== "" ||
    url.password !== "" ||
    url.hash !== "" ||
    url.search !== ""
  ) {
    throw new Error("UNREAL_MCP_ENDPOINT_REFUSED");
  }
  return url;
}

export function isNativeToolSearchOnly(tools) {
  if (!Array.isArray(tools) || tools.length !== NATIVE_TOOL_SEARCH_TOOLS.length) return false;
  const names = new Set(
    tools
      .map((tool) => (tool !== null && typeof tool === "object" ? tool.name : null))
      .filter((name) => typeof name === "string")
  );
  return NATIVE_TOOL_SEARCH_TOOLS.every((name) => names.has(name));
}

export function isReviewedReadOnlyTool(toolName) {
  return typeof toolName === "string" && REVIEWED_READ_ONLY_TOOL_SET.has(toolName);
}

export function annotateUnrealTool(tool) {
  const readOnly = isReviewedReadOnlyTool(tool?.name);
  return {
    ...tool,
    annotations: {
      ...(tool?.annotations ?? {}),
      readOnlyHint: readOnly,
      destructiveHint: !readOnly,
      idempotentHint: readOnly,
      openWorldHint: tool?.annotations?.openWorldHint === true
    }
  };
}

export function annotateUnrealTools(tools) {
  return Array.isArray(tools) ? tools.map(annotateUnrealTool) : [];
}

export function installToolListChangedForwarder(client, server) {
  client.setNotificationHandler(ToolListChangedNotificationSchema, async () => {
    await server.sendToolListChanged();
  });
}

export async function startContextForgeUnreal({ endpoint = parseLoopbackEndpoint() } = {}) {
  const server = new Server(
    { name: "contextforge-unreal", version: ADAPTER_VERSION },
    { capabilities: { tools: { listChanged: true } } }
  );

  let connection = null;
  let connecting = null;
  let operationTail = Promise.resolve();

  async function connectUpstream() {
    if (connection !== null) return connection;
    if (connecting !== null) return await connecting;

    connecting = (async () => {
      const client = new Client(
        { name: "contextforge-unreal-adapter", version: ADAPTER_VERSION },
        { capabilities: {} }
      );
      const transport = new StreamableHTTPClientTransport(endpoint);

      installToolListChangedForwarder(client, server);

      try {
        await client.connect(transport);
        const initialPage = await client.listTools();
        assertEagerTools(initialPage.tools);
        connection = { client, transport, initialPage };
        return connection;
      } catch (error) {
        await client.close().catch(() => undefined);
        throw error;
      }
    })();

    try {
      return await connecting;
    } finally {
      connecting = null;
    }
  }

  async function resetUpstream() {
    const current = connection;
    connection = null;
    if (current !== null) {
      await current.client.close().catch(() => undefined);
    }
  }

  async function withUpstream(operation, { retryConnectionFailure = false } = {}) {
    const reusedExistingConnection = connection !== null;
    try {
      return await operation(await connectUpstream());
    } catch (error) {
      if (isConnectionFailure(error)) {
        await resetUpstream();
        if (retryConnectionFailure && reusedExistingConnection) {
          return await operation(await connectUpstream());
        }
      }
      throw error;
    }
  }

  function serialize(operation) {
    const running = operationTail.then(operation, operation);
    operationTail = running.then(
      () => undefined,
      () => undefined
    );
    return running;
  }

  server.setRequestHandler(ListToolsRequestSchema, async (request) => {
    try {
      const page = await withUpstream(
        async (state) => {
          const cursor = request.params?.cursor;
          if (cursor === undefined && state.initialPage !== null) {
            const initialPage = state.initialPage;
            state.initialPage = null;
            return initialPage;
          }
          const result = await state.client.listTools(
            cursor === undefined ? undefined : { cursor }
          );
          assertEagerTools(result.tools);
          return result;
        },
        { retryConnectionFailure: true }
      );

      return {
        ...page,
        tools: annotateUnrealTools(page.tools)
      };
    } catch (error) {
      throw new Error(unrealListError(error, endpoint));
    }
  });

  server.setRequestHandler(CallToolRequestSchema, async (request) => {
    const toolName = request.params.name;
    const args = request.params.arguments ?? {};
    const retrySafe = isReviewedReadOnlyTool(toolName);

    return await serialize(async () => {
      try {
        return await withUpstream(
          async ({ client }) =>
            await client.callTool({
              name: toolName,
              arguments: args
            }),
          { retryConnectionFailure: retrySafe }
        );
      } catch (error) {
        return unrealRuntimeError(error, endpoint, toolName, retrySafe);
      }
    });
  });

  const shutdown = async () => {
    await resetUpstream();
  };
  process.once("SIGINT", shutdown);
  process.once("SIGTERM", shutdown);

  await server.connect(new StdioServerTransport());
}

function assertEagerTools(tools) {
  if (isNativeToolSearchOnly(tools)) {
    throw new Error("UNREAL_MCP_EAGER_TOOLS_REQUIRED");
  }
}

function eagerToolsRequirementMessage() {
  return [
    "ContextForge Unreal requires Unreal MCP eager tools.",
    "In Editor Preferences > Model Context Protocol, disable Enable Tool Search,",
    "then restart the MCP server or reconnect the Editor."
  ].join(" ");
}

function toolError(text) {
  return { content: [{ type: "text", text }], isError: true };
}

function isConnectionFailure(error) {
  const diagnostic = errorDiagnostic(error);
  return /ECONNREFUSED|ECONNRESET|EPIPE|fetch failed|connect|socket|closed/i.test(diagnostic);
}

function unrealListError(error, endpoint) {
  const diagnostic = errorDiagnostic(error);
  if (diagnostic.includes("UNREAL_MCP_EAGER_TOOLS_REQUIRED")) {
    return eagerToolsRequirementMessage();
  }
  if (isConnectionFailure(error)) {
    return [
      `Unreal MCP is unavailable at ${endpoint.origin}${endpoint.pathname}.`,
      "Open Unreal Engine 5.8, enable the Model Context Protocol and desired toolset plugins,",
      "and enable Auto Start Server or run ModelContextProtocol.StartServer in the Editor console."
    ].join(" ");
  }
  return `Unreal MCP could not list tools. ${
    diagnostic === "" ? "Verify the Editor MCP server is running." : diagnostic
  }`;
}

function unrealRuntimeError(error, endpoint, toolName, retrySafe) {
  const diagnostic = errorDiagnostic(error);

  if (diagnostic.includes("UNREAL_MCP_EAGER_TOOLS_REQUIRED")) {
    return toolError(eagerToolsRequirementMessage());
  }

  if (isConnectionFailure(error)) {
    if (!retrySafe) {
      return toolError(
        [
          `The Unreal MCP connection dropped while dispatching ${toolName}, so the tool outcome may be unknown.`,
          "ContextForge Unreal did not automatically retry the dispatch because it may have changed Editor state.",
          "Verify Unreal state, then retry only if needed."
        ].join(" ")
      );
    }

    return toolError(
      [
        `Unreal MCP is unavailable at ${endpoint.origin}${endpoint.pathname}.`,
        "The reviewed read-only call was safe to retry, but the Editor MCP endpoint is still unavailable."
      ].join(" ")
    );
  }

  return toolError(
    `Unreal MCP could not complete ${toolName}. ${
      diagnostic === "" ? "Verify the Editor MCP server is running." : diagnostic
    }`
  );
}

function errorDiagnostic(error) {
  if (!(error instanceof Error)) return "";
  const cause = error.cause;
  const causeText = cause instanceof Error ? ` ${cause.message}` : "";
  return `${error.message}${causeText}`;
}

async function main() {
  try {
    await startContextForgeUnreal();
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    process.stderr.write(`ContextForge Unreal: ${message}\n`);
    process.exitCode = 1;
  }
}

function isMainModule() {
  const entry = process.argv[1];
  return typeof entry === "string" && pathToFileURL(entry).href === import.meta.url;
}

if (isMainModule()) {
  await main();
}
