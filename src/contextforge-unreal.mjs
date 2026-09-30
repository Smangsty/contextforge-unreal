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

export const ADAPTER_VERSION = "2.0.3";
export const DEFAULT_UNREAL_MCP_ENDPOINT = "http://127.0.0.1:8000/mcp";
export const NATIVE_TOOL_SEARCH_TOOLS = Object.freeze([
  "list_toolsets",
  "describe_toolset",
  "call_tool"
]);
export const CAPTURE_VIEWPORT_TOOL_NAME = "EditorToolset.EditorAppToolset.CaptureViewport";
export const NIAGARA_SET_STACK_INPUT_DATA_TOOL_NAME =
  "NiagaraToolsets.NiagaraToolset_System.SetStackInputData";
const CAPTURE_VIEWPORT_IMAGE_MARKER = "[emitted as MCP image/png]";

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

export function normalizeUnrealToolSchema(tool) {
  if (tool?.name !== NIAGARA_SET_STACK_INPUT_DATA_TOOL_NAME || !isRecord(tool?.inputSchema)) {
    return tool;
  }

  const rootProperties = tool.inputSchema.properties;
  if (!isRecord(rootProperties)) return tool;
  const inputData = rootProperties.inputData;
  if (!isRecord(inputData)) return tool;
  const inputDataProperties = inputData.properties;
  if (!isRecord(inputDataProperties)) return tool;
  const valueSchema = inputDataProperties.value;
  if (!isRecord(valueSchema) || !Array.isArray(valueSchema.oneOf) || valueSchema.anyOf !== undefined) {
    return tool;
  }

  const primitiveTypes = new Set(
    valueSchema.oneOf.map((branch) => {
      if (!isRecord(branch) || !isRecord(branch.properties)) return null;
      const branchValue = branch.properties.value;
      return isRecord(branchValue) && typeof branchValue.type === "string" ? branchValue.type : null;
    })
  );
  if (!primitiveTypes.has("number") || !primitiveTypes.has("integer")) return tool;

  const { oneOf, ...valueSchemaWithoutOneOf } = valueSchema;
  return {
    ...tool,
    inputSchema: {
      ...tool.inputSchema,
      properties: {
        ...rootProperties,
        inputData: {
          ...inputData,
          properties: {
            ...inputDataProperties,
            value: {
              ...valueSchemaWithoutOneOf,
              anyOf: oneOf
            }
          }
        }
      }
    }
  };
}

export function annotateUnrealTool(tool) {
  const normalizedTool = normalizeUnrealToolSchema(tool);
  const readOnly = isReviewedReadOnlyTool(normalizedTool?.name);
  return {
    ...normalizedTool,
    annotations: {
      ...(normalizedTool?.annotations ?? {}),
      readOnlyHint: readOnly,
      destructiveHint: !readOnly,
      idempotentHint: readOnly,
      openWorldHint: normalizedTool?.annotations?.openWorldHint === true
    }
  };
}

export function annotateUnrealTools(tools) {
  return Array.isArray(tools) ? tools.map(annotateUnrealTool) : [];
}

export function promoteCaptureViewportImage(toolName, result) {
  if (toolName !== CAPTURE_VIEWPORT_TOOL_NAME || !isRecord(result) || !Array.isArray(result.content)) {
    return result;
  }

  let imageData = null;
  if (isRecord(result.structuredContent) && isPngBase64(result.structuredContent.Image)) {
    imageData = result.structuredContent.Image;
  }
  if (imageData === null) {
    for (const item of result.content) {
      if (!isRecord(item) || item.type !== "text" || typeof item.text !== "string") continue;
      const parsed = parseJsonRecord(item.text);
      if (parsed !== null && isPngBase64(parsed.Image)) {
        imageData = parsed.Image;
        break;
      }
    }
  }
  if (imageData === null) return result;

  const content = result.content.map((item) => {
    if (!isRecord(item) || item.type !== "text" || typeof item.text !== "string") return item;
    const parsed = parseJsonRecord(item.text);
    if (parsed === null || parsed.Image !== imageData) return item;
    return { ...item, text: JSON.stringify({ ...parsed, Image: CAPTURE_VIEWPORT_IMAGE_MARKER }) };
  });
  if (
    !content.some(
      (item) =>
        isRecord(item) &&
        item.type === "image" &&
        item.mimeType === "image/png" &&
        item.data === imageData
    )
  ) {
    content.push({ type: "image", data: imageData, mimeType: "image/png" });
  }

  const structuredContent =
    isRecord(result.structuredContent) && result.structuredContent.Image === imageData
      ? { ...result.structuredContent, Image: CAPTURE_VIEWPORT_IMAGE_MARKER }
      : result.structuredContent;

  return {
    ...result,
    content,
    ...(structuredContent === undefined ? {} : { structuredContent })
  };
}

function isRecord(value) {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function parseJsonRecord(text) {
  try {
    const parsed = JSON.parse(text);
    return isRecord(parsed) ? parsed : null;
  } catch {
    return null;
  }
}

function isPngBase64(value) {
  return (
    typeof value === "string" &&
    value.length >= 16 &&
    value.length % 4 === 0 &&
    value.startsWith("iVBORw0KGgo") &&
    /^[A-Za-z0-9+/]+={0,2}$/.test(value)
  );
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
        const upstreamResult = await withUpstream(
          async ({ client }) =>
            await client.callTool({
              name: toolName,
              arguments: args
            }),
          { retryConnectionFailure: retrySafe }
        );
        return promoteCaptureViewportImage(toolName, upstreamResult);
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
