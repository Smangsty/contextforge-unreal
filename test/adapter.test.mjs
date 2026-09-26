import assert from "node:assert/strict";
import test from "node:test";

import {
  ADAPTER_VERSION,
  DEFAULT_UNREAL_MCP_ENDPOINT,
  NATIVE_TOOL_SEARCH_TOOLS,
  STABLE_TOOLS,
  hasNativeToolSearch,
  isRetrySafeToolSearchRequest,
  missingNativeToolSearchTools,
  parseLoopbackEndpoint
} from "../bin/contextforge-unreal.mjs";

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

test("keeps the native three-tool Unreal Tool Search surface", () => {
  assert.equal(ADAPTER_VERSION, "1.1.0");
  assert.deepEqual(STABLE_TOOLS.map((tool) => tool.name), NATIVE_TOOL_SEARCH_TOOLS);
  assert.deepEqual(NATIVE_TOOL_SEARCH_TOOLS, ["list_toolsets", "describe_toolset", "call_tool"]);
});

test("requires Unreal's native Tool Search surface", () => {
  const native = NATIVE_TOOL_SEARCH_TOOLS.map((name) => ({ name }));
  assert.equal(hasNativeToolSearch(native), true);
  assert.deepEqual(missingNativeToolSearchTools(native), []);

  const eager = [{ name: "actor.spawn" }, { name: "material.create" }];
  assert.equal(hasNativeToolSearch(eager), false);
  assert.deepEqual(missingNativeToolSearchTools(eager), NATIVE_TOOL_SEARCH_TOOLS);

  assert.deepEqual(
    missingNativeToolSearchTools([{ name: "list_toolsets" }, { name: "call_tool" }]),
    ["describe_toolset"]
  );
});

test("never auto-retries dispatched Unreal tools", () => {
  assert.equal(isRetrySafeToolSearchRequest("list_toolsets"), true);
  assert.equal(isRetrySafeToolSearchRequest("describe_toolset"), true);
  assert.equal(isRetrySafeToolSearchRequest("call_tool"), false);
});
