#!/usr/bin/env node

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";
import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { CallToolRequestSchema, ListToolsRequestSchema } from "@modelcontextprotocol/sdk/types.js";
import { pathToFileURL } from "node:url";

export const DEFAULT_UNREAL_MCP_ENDPOINT = "http://127.0.0.1:8000/mcp";
export const LIST_TOOLSETS = "list_toolsets";
export const DESCRIBE_TOOLSET = "describe_toolset";
export const CALL_TOOL = "call_tool";

export const STABLE_TOOLS = Object.freeze([
  Object.freeze({
    name: LIST_TOOLSETS,
    description: "List Unreal MCP toolsets currently available in the running Editor.",
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
    description: "Describe one Unreal MCP toolset, including tool names and input schemas.",
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
    description: "Call an Unreal MCP tool by toolset and tool name.",
    inputSchema: {
      type: "object",
      properties: {
        toolset_name: {
          type: "string",
          description: "Optional toolset containing the requested tool."
        },
        tool_name: {
          type: "string",
          description: "Tool name without the toolset prefix."
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

export function toolsetName(toolName) {
  const separator = toolName.lastIndexOf(".");
  return separator > 0 ? toolName.slice(0, separator) : null;
}

export function summarizeToolsets(tools) {
  const counts = new Map();
  for (const tool of tools) {
    const toolset = toolsetName(tool.name);
    if (toolset === null) continue;
    counts.set(toolset, (counts.get(toolset) ?? 0) + 1);
  }

  const lines = [...counts.entries()]
    .sort(([left], [right]) => left.localeCompare(right, "en"))
    .map(([name, count]) => `- ${name} (${count} tool${count === 1 ? "" : "s"})`);

  return lines.length === 0
    ? "No Unreal toolsets are currently registered. Enable the AllToolsets plugin if you expected engine toolsets."
    : `Available Unreal toolsets:\n${lines.join("\n")}`;
}

export async function startContextForgeUnreal({
  endpoint = parseLoopbackEndpoint()
} = {}) {
  const server = new Server(
    { name: "contextforge-unreal", version: "1.0.0" },
    { capabilities: { tools: { listChanged: false } } }
  );

  let connection = null;
  let connecting = null;

  async function listAllTools(client) {
    const tools = [];
    let cursor;
    for (let pageIndex = 0; pageIndex < 32; pageIndex += 1) {
      const page = await client.listTools(cursor === undefined ? undefined : { cursor });
      for (const tool of page.tools) {
        if (tools.length >= 1000) throw new Error("UNREAL_MCP_TOOL_LIMIT_EXCEEDED");
        tools.push(tool);
      }
      cursor = page.nextCursor;
      if (cursor === undefined || cursor === "") return tools;
    }
    throw new Error("UNREAL_MCP_TOOL_PAGE_LIMIT_EXCEEDED");
  }

  async function connectUpstream() {
    if (connection !== null) return connection;
    if (connecting !== null) return await connecting;

    connecting = (async () => {
      const client = new Client(
        { name: "contextforge-unreal-adapter", version: "1.0.0" },
        { capabilities: {} }
      );
      const transport = new StreamableHTTPClientTransport(endpoint);
      try {
        await client.connect(transport);
        const tools = await listAllTools(client);
        connection = { client, transport, tools };
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

  async function withUpstream(operation) {
    const reusedExistingConnection = connection !== null;
    try {
      return await operation(await connectUpstream());
    } catch (error) {
      if (isConnectionFailure(error)) {
        await resetUpstream();
        if (reusedExistingConnection) {
          return await operation(await connectUpstream());
        }
      }
      throw error;
    }
  }

  async function refreshTools(state) {
    state.tools = await listAllTools(state.client);
    return state.tools;
  }

  server.setRequestHandler(ListToolsRequestSchema, async () => ({
    tools: STABLE_TOOLS
  }));

  server.setRequestHandler(CallToolRequestSchema, async (request) => {
    const args = request.params.arguments ?? {};
    try {
      return await withUpstream(async (state) => {
        const directMetaTool = state.tools.some((tool) => tool.name === request.params.name);
        if (directMetaTool) {
          return await state.client.callTool({
            name: request.params.name,
            arguments: args
          });
        }

        switch (request.params.name) {
          case LIST_TOOLSETS:
            return textResult(summarizeToolsets(state.tools));

          case DESCRIBE_TOOLSET: {
            const requested = requiredString(args.toolset_name);
            if (requested === null) return toolError("Missing required parameter: toolset_name");
            return describeToolset(state.tools, requested);
          }

          case CALL_TOOL: {
            const toolName = requiredString(args.tool_name);
            if (toolName === null) return toolError("Missing required parameter: tool_name");
            const requestedToolset = optionalString(args.toolset_name);
            const fullName =
              requestedToolset === null ? toolName : `${requestedToolset}.${toolName}`;

            if (!state.tools.some((tool) => tool.name === fullName)) {
              await refreshTools(state);
            }
            if (!state.tools.some((tool) => tool.name === fullName)) {
              return toolError(`Unreal MCP tool '${fullName}' is not currently available.`);
            }

            return await state.client.callTool({
              name: fullName,
              arguments: isRecord(args.arguments) ? args.arguments : {}
            });
          }

          default:
            return toolError(`Unsupported ContextForge Unreal tool: ${request.params.name}`);
        }
      });
    } catch (error) {
      return unrealRuntimeError(error, endpoint);
    }
  });

  const shutdown = async () => {
    await resetUpstream();
  };
  process.once("SIGINT", shutdown);
  process.once("SIGTERM", shutdown);

  await server.connect(new StdioServerTransport());
}

export function describeToolset(tools, requested) {
  const prefix = `${requested}.`;
  const matching = tools
    .filter((tool) => tool.name.startsWith(prefix))
    .map((tool) => ({
      name: tool.name.slice(prefix.length),
      ...(tool.description === undefined ? {} : { description: tool.description }),
      inputSchema: tool.inputSchema,
      ...(tool.outputSchema === undefined ? {} : { outputSchema: tool.outputSchema })
    }));

  if (matching.length === 0) {
    return toolError(`Toolset '${requested}' is not currently available.`);
  }
  return textResult(JSON.stringify({ name: requested, tools: matching }));
}

function requiredString(value) {
  return typeof value === "string" && value.trim() !== "" ? value.trim() : null;
}

function optionalString(value) {
  return value === undefined || value === null || value === "" ? null : requiredString(value);
}

function isRecord(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function textResult(text) {
  return { content: [{ type: "text", text }] };
}

function toolError(text) {
  return { content: [{ type: "text", text }], isError: true };
}

function isConnectionFailure(error) {
  const diagnostic = errorDiagnostic(error);
  return /ECONNREFUSED|ECONNRESET|EPIPE|fetch failed|connect|socket|closed/i.test(diagnostic);
}

function unrealRuntimeError(error, endpoint) {
  const diagnostic = errorDiagnostic(error);
  if (isConnectionFailure(error)) {
    return toolError(
      [
        `Unreal MCP is unavailable at ${endpoint.origin}${endpoint.pathname}.`,
        "Open Unreal Engine 5.8, enable the Unreal MCP plugin, and enable Auto Start Server",
        "or run ModelContextProtocol.StartServer in the Editor console."
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
