#!/usr/bin/env node

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";
import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { CallToolRequestSchema, ListToolsRequestSchema } from "@modelcontextprotocol/sdk/types.js";
import { pathToFileURL } from "node:url";

export const ADAPTER_VERSION = "1.1.0";
export const DEFAULT_UNREAL_MCP_ENDPOINT = "http://127.0.0.1:8000/mcp";
export const LIST_TOOLSETS = "list_toolsets";
export const DESCRIBE_TOOLSET = "describe_toolset";
export const CALL_TOOL = "call_tool";
export const NATIVE_TOOL_SEARCH_TOOLS = Object.freeze([
  LIST_TOOLSETS,
  DESCRIBE_TOOLSET,
  CALL_TOOL
]);

export const STABLE_TOOLS = Object.freeze([
  Object.freeze({
    name: LIST_TOOLSETS,
    description: "List all Unreal MCP toolsets available in the running Editor.",
    inputSchema: { type: "object", properties: {} },
    annotations: {
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false
    }
  }),
  Object.freeze({
    name: DESCRIBE_TOOLSET,
    description: "Describe an Unreal MCP toolset, including its tool names and input schemas.",
    inputSchema: {
      type: "object",
      properties: {
        toolset_name: {
          type: "string",
          description: "Toolset name returned by list_toolsets."
        }
      },
      required: ["toolset_name"]
    },
    annotations: {
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false
    }
  }),
  Object.freeze({
    name: CALL_TOOL,
    description: "Call a tool through Unreal MCP's native Tool Search dispatcher.",
    inputSchema: {
      type: "object",
      properties: {
        toolset_name: {
          type: "string",
          description: "Optional toolset containing the requested tool. Omit only for a top-level MCP tool."
        },
        tool_name: {
          type: "string",
          description: "Tool name without a toolset prefix."
        },
        arguments: {
          type: "object",
          description: "Arguments matching the selected Unreal tool schema."
        }
      },
      required: ["tool_name"]
    },
    annotations: {
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false
    }
  })
]);

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

export function missingNativeToolSearchTools(tools) {
  const available = new Set(
    Array.isArray(tools)
      ? tools
          .map((tool) => (tool !== null && typeof tool === "object" ? tool.name : null))
          .filter((name) => typeof name === "string")
      : []
  );
  return NATIVE_TOOL_SEARCH_TOOLS.filter((name) => !available.has(name));
}

export function hasNativeToolSearch(tools) {
  return missingNativeToolSearchTools(tools).length === 0;
}

export async function startContextForgeUnreal({
  endpoint = parseLoopbackEndpoint()
} = {}) {
  const server = new Server(
    { name: "contextforge-unreal", version: ADAPTER_VERSION },
    { capabilities: { tools: { listChanged: false } } }
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
      try {
        await client.connect(transport);
        const page = await client.listTools();
        const missing = missingNativeToolSearchTools(page.tools);
        if (missing.length !== 0) {
          throw new Error(`UNREAL_MCP_TOOL_SEARCH_REQUIRED:${missing.join(",")}`);
        }
        connection = { client, transport };
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

  server.setRequestHandler(ListToolsRequestSchema, async () => ({
    tools: STABLE_TOOLS
  }));

  server.setRequestHandler(CallToolRequestSchema, async (request) => {
    if (!NATIVE_TOOL_SEARCH_TOOLS.includes(request.params.name)) {
      return toolError(`Unsupported ContextForge Unreal tool: ${request.params.name}`);
    }

    const args = request.params.arguments ?? {};
    return await serialize(async () => {
      try {
        return await withUpstream(
          async ({ client }) => {
            return await client.callTool({
              name: request.params.name,
              arguments: args
            });
          },
          { retryConnectionFailure: isRetrySafeToolSearchRequest(request.params.name) }
        );
      } catch (error) {
        return unrealRuntimeError(error, endpoint, request.params.name);
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

export function isRetrySafeToolSearchRequest(toolName) {
  return toolName === LIST_TOOLSETS || toolName === DESCRIBE_TOOLSET;
}

function toolError(text) {
  return { content: [{ type: "text", text }], isError: true };
}

function isConnectionFailure(error) {
  const diagnostic = errorDiagnostic(error);
  return /ECONNREFUSED|ECONNRESET|EPIPE|fetch failed|connect|socket|closed/i.test(diagnostic);
}

function unrealRuntimeError(error, endpoint, toolName) {
  const diagnostic = errorDiagnostic(error);

  if (diagnostic.includes("UNREAL_MCP_TOOL_SEARCH_REQUIRED")) {
    return toolError(
      [
        "Unreal MCP native Tool Search is required by ContextForge Unreal.",
        "In Editor Preferences > Model Context Protocol, enable Enable Tool Search (the Unreal 5.8 default),",
        "then restart the MCP server or reconnect the Editor."
      ].join(" ")
    );
  }

  if (isConnectionFailure(error)) {
    if (toolName === CALL_TOOL) {
      return toolError(
        [
          "The Unreal MCP connection dropped while dispatching call_tool, so the tool outcome may be unknown.",
          "ContextForge Unreal did not automatically retry the dispatch because it may have changed Editor state.",
          "Verify the Unreal state, then retry only if needed."
        ].join(" ")
      );
    }

    return toolError(
      [
        `Unreal MCP is unavailable at ${endpoint.origin}${endpoint.pathname}.`,
        "Open Unreal Engine 5.8, enable the Model Context Protocol and desired toolset plugins,",
        "and enable Auto Start Server or run ModelContextProtocol.StartServer in the Editor console."
      ].join(" ")
    );
  }

  return toolError(
    `Unreal MCP could not complete the request. ${diagnostic === "" ? "Verify the Editor MCP server is running." : diagnostic}`
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
