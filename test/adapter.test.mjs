import assert from "node:assert/strict";
import test from "node:test";

import {
  DEFAULT_UNREAL_MCP_ENDPOINT,
  STABLE_TOOLS,
  parseLoopbackEndpoint,
  summarizeToolsets,
  toolsetName
} from "../bin/contextforge-unreal.mjs";

test("uses Epic's default local Unreal MCP endpoint", () => {
  assert.equal(parseLoopbackEndpoint().href, DEFAULT_UNREAL_MCP_ENDPOINT);
});

test("accepts loopback HTTP endpoints only", () => {
  assert.equal(parseLoopbackEndpoint("http://localhost:8000/mcp").hostname, "localhost");
  assert.throws(() => parseLoopbackEndpoint("https://127.0.0.1:8000/mcp"), /REFUSED/);
  assert.throws(() => parseLoopbackEndpoint("http://192.168.1.10:8000/mcp"), /REFUSED/);
  assert.throws(() => parseLoopbackEndpoint("http://user:pass@127.0.0.1:8000/mcp"), /REFUSED/);
});

test("keeps a stable three-tool ContextForge surface", () => {
  assert.deepEqual(
    STABLE_TOOLS.map((tool) => tool.name),
    ["list_toolsets", "describe_toolset", "call_tool"]
  );
});

test("groups full Unreal tool names by toolset", () => {
  assert.equal(toolsetName("actor.spawn"), "actor");
  assert.equal(toolsetName("plain_tool"), null);
  assert.match(
    summarizeToolsets([
      { name: "actor.spawn" },
      { name: "actor.delete" },
      { name: "material.create" }
    ]),
    /actor \(2 tools\)/
  );
});
