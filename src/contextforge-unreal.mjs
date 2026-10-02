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
import { createHash } from "node:crypto";
import { win32 as win32Path } from "node:path";
import { pathToFileURL } from "node:url";

import { REVIEWED_READ_ONLY_TOOLS } from "./read-only-tools.mjs";

export const ADAPTER_VERSION = "2.1.1";
export const DEFAULT_UNREAL_MCP_ENDPOINT = "http://127.0.0.1:8000/mcp";
export const NATIVE_TOOL_SEARCH_TOOLS = Object.freeze([
  "list_toolsets",
  "describe_toolset",
  "call_tool"
]);
export const CAPTURE_ASSET_IMAGE_TOOL_NAME = "EditorToolset.EditorAppToolset.CaptureAssetImage";
export const CAPTURE_EDITOR_IMAGE_TOOL_NAME = "EditorToolset.EditorAppToolset.CaptureEditorImage";
export const CAPTURE_VIEWPORT_TOOL_NAME = "EditorToolset.EditorAppToolset.CaptureViewport";
export const IMAGE_CAPTURE_TOOL_NAMES = Object.freeze([
  CAPTURE_ASSET_IMAGE_TOOL_NAME,
  CAPTURE_EDITOR_IMAGE_TOOL_NAME,
  CAPTURE_VIEWPORT_TOOL_NAME
]);
export const NIAGARA_SET_STACK_INPUT_DATA_TOOL_NAME =
  "NiagaraToolsets.NiagaraToolset_System.SetStackInputData";
const CAPTURE_IMAGE_DATA_MARKER = "[emitted as MCP image/png]";
export const DEFAULT_UNREAL_MCP_PORT_START = 8000;
export const DEFAULT_UNREAL_MCP_PORT_END = 8015;
export const MAX_UNREAL_MCP_DISCOVERY_PORTS = 64;
export const CONTEXTFORGE_UNREAL_ROUTE_ARGUMENT = "_contextforgeUnreal";
export const CONTEXTFORGE_UNREAL_LIST_EDITORS_TOOL = "ContextForgeUnreal.ListEditors";
export const UNREAL_PROJECT_IDENTITY_TOOL = "EditorToolset.LogsToolset.GetLogEntries";
export const UNREAL_PROJECT_IDENTITY_ARGUMENTS = Object.freeze({
  pattern: "^LogCsvProfiler: Display: Metadata set : commandline=.*uproject",
  category: "",
  maxEntries: 10
});

const CONTEXTFORGE_UNREAL_LIST_EDITORS_TOOL_DEFINITION = Object.freeze({
  name: CONTEXTFORGE_UNREAL_LIST_EDITORS_TOOL,
  description:
    "List running Unreal Editor MCP instances, their authoritative .uproject identity, and loopback transport port.",
  inputSchema: Object.freeze({
    type: "object",
    properties: Object.freeze({}),
    additionalProperties: false
  }),
  annotations: Object.freeze({
    readOnlyHint: true,
    destructiveHint: false,
    idempotentHint: true,
    openWorldHint: false
  })
});

function parsePort(value) {
  const parsed =
    typeof value === "number"
      ? value
      : typeof value === "string" && value.trim() !== ""
        ? Number(value)
        : Number.NaN;
  if (!Number.isInteger(parsed) || parsed < 1 || parsed > 65535) {
    throw new Error("UNREAL_MCP_PORT_RANGE_INVALID");
  }
  return parsed;
}

export function parseUnrealPortRange(
  startValue = DEFAULT_UNREAL_MCP_PORT_START,
  endValue = DEFAULT_UNREAL_MCP_PORT_END
) {
  const start = parsePort(startValue);
  const end = parsePort(endValue);
  if (end < start || end - start + 1 > MAX_UNREAL_MCP_DISCOVERY_PORTS) {
    throw new Error("UNREAL_MCP_PORT_RANGE_INVALID");
  }
  return Object.freeze({ start, end });
}

export function unrealEndpointForPort(portValue) {
  const port = parsePort(portValue);
  return new URL(`http://127.0.0.1:${port}/mcp`);
}

