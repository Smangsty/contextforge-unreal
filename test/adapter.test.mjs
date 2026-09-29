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
import { REVIEWED_READ_ONLY_TOOLS } from "../src/read-only-tools.mjs";

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
  assert.equal(ADAPTER_VERSION, "2.0.1");
  assert.equal(isNativeToolSearchOnly(NATIVE_TOOL_SEARCH_TOOLS.map((name) => ({ name }))), true);
  assert.equal(isNativeToolSearchOnly([
    { name: "EditorToolset.EditorAppToolset.GetCameraTransform" },
    { name: "editor_toolset.toolsets.scene.SceneTools.get_current_level" }
  ]), false);
});

test("reviewed READ tools receive conservative read-only authority hints", () => {
  const names = [
    "EditorToolset.EditorAppToolset.GetCameraTransform",
    "aimodule_toolset.toolsets.behavior_tree.BehaviorTreeTools.get_blackboard",
    "animation_toolset.toolsets.sequencer.SequencerTools.get_current_sequence",
    "editor_toolset.toolsets.blueprint.BlueprintTools.find_nodes",
    "state_tree_toolset.toolsets.state_tree.StateTreeTools.get_tasks"
  ];
  for (const name of names) {
    assert.equal(isReviewedReadOnlyTool(name), true, name);
    assert.deepEqual(annotateUnrealTool({ name }).annotations, {
      readOnlyHint: true, destructiveHint: false, idempotentHint: true, openWorldHint: false
    });
  }
});

test("reviewed READ manifest is exact, sorted, and duplicate-free", () => {
  assert.equal(REVIEWED_READ_ONLY_TOOLS.length, 238);
  assert.deepEqual(REVIEWED_READ_ONLY_TOOLS, [...REVIEWED_READ_ONLY_TOOLS].sort());
  assert.equal(new Set(REVIEWED_READ_ONLY_TOOLS).size, REVIEWED_READ_ONLY_TOOLS.length);
});

test("ambiguous or mutating getter-shaped tools remain WRITE", () => {
  for (const name of [
    "animation_toolset.toolsets.controlrig_sequencer.SequencerControlRigTools.find_or_create_track",
    "animation_toolset.toolsets.outliner.SequencerOutlinerTools.get_sections_for_nodes",
    "animation_toolset.toolsets.sequencer.SequencerTools.get_bound_objects",
    "editor_toolset.toolsets.blueprint.BlueprintTools.get_node_type_pins"
  ]) {
    assert.equal(isReviewedReadOnlyTool(name), false, name);
    assert.equal(annotateUnrealTool({ name }).annotations.readOnlyHint, false, name);
  }
});

test("unknown tools default to WRITE even if upstream later claims readOnlyHint", () => {
  const tool = annotateUnrealTool({
    name: "Example.UnknownTool",
    annotations: { readOnlyHint: true, destructiveHint: false, idempotentHint: true, openWorldHint: true }
  });
  assert.deepEqual(tool.annotations, {
    readOnlyHint: false, destructiveHint: true, idempotentHint: false, openWorldHint: true
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
  const client = { setNotificationHandler(_schema, value) { handler = value; } };
  const server = { async sendToolListChanged() { sent += 1; } };
  installToolListChangedForwarder(client, server);
  assert.equal(typeof handler, "function");
  await handler({ method: "notifications/tools/list_changed" });
  assert.equal(sent, 1);
});
