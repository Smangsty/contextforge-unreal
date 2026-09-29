import assert from "node:assert/strict";
import test from "node:test";

import {
  ADAPTER_VERSION,
  DEFAULT_UNREAL_MCP_ENDPOINT,
  NATIVE_TOOL_SEARCH_TOOLS,
  annotateUnrealTool,
  annotateUnrealTools,
  installToolListChangedForwarder,
  isNativeToolSearchOnly,
  isReviewedReadOnlyTool,
  parseLoopbackEndpoint
} from "../src/contextforge-unreal.mjs";

test("uses Epic's default local Unreal MCP endpoint", () => {
  assert.equal(parseLoopbackEndpoint().href, DEFAULT_UNREAL_MCP_ENDPOINT);
});

test("accepts loopback HTTP endpoints only", () => {
  assert.equal(parseLoopbackEndpoint("http://localhost:8000/mcp").hostname, "localhost");
  assert.throws(() => parseLoopbackEndpoint("https://127.0.0.1:8000/mcp"), /REFUSED/);
  assert.throws(() => parseLoopbackEndpoint("http://192.168.1.10:8000/mcp"), /REFUSED/);
  assert.throws(() => parseLoopbackEndpoint("http://user:pass@127.0.0.1:8000/mcp"), /REFUSED/);
  assert.throws(() => parseLoopbackEndpoint("http://127.0.0.1:8000/mcp?x=1"), /REFUSED/);
  assert.throws(() => parseLoopbackEndpoint("http://127.0.0.1:8000/mcp#x"), /REFUSED/);
});

test("v2 requires Unreal eager tools rather than the three Tool Search meta-tools", () => {
  assert.equal(ADAPTER_VERSION, "2.0.0");
  assert.equal(
    isNativeToolSearchOnly(NATIVE_TOOL_SEARCH_TOOLS.map((name) => ({ name }))),
    true
  );
  assert.equal(
    isNativeToolSearchOnly([
      { name: "EditorToolset.EditorAppToolset.GetCameraTransform" },
      { name: "editor_toolset.toolsets.scene.SceneTools.get_current_level" }
    ]),
    false
  );
});

test("reviewed READ tools receive conservative read-only authority hints", () => {
  const name = "EditorToolset.EditorAppToolset.GetCameraTransform";
  assert.equal(isReviewedReadOnlyTool(name), true);
  assert.deepEqual(annotateUnrealTool({ name, inputSchema: { type: "object" } }).annotations, {
    readOnlyHint: true,
    destructiveHint: false,
    idempotentHint: true,
    openWorldHint: false
  });
});

test("unknown tools default to WRITE even if upstream later claims readOnlyHint", () => {
  const tool = annotateUnrealTool({
    name: "Example.UnknownTool",
    annotations: {
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: true
    }
  });
  assert.deepEqual(tool.annotations, {
    readOnlyHint: false,
    destructiveHint: true,
    idempotentHint: false,
    openWorldHint: true
  });
});

test("annotation overlay preserves Unreal schemas and names", () => {
  const source = {
    name: "editor_toolset.toolsets.scene.SceneTools.get_current_level",
    description: "Returns current level",
    inputSchema: { type: "object", properties: {} },
    outputSchema: { type: "object" }
  };
  const [tool] = annotateUnrealTools([source]);
  assert.equal(tool.name, source.name);
  assert.equal(tool.description, source.description);
  assert.deepEqual(tool.inputSchema, source.inputSchema);
  assert.deepEqual(tool.outputSchema, source.outputSchema);
  assert.equal(tool.annotations.readOnlyHint, true);
});

test("forwards Unreal tools/list_changed notifications downstream", async () => {
  let handler = null;
  let sent = 0;
  const client = {
    setNotificationHandler(_schema, value) {
      handler = value;
    }
  };
  const server = {
    async sendToolListChanged() {
      sent += 1;
    }
  };

  installToolListChangedForwarder(client, server);
  assert.equal(typeof handler, "function");
  await handler({ method: "notifications/tools/list_changed" });
  assert.equal(sent, 1);
});