function normalizeWindowsAbsolutePath(value) {
  if (typeof value !== "string") return null;
  const trimmed = value.trim().replace(/^"+|"+$/g, "");
  if (!/^[A-Za-z]:[\\/]/.test(trimmed)) return null;
  if (trimmed.includes(String.fromCharCode(0)) || /[\r\n]/.test(trimmed)) return null;
  return win32Path.normalize(trimmed).replace(/[\\/]+$/g, "");
}

export function normalizeUnrealProjectPath(value) {
  const normalized = normalizeWindowsAbsolutePath(value);
  return normalized !== null && normalized.toLowerCase().endsWith(".uproject")
    ? normalized
    : null;
}

export function normalizeContextProjectRoot(value) {
  if (value === null || value === undefined || value === "") return null;
  return normalizeWindowsAbsolutePath(value);
}

function pathKey(value) {
  return value.toLowerCase();
}

export function projectPathWithinRoot(projectPathValue, projectRootValue) {
  const projectPath = normalizeUnrealProjectPath(projectPathValue);
  const projectRoot = normalizeContextProjectRoot(projectRootValue);
  if (projectPath === null) return false;
  if (projectRoot === null) return true;

  const projectKey = pathKey(projectPath);
  const rootKey = pathKey(projectRoot);
  if (rootKey.endsWith(".uproject")) return projectKey === rootKey;
  return projectKey.startsWith(`${rootKey}\\`);
}

export function projectPathMatchesRootExactly(projectPathValue, projectRootValue) {
  const projectPath = normalizeUnrealProjectPath(projectPathValue);
  const projectRoot = normalizeContextProjectRoot(projectRootValue);
  if (projectPath === null || projectRoot === null) return false;

  if (projectRoot.toLowerCase().endsWith(".uproject")) {
    return pathKey(projectPath) === pathKey(projectRoot);
  }
  return pathKey(win32Path.dirname(projectPath)) === pathKey(projectRoot);
}

function collectStrings(value, output, depth = 0) {
  if (depth > 10) return;
  if (typeof value === "string") {
    const trimmed = value.trim();
    if (
      trimmed.length > 1 &&
      trimmed.length < 1_000_000 &&
      (trimmed.startsWith("{") || trimmed.startsWith("["))
    ) {
      try {
        collectStrings(JSON.parse(trimmed), output, depth + 1);
        return;
      } catch {
        // Ordinary log text is expected too.
      }
    }
    output.push(value);
    return;
  }
  if (Array.isArray(value)) {
    for (const item of value) collectStrings(item, output, depth + 1);
    return;
  }
  if (isRecord(value)) {
    for (const item of Object.values(value)) collectStrings(item, output, depth + 1);
  }
}

export function extractUnrealProjectPath(result) {
  const strings = [];
  collectStrings(result, strings);

  const candidates = new Map();
  for (const value of strings) {
    if (!/Metadata set\s*:\s*commandline=/i.test(value)) continue;
    const matches = value.match(/[A-Za-z]:[^"\r\n]*?\.uproject\b/gi) ?? [];
    for (const match of matches) {
      const normalized = normalizeUnrealProjectPath(match);
      if (normalized !== null) candidates.set(pathKey(normalized), normalized);
    }
  }

  return candidates.size === 1 ? [...candidates.values()][0] : null;
}

const ROUTING_SCHEMA = Object.freeze({
  type: "object",
  description:
    "Optional ContextForge Unreal routing metadata. projectPath is authoritative; port is only a secondary disambiguator. This object is removed before forwarding to Unreal.",
  properties: Object.freeze({
    projectPath: Object.freeze({
      type: "string",
      description:
        "Absolute .uproject path reported by ContextForgeUnreal.ListEditors. Required whenever routing metadata is supplied."
    }),
    port: Object.freeze({
      type: "integer",
      minimum: 1,
      maximum: 65535,
      description:
        "Optional loopback MCP port used only to disambiguate duplicate instances of the same .uproject."
    })
  }),
  required: Object.freeze(["projectPath"]),
  additionalProperties: false
});

export function addRoutingTargetToTool(tool) {
  if (!isRecord(tool) || !isRecord(tool.inputSchema)) return tool;
  const schema = tool.inputSchema;
  if (schema.type !== undefined && schema.type !== "object") return tool;

  const properties = isRecord(schema.properties) ? schema.properties : {};
  if (Object.prototype.hasOwnProperty.call(properties, CONTEXTFORGE_UNREAL_ROUTE_ARGUMENT)) {
    throw new Error("UNREAL_MCP_ROUTING_ARGUMENT_COLLISION");
  }

  return {
    ...tool,
    inputSchema: {
      ...schema,
      type: schema.type ?? "object",
      properties: {
        ...properties,
        [CONTEXTFORGE_UNREAL_ROUTE_ARGUMENT]: ROUTING_SCHEMA
      }
    }
  };
}

export function splitUnrealRoutingArguments(value) {
  const source = isRecord(value) ? value : {};
  if (!Object.prototype.hasOwnProperty.call(source, CONTEXTFORGE_UNREAL_ROUTE_ARGUMENT)) {
    return Object.freeze({ arguments: source, target: null });
  }

  const rawTarget = source[CONTEXTFORGE_UNREAL_ROUTE_ARGUMENT];
  if (!isRecord(rawTarget)) throw new Error("UNREAL_TARGET_INVALID");
  if (Object.keys(rawTarget).some((key) => key !== "projectPath" && key !== "port")) {
    throw new Error("UNREAL_TARGET_INVALID");
  }

  const projectPath = normalizeUnrealProjectPath(rawTarget.projectPath);
  if (projectPath === null) throw new Error("UNREAL_TARGET_INVALID");

  let port = null;
  if (rawTarget.port !== undefined) {
    try {
      port = parsePort(rawTarget.port);
    } catch {
      throw new Error("UNREAL_TARGET_INVALID");
    }
  }

  const forwarded = { ...source };
  delete forwarded[CONTEXTFORGE_UNREAL_ROUTE_ARGUMENT];
  return Object.freeze({
    arguments: forwarded,
    target: Object.freeze({ projectPath, port })
  });
}

export function selectUnrealRoute(routesValue, target = null, projectRootValue = null) {
  const routes = Array.isArray(routesValue) ? routesValue : [];
  const projectRoot = normalizeContextProjectRoot(projectRootValue);
  if (
    projectRootValue !== null &&
    projectRootValue !== undefined &&
    projectRootValue !== "" &&
    projectRoot === null
  ) {
    throw new Error("UNREAL_PROJECT_ROOT_INVALID");
  }

  const eligible =
    projectRoot === null
      ? routes
      : routes.filter(
          (route) =>
            typeof route?.projectPath === "string" &&
            projectPathWithinRoot(route.projectPath, projectRoot)
        );

  if (target !== null) {
    const normalizedTarget = normalizeUnrealProjectPath(target.projectPath);
    if (normalizedTarget === null) throw new Error("UNREAL_TARGET_INVALID");

    let matches = eligible.filter(
      (route) =>
        typeof route?.projectPath === "string" &&
        normalizeUnrealProjectPath(route.projectPath)?.toLowerCase() ===
          normalizedTarget.toLowerCase()
    );

    if (target.port !== null && target.port !== undefined) {
      const targetPort = parsePort(target.port);
      matches = matches.filter((route) => route?.port === targetPort);
    }

    if (matches.length === 0) throw new Error("UNREAL_TARGET_UNAVAILABLE");
    if (matches.length > 1) throw new Error("UNREAL_TARGET_AMBIGUOUS");
    return matches[0];
  }

  if (projectRoot !== null) {
    const exact = eligible.filter(
      (route) =>
        typeof route?.projectPath === "string" &&
        projectPathMatchesRootExactly(route.projectPath, projectRoot)
    );
    if (exact.length === 1) return exact[0];
    if (exact.length > 1) throw new Error("UNREAL_TARGET_AMBIGUOUS");
  }

  if (eligible.length === 0) throw new Error("UNREAL_TARGET_UNAVAILABLE");
  if (eligible.length > 1) throw new Error("UNREAL_TARGET_REQUIRED");
  return eligible[0];
}

const REVIEWED_READ_ONLY_TOOL_SET = new Set(REVIEWED_READ_ONLY_TOOLS);
const IMAGE_CAPTURE_TOOL_SET = new Set(IMAGE_CAPTURE_TOOL_NAMES);

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

export function normalizeUnrealToolArguments(toolName, args) {
  const source = isRecord(args) ? args : {};
  if (toolName !== CAPTURE_VIEWPORT_TOOL_NAME) return source;

  const normalized = { ...source };
  if (!Object.prototype.hasOwnProperty.call(normalized, "captureTransform")) {
    normalized.captureTransform = null;
  }
  if (!Object.prototype.hasOwnProperty.call(normalized, "annotations")) {
    normalized.annotations = null;
  }
  return normalized;
}

function captureImageMatch(toolName, payload) {
  if (!isRecord(payload)) return null;

  if (toolName === CAPTURE_VIEWPORT_TOOL_NAME) {
    const returnValue = payload.returnValue;
    if (isRecord(returnValue) && isToolsetImage(returnValue.image)) {
      return {
        data: returnValue.image.data,
        mimeType: returnValue.image.mimeType,
        sanitized: {
          ...payload,
          returnValue: {
            ...returnValue,
            image: { ...returnValue.image, data: CAPTURE_IMAGE_DATA_MARKER }
          }
        }
      };
    }

    // Preserve compatibility with UE builds that emitted the older top-level Image field.
    if (isPngBase64(payload.Image)) {
      return {
        data: payload.Image,
        mimeType: "image/png",
        sanitized: { ...payload, Image: CAPTURE_IMAGE_DATA_MARKER }
      };
    }
  }

  if (
    (toolName === CAPTURE_EDITOR_IMAGE_TOOL_NAME || toolName === CAPTURE_ASSET_IMAGE_TOOL_NAME) &&
    isToolsetImage(payload.returnValue)
  ) {
    return {
      data: payload.returnValue.data,
      mimeType: payload.returnValue.mimeType,
      sanitized: {
        ...payload,
        returnValue: { ...payload.returnValue, data: CAPTURE_IMAGE_DATA_MARKER }
      }
    };
  }

  return null;
}

export function promoteUnrealImageResult(toolName, result) {
  if (
    !IMAGE_CAPTURE_TOOL_SET.has(toolName) ||
    !isRecord(result) ||
    !Array.isArray(result.content) ||
    result.isError === true
  ) {
    return result;
  }

  let image = null;
  let structuredContent = result.structuredContent;
  let sanitizedTextPayload = null;

  if (isRecord(structuredContent)) {
    const match = captureImageMatch(toolName, structuredContent);
    if (match !== null) {
      image = { data: match.data, mimeType: match.mimeType };
      structuredContent = match.sanitized;
    }
  }

  const content = result.content.map((item) => {
    if (!isRecord(item) || item.type !== "text" || typeof item.text !== "string") return item;
    const parsed = parseJsonRecord(item.text);
    if (parsed === null) return item;

    const match = captureImageMatch(toolName, parsed);
    if (match === null) return item;
    if (image !== null && (image.data !== match.data || image.mimeType !== match.mimeType)) {
      return item;
    }

    image ??= { data: match.data, mimeType: match.mimeType };
    sanitizedTextPayload ??= match.sanitized;
    return { ...item, text: JSON.stringify(match.sanitized) };
  });

  if (image === null) return result;

  if (
    !content.some(
      (item) =>
        isRecord(item) &&
        item.type === "image" &&
        item.mimeType === image.mimeType &&
        item.data === image.data
    )
  ) {
    content.push({ type: "image", data: image.data, mimeType: image.mimeType });
  }

  if (structuredContent === undefined && sanitizedTextPayload !== null) {
    structuredContent = sanitizedTextPayload;
  }

  return {
    ...result,
    content,
    ...(structuredContent === undefined ? {} : { structuredContent })
  };
}

export function promoteCaptureViewportImage(toolName, result) {
  return promoteUnrealImageResult(toolName, result);
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

function isToolsetImage(value) {
  return (
    isRecord(value) &&
    value.mimeType === "image/png" &&
    isPngBase64(value.data)
  );
}

export function installToolListChangedForwarder(client, server) {
  client.setNotificationHandler(ToolListChangedNotificationSchema, async () => {
    await server.sendToolListChanged();
  });
}

export function fingerprintUnrealTools(tools) {
  const canonicalTools = [...(tools ?? [])].sort((left, right) =>
    String(left?.name ?? "").localeCompare(String(right?.name ?? ""))
  );
  return createHash("sha256").update(JSON.stringify(canonicalTools)).digest("hex");
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
    const args = normalizeUnrealToolArguments(toolName, request.params.arguments ?? {});
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
        return promoteUnrealImageResult(toolName, upstreamResult);
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

export async function startContextForgeUnrealMultiplexed({
  endpoint = process.env.UNREAL_MCP_ENDPOINT ?? null,
  projectRoot = process.env.CONTEXTFORGE_UNREAL_PROJECT_ROOT ?? null,
  portStart = process.env.CONTEXTFORGE_UNREAL_PORT_START ?? DEFAULT_UNREAL_MCP_PORT_START,
  portEnd = process.env.CONTEXTFORGE_UNREAL_PORT_END ?? DEFAULT_UNREAL_MCP_PORT_END
} = {}) {
  const explicitEndpoint =
    endpoint === null
      ? null
      : parseLoopbackEndpoint(endpoint instanceof URL ? endpoint.href : endpoint);
  const normalizedProjectRoot = normalizeContextProjectRoot(projectRoot);
  if (projectRoot !== null && projectRoot !== "" && normalizedProjectRoot === null) {
    throw new Error("UNREAL_PROJECT_ROOT_INVALID");
  }
  const portRange =
    explicitEndpoint === null ? parseUnrealPortRange(portStart, portEnd) : null;

  const server = new Server(
    { name: "contextforge-unreal", version: ADAPTER_VERSION },
    { capabilities: { tools: { listChanged: true } } }
  );

  const connections = new Map();
  const connecting = new Map();
  let operationTail = Promise.resolve();
  let publishedCatalogFingerprint = null;

  function discoveryDescription() {
    if (explicitEndpoint !== null) return explicitEndpoint.href;
    return `127.0.0.1 ports ${portRange.start}-${portRange.end}`;
  }

  function candidateEndpoints() {
    if (explicitEndpoint !== null) return [explicitEndpoint];
    const endpoints = [];
    for (let port = portRange.start; port <= portRange.end; port += 1) {
      endpoints.push(unrealEndpointForPort(port));
    }
    return endpoints;
  }

  function fingerprintTools(tools) {
    return fingerprintUnrealTools(tools);
  }

  async function identifyProjectPath(client, tools) {
    if (!tools.some((tool) => tool?.name === UNREAL_PROJECT_IDENTITY_TOOL)) return null;
    try {
      const result = await client.callTool({
        name: UNREAL_PROJECT_IDENTITY_TOOL,
        arguments: UNREAL_PROJECT_IDENTITY_ARGUMENTS
      });
      if (result?.isError === true) return null;
      return extractUnrealProjectPath(result);
    } catch (error) {
      if (isConnectionFailure(error)) throw error;
      return null;
    }
  }

  async function resetEndpoint(endpointUrl) {
    const key = endpointUrl.href;
    const current = connections.get(key);
    connections.delete(key);
    if (current !== undefined) {
      await current.client.close().catch(() => undefined);
    }
  }

  async function connectEndpoint(endpointUrl) {
    const key = endpointUrl.href;
    const existing = connections.get(key);
    if (existing !== undefined) {
      try {
        existing.projectPath = await identifyProjectPath(existing.client, existing.tools);
        return existing;
      } catch (error) {
        await resetEndpoint(endpointUrl);
        if (!isConnectionFailure(error)) throw error;
      }
    }

    const pending = connecting.get(key);
    if (pending !== undefined) return await pending;

    const attempt = (async () => {
      const client = new Client(
        { name: "contextforge-unreal-adapter", version: ADAPTER_VERSION },
        { capabilities: {} }
      );
      const transport = new StreamableHTTPClientTransport(endpointUrl);
      installToolListChangedForwarder(client, server);

      try {
        await client.connect(transport);
        const initialPage = await client.listTools();
        assertEagerTools(initialPage.tools);
        const state = {
          client,
          transport,
          endpoint: endpointUrl,
          port: Number(endpointUrl.port),
          projectPath: null,
          initialPage,
          tools: initialPage.tools,
          catalogFingerprint: fingerprintTools(initialPage.tools)
        };
        state.projectPath = await identifyProjectPath(client, state.tools);
        connections.set(key, state);
        return state;
      } catch (error) {
        await client.close().catch(() => undefined);
        throw error;
      }
    })();

    connecting.set(key, attempt);
    try {
      return await attempt;
    } finally {
      connecting.delete(key);
    }
  }

  async function discoverRoutes() {
    const attempts = await Promise.allSettled(candidateEndpoints().map(connectEndpoint));
    return attempts
      .filter((item) => item.status === "fulfilled")
      .map((item) => item.value)
      .sort((left, right) => left.port - right.port);
  }

  function eligibleRoutes(routes) {
    if (normalizedProjectRoot === null) return routes;
    return routes.filter(
      (route) =>
        route.projectPath !== null &&
        projectPathWithinRoot(route.projectPath, normalizedProjectRoot)
    );
  }

  function selectCatalogRoute(routes) {
    const eligible = eligibleRoutes(routes);
    if (eligible.length === 0) throw new Error("UNREAL_TARGET_UNAVAILABLE");

    if (normalizedProjectRoot !== null) {
      const exact = eligible.filter((route) =>
        projectPathMatchesRootExactly(route.projectPath, normalizedProjectRoot)
      );
      if (exact.length === 1) return exact[0];
    }

    return eligible[0];
  }

  async function assertPublishedCatalog(route) {
    if (
      publishedCatalogFingerprint === null ||
      route.catalogFingerprint === publishedCatalogFingerprint
    ) {
      return;
    }

    const page = await route.client.listTools();
    assertEagerTools(page.tools);
    route.initialPage = null;
    route.tools = page.tools;
    route.catalogFingerprint = fingerprintTools(page.tools);

    if (route.catalogFingerprint !== publishedCatalogFingerprint) {
      throw new Error("UNREAL_MCP_TOOL_CATALOG_MISMATCH");
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

  function routingError(error, toolName, route = null, retrySafe = false) {
    const diagnostic = errorDiagnostic(error);
    if (diagnostic.includes("UNREAL_TARGET_REQUIRED")) {
      return toolError(
        "Multiple Unreal Editors are available. Call ContextForgeUnreal.ListEditors, then pass _contextforgeUnreal.projectPath on the Unreal tool call."
      );
    }
    if (diagnostic.includes("UNREAL_TARGET_AMBIGUOUS")) {
      return toolError(
        "More than one Unreal Editor matches that .uproject. Add _contextforgeUnreal.port from ContextForgeUnreal.ListEditors to disambiguate the duplicate instance."
      );
    }
    if (diagnostic.includes("UNREAL_TARGET_UNAVAILABLE")) {
      return toolError(
        "The requested Unreal project is not currently available in the configured loopback discovery range."
      );
    }
    if (
      diagnostic.includes("UNREAL_TARGET_INVALID") ||
      diagnostic.includes("UNREAL_PROJECT_ROOT_INVALID")
    ) {
      return toolError("ContextForge Unreal routing metadata is invalid.");
    }
    if (diagnostic.includes("UNREAL_MCP_TOOL_CATALOG_MISMATCH")) {
      return toolError(
        "The target Unreal Editor exposes a different MCP tool catalog than the catalog admitted by ContextForge. Re-inspect the Skill before targeting that Editor."
      );
    }
    if (route !== null) {
      return unrealRuntimeError(error, route.endpoint, toolName, retrySafe);
    }
    return toolError(
      `Unreal MCP could not complete ${toolName}. ${diagnostic || "No eligible Editor is available."}`
    );
  }

  server.setRequestHandler(ListToolsRequestSchema, async (request) => {
    try {
      const routes = await discoverRoutes();
      const route = selectCatalogRoute(routes);
      const cursor = request.params?.cursor;
      let page;
      if (cursor === undefined && route.initialPage !== null) {
        page = route.initialPage;
        route.initialPage = null;
      } else {
        page = await route.client.listTools(
          cursor === undefined ? undefined : { cursor }
        );
        assertEagerTools(page.tools);
      }

      if (cursor === undefined) {
        route.tools = page.tools;
        route.catalogFingerprint = fingerprintTools(page.tools);
        publishedCatalogFingerprint = route.catalogFingerprint;
      }

      const tools = annotateUnrealTools(page.tools).map(addRoutingTargetToTool);
      if (cursor === undefined) tools.push(CONTEXTFORGE_UNREAL_LIST_EDITORS_TOOL_DEFINITION);
      return { ...page, tools };
    } catch (error) {
      const diagnostic = errorDiagnostic(error);
      if (
        diagnostic.includes("UNREAL_TARGET_UNAVAILABLE") ||
        isConnectionFailure(error)
      ) {
        throw new Error(
          `Unreal MCP is unavailable on ${discoveryDescription()}. Open Unreal Engine 5.8 and start the Model Context Protocol server.`
        );
      }
      throw new Error(`Unreal MCP could not list tools. ${diagnostic || "Verify the Editor MCP server is running."}`);
    }
  });

  server.setRequestHandler(CallToolRequestSchema, async (request) => {
    const toolName = request.params.name;

    if (toolName === CONTEXTFORGE_UNREAL_LIST_EDITORS_TOOL) {
      return await serialize(async () => {
        const routes = await discoverRoutes();
        const payload = {
          projectRoot: normalizedProjectRoot,
          discovery:
            explicitEndpoint === null
              ? { host: "127.0.0.1", portStart: portRange.start, portEnd: portRange.end }
              : { endpoint: explicitEndpoint.href },
          editors: routes.map((route) => ({
            projectPath: route.projectPath,
            port: route.port,
            endpoint: route.endpoint.href,
            eligibleForProjectRoot:
              normalizedProjectRoot === null ||
              (route.projectPath !== null &&
                projectPathWithinRoot(route.projectPath, normalizedProjectRoot))
          }))
        };
        return {
          content: [{ type: "text", text: JSON.stringify(payload, null, 2) }],
          structuredContent: payload
        };
      });
    }

    let split;
    try {
      split = splitUnrealRoutingArguments(request.params.arguments ?? {});
    } catch (error) {
      return routingError(error, toolName);
    }

    const args = normalizeUnrealToolArguments(toolName, split.arguments);
    const retrySafe = isReviewedReadOnlyTool(toolName);

    return await serialize(async () => {
      let route = null;
      try {
        const routes = await discoverRoutes();
        route = selectUnrealRoute(routes, split.target, normalizedProjectRoot);
        await assertPublishedCatalog(route);
        const upstreamResult = await route.client.callTool({
          name: toolName,
          arguments: args
        });
        return promoteUnrealImageResult(toolName, upstreamResult);
      } catch (error) {
        if (!isConnectionFailure(error) || route === null) {
          return routingError(error, toolName, route, retrySafe);
        }

        await resetEndpoint(route.endpoint);
        if (!retrySafe) {
          return unrealRuntimeError(error, route.endpoint, toolName, false);
        }

        const retryTarget =
          split.target ??
          (route.projectPath === null
            ? null
            : { projectPath: route.projectPath, port: null });
        if (retryTarget === null) {
          return routingError(error, toolName, route, true);
        }

        try {
          const retryRoutes = await discoverRoutes();
          const retryRoute = selectUnrealRoute(
            retryRoutes,
            retryTarget,
            normalizedProjectRoot
          );
          await assertPublishedCatalog(retryRoute);
          const upstreamResult = await retryRoute.client.callTool({
            name: toolName,
            arguments: args
          });
          return promoteUnrealImageResult(toolName, upstreamResult);
        } catch (retryError) {
          return routingError(retryError, toolName, route, true);
        }
      }
    });
  });

  const shutdown = async () => {
    const current = [...connections.values()];
    connections.clear();
    await Promise.allSettled(current.map((state) => state.client.close()));
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
    await startContextForgeUnrealMultiplexed();
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
