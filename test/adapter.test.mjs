import assert from "node:assert/strict";
import test from "node:test";

import {
  ADAPTER_VERSION,
  CAPTURE_ASSET_IMAGE_TOOL_NAME,
  CAPTURE_EDITOR_IMAGE_TOOL_NAME,
  CAPTURE_VIEWPORT_TOOL_NAME,
  CONTEXTFORGE_UNREAL_ROUTE_ARGUMENT,
  DEFAULT_UNREAL_MCP_ENDPOINT,
  DEFAULT_UNREAL_MCP_PORT_END,
  DEFAULT_UNREAL_MCP_PORT_START,
  NATIVE_TOOL_SEARCH_TOOLS,
  NIAGARA_SET_STACK_INPUT_DATA_TOOL_NAME,
  addRoutingTargetToTool,
  annotateUnrealTool,
  annotateUnrealTools,
  extractUnrealProjectPath,
  fingerprintUnrealTools,
  installToolListChangedForwarder,
  isNativeToolSearchOnly,
  isReviewedReadOnlyTool,
  normalizeContextProjectRoot,
  normalizeUnrealProjectPath,
  normalizeUnrealToolArguments,
  normalizeUnrealToolSchema,
  parseLoopbackEndpoint,
  parseUnrealPortRange,
  projectPathWithinRoot,
  promoteCaptureViewportImage,
  promoteUnrealImageResult,
  selectUnrealRoute,
  splitUnrealRoutingArguments,
  unrealEndpointForPort
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
  assert.match(ADAPTER_VERSION, /^2\./);
  assert.equal(isNativeToolSearchOnly(NATIVE_TOOL_SEARCH_TOOLS.map((name) => ({ name }))), true);
  assert.equal(isNativeToolSearchOnly([
    { name: "EditorToolset.EditorAppToolset.GetCameraTransform" },
    { name: "editor_toolset.toolsets.scene.SceneTools.get_current_level" }
  ]), false);
});

test("reviewed READ tools receive conservative read-only authority hints", () => {
  const names = [
    CAPTURE_ASSET_IMAGE_TOOL_NAME,
    CAPTURE_EDITOR_IMAGE_TOOL_NAME,
    CAPTURE_VIEWPORT_TOOL_NAME,
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

test("field-audited material inspection tools are READ", () => {
  for (const name of [
    "editor_toolset.toolsets.material.MaterialTools.get_expression_inputs",
    "editor_toolset.toolsets.material.MaterialTools.get_property_input",
    "editor_toolset.toolsets.material.MaterialTools.list_expression_classes",
    "editor_toolset.toolsets.material.MaterialTools.list_parameter_groups",
    "editor_toolset.toolsets.material_instance.MaterialInstanceTools.list_parameters"
  ]) {
    assert.equal(isReviewedReadOnlyTool(name), true, name);
    assert.equal(annotateUnrealTool({ name }).annotations.readOnlyHint, true, name);
  }
});

test("reviewed READ manifest is exact, sorted, and duplicate-free", () => {
  assert.equal(REVIEWED_READ_ONLY_TOOLS.length, 246);
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

test("normalizes the known Niagara numeric union without weakening other schemas", () => {
  const floatBranch = {
    description: "float",
    properties: { value: { type: "number" } },
    required: ["value"],
    title: "/Script/Niagara.NiagaraFloat",
    type: "object"
  };
  const intBranch = {
    description: "int32",
    properties: { value: { type: "integer" } },
    required: ["value"],
    title: "/Script/Niagara.NiagaraInt32",
    type: "object"
  };
  const source = {
    name: NIAGARA_SET_STACK_INPUT_DATA_TOOL_NAME,
    inputSchema: {
      type: "object",
      properties: {
        inputData: {
          properties: {
            value: { oneOf: [floatBranch, intBranch] }
          }
        }
      }
    }
  };

  const normalized = normalizeUnrealToolSchema(source);
  const normalizedValue = normalized.inputSchema.properties.inputData.properties.value;
  assert.equal(normalizedValue.oneOf, undefined);
  assert.deepEqual(normalizedValue.anyOf, [floatBranch, intBranch]);
  assert.deepEqual(source.inputSchema.properties.inputData.properties.value.oneOf, [
    floatBranch,
    intBranch
  ]);
  assert.equal(source.inputSchema.properties.inputData.properties.value.anyOf, undefined);

  const unrelated = { ...source, name: "Example.OtherTool" };
  assert.equal(normalizeUnrealToolSchema(unrelated), unrelated);
  const nonOverlapping = {
    ...source,
    inputSchema: {
      ...source.inputSchema,
      properties: {
        inputData: {
          properties: {
            value: { oneOf: [floatBranch] }
          }
        }
      }
    }
  };
  assert.equal(normalizeUnrealToolSchema(nonOverlapping), nonOverlapping);
});

test("annotation overlay applies the Niagara compatibility schema before authority hints", () => {
  const source = {
    name: NIAGARA_SET_STACK_INPUT_DATA_TOOL_NAME,
    inputSchema: {
      type: "object",
      properties: {
        inputData: {
          properties: {
            value: {
              oneOf: [
                { properties: { value: { type: "number" } } },
                { properties: { value: { type: "integer" } } }
              ]
            }
          }
        }
      }
    }
  };
  const tool = annotateUnrealTool(source);
  assert.ok(Array.isArray(tool.inputSchema.properties.inputData.properties.value.anyOf));
  assert.equal(tool.inputSchema.properties.inputData.properties.value.oneOf, undefined);
  assert.equal(tool.annotations.readOnlyHint, false);
  assert.equal(tool.annotations.destructiveHint, true);
});

test("promotes CaptureViewport PNG payloads to native MCP image content", () => {
  const png =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Y9ZQmcAAAAASUVORK5CYII=";
  const source = {
    content: [{ type: "text", text: JSON.stringify({ Image: png, Width: 1 }) }],
    structuredContent: { Image: png, Width: 1 },
    isError: false
  };
  const promoted = promoteCaptureViewportImage(CAPTURE_VIEWPORT_TOOL_NAME, source);

  assert.equal(promoted.content.length, 2);
  assert.deepEqual(JSON.parse(promoted.content[0].text), {
    Image: "[emitted as MCP image/png]",
    Width: 1
  });
  assert.deepEqual(promoted.content[1], { type: "image", data: png, mimeType: "image/png" });
  assert.deepEqual(promoted.structuredContent, {
    Image: "[emitted as MCP image/png]",
    Width: 1
  });
  assert.equal(promoteCaptureViewportImage("Example.OtherTool", source), source);
});

test("repairs omitted CaptureViewport optionals without changing explicit values", () => {
  const transform = {
    translation: { x: 1, y: 2, z: 3 },
    rotation: { x: 0, y: 0, z: 0, w: 1 },
    scale: { x: 1, y: 1, z: 1 }
  };
  const annotations = {
    showGrid: true,
    showActorLabels: false,
    gridSpacing: 100,
    gridExtent: 1000,
    groundZ: 0
  };

  assert.deepEqual(normalizeUnrealToolArguments(CAPTURE_VIEWPORT_TOOL_NAME, {}), {
    captureTransform: null,
    annotations: null
  });
  assert.deepEqual(
    normalizeUnrealToolArguments(CAPTURE_VIEWPORT_TOOL_NAME, { captureTransform: transform }),
    { captureTransform: transform, annotations: null }
  );
  assert.deepEqual(
    normalizeUnrealToolArguments(CAPTURE_VIEWPORT_TOOL_NAME, { annotations }),
    { captureTransform: null, annotations }
  );
  assert.deepEqual(
    normalizeUnrealToolArguments(CAPTURE_VIEWPORT_TOOL_NAME, {
      captureTransform: transform,
      annotations,
      bShowUI: true
    }),
    { captureTransform: transform, annotations, bShowUI: true }
  );

  const assetArgs = { assetPath: "/Engine/BasicShapes/Cube.Cube" };
  assert.equal(normalizeUnrealToolArguments(CAPTURE_ASSET_IMAGE_TOOL_NAME, assetArgs), assetArgs);
});

test("promotes the UE 5.8 CaptureViewport wire shape and preserves metadata", () => {
  const png = Buffer.concat([
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    Buffer.alloc(900_000)
  ]).toString("base64");
  assert.ok(png.length > 1_000_000);

  const payload = {
    returnValue: {
      image: { mimeType: "image/png", data: png },
      cameraLocation: { x: 10, y: 20, z: 30 },
      cameraRotation: { pitch: 1, yaw: 2, roll: 3 },
      cameraFOV: 90,
      grid: null,
      labeledActors: []
    }
  };
  const source = {
    content: [{ type: "text", text: JSON.stringify(payload) }],
    isError: false
  };

  const promoted = promoteUnrealImageResult(CAPTURE_VIEWPORT_TOOL_NAME, source);
  const image = promoted.content.find((item) => item.type === "image");
  const text = promoted.content.find((item) => item.type === "text");
  assert.deepEqual(image, { type: "image", data: png, mimeType: "image/png" });
  assert.ok(text.text.length < 1024);
  assert.equal(text.text.includes(png), false);

  const sanitized = JSON.parse(text.text);
  assert.equal(sanitized.returnValue.image.data, "[emitted as MCP image/png]");
  assert.deepEqual(sanitized.returnValue.cameraLocation, { x: 10, y: 20, z: 30 });
  assert.deepEqual(sanitized.returnValue.cameraRotation, { pitch: 1, yaw: 2, roll: 3 });
  assert.equal(sanitized.returnValue.cameraFOV, 90);
  assert.deepEqual(promoted.structuredContent, sanitized);
});

test("promotes top-level FToolsetImage results for editor and asset capture only", () => {
  const png =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Y9ZQmcAAAAASUVORK5CYII=";
  for (const toolName of [CAPTURE_EDITOR_IMAGE_TOOL_NAME, CAPTURE_ASSET_IMAGE_TOOL_NAME]) {
    const payload = { returnValue: { mimeType: "image/png", data: png } };
    const source = { content: [{ type: "text", text: JSON.stringify(payload) }], isError: false };
    const promoted = promoteUnrealImageResult(toolName, source);
    assert.deepEqual(promoted.content[1], { type: "image", data: png, mimeType: "image/png" });
    assert.equal(promoted.content[0].text.includes(png), false);
    assert.deepEqual(promoted.structuredContent, {
      returnValue: { mimeType: "image/png", data: "[emitted as MCP image/png]" }
    });
  }

  const lookalike = {
    content: [{
      type: "text",
      text: JSON.stringify({ returnValue: { mimeType: "image/png", data: png } })
    }],
    isError: false
  };
  assert.equal(promoteUnrealImageResult("Example.UnrelatedTool", lookalike), lookalike);
});

test("catalog fingerprints ignore tool order but detect real schema drift", () => {
  const alpha = { name: "Alpha", inputSchema: { type: "object", properties: {} } };
  const beta = { name: "Beta", inputSchema: { type: "object", properties: { value: { type: "string" } } } };
  const changedBeta = { name: "Beta", inputSchema: { type: "object", properties: { value: { type: "number" } } } };

  assert.equal(fingerprintUnrealTools([alpha, beta]), fingerprintUnrealTools([beta, alpha]));
  assert.notEqual(
    fingerprintUnrealTools([alpha, beta]),
    fingerprintUnrealTools([alpha, changedBeta])
  );
});

test("uses a bounded configurable multi-editor discovery range", () => {
  assert.deepEqual(parseUnrealPortRange(), {
    start: DEFAULT_UNREAL_MCP_PORT_START,
    end: DEFAULT_UNREAL_MCP_PORT_END
  });
  assert.deepEqual(parseUnrealPortRange("8100", "8103"), { start: 8100, end: 8103 });
  assert.equal(unrealEndpointForPort(8001).href, "http://127.0.0.1:8001/mcp");
  assert.throws(() => parseUnrealPortRange(8100, 8099), /PORT_RANGE_INVALID/);
  assert.throws(() => parseUnrealPortRange(8000, 8064), /PORT_RANGE_INVALID/);
});

test("extracts the current uproject only from startup commandline metadata", () => {
  const pwf =
    "C:\\Unreal Projects\\Pirates with Friends\\Game_PiratesWithFriends\\PiratesWithFriends.uproject";
  const deathrey =
    "C:\\Unreal Projects\\Pirates with Friends\\Asset_Deathrey_58\\Pirate_Deathrey_58.uproject";
  const current =
    `LogCsvProfiler: Display: Metadata set : commandline="" "${pwf}""`;
  const result = {
    content: [
      { type: "text", text: JSON.stringify({ entries: [current, `RecentlyOpened=${deathrey}`] }) }
    ],
    structuredContent: { entries: [current] }
  };

  assert.equal(extractUnrealProjectPath(result), pwf);
  assert.equal(
    extractUnrealProjectPath({ content: [{ type: "text", text: `RecentlyOpened=${deathrey}` }] }),
    null
  );
  assert.equal(normalizeUnrealProjectPath(`"${pwf}"`), pwf);
});

test("routing metadata augments schemas and is stripped before Unreal dispatch", () => {
  const source = {
    name: "Example.Tool",
    inputSchema: {
      type: "object",
      properties: { value: { type: "number" } },
      required: ["value"],
      additionalProperties: false
    }
  };
  const routed = addRoutingTargetToTool(source);
  assert.equal(source.inputSchema.properties[CONTEXTFORGE_UNREAL_ROUTE_ARGUMENT], undefined);
  assert.equal(
    routed.inputSchema.properties[CONTEXTFORGE_UNREAL_ROUTE_ARGUMENT].required[0],
    "projectPath"
  );
  assert.deepEqual(routed.inputSchema.required, ["value"]);
  assert.equal(routed.inputSchema.additionalProperties, false);

  const projectPath =
    "C:\\Unreal Projects\\Pirates with Friends\\Game_PiratesWithFriends\\PiratesWithFriends.uproject";
  const split = splitUnrealRoutingArguments({
    value: 42,
    [CONTEXTFORGE_UNREAL_ROUTE_ARGUMENT]: { projectPath, port: 8000 }
  });
  assert.deepEqual(split.arguments, { value: 42 });
  assert.deepEqual(split.target, { projectPath, port: 8000 });
  assert.throws(
    () =>
      splitUnrealRoutingArguments({
        [CONTEXTFORGE_UNREAL_ROUTE_ARGUMENT]: { port: 8000 }
      }),
    /UNREAL_TARGET_INVALID/
  );
});

test("routes by authoritative project identity and active project root", () => {
  const root = "C:\\Unreal Projects\\Pirates with Friends";
  const pwfRoot = `${root}\\Game_PiratesWithFriends`;
  const pwf = `${pwfRoot}\\PiratesWithFriends.uproject`;
  const deathrey = `${root}\\Asset_Deathrey_58\\Pirate_Deathrey_58.uproject`;
  const routes = [
    { projectPath: pwf, port: 8000 },
    { projectPath: deathrey, port: 8001 }
  ];

  assert.equal(normalizeContextProjectRoot(pwfRoot), pwfRoot);
  assert.equal(projectPathWithinRoot(pwf, root), true);
  assert.equal(projectPathWithinRoot(deathrey, pwfRoot), false);
  assert.equal(selectUnrealRoute(routes, null, pwfRoot).port, 8000);
  assert.equal(
    selectUnrealRoute(routes, { projectPath: deathrey, port: null }, root).port,
    8001
  );
});

test("multi-editor routing fails closed instead of selecting the wrong Editor", () => {
  const root = "C:\\Unreal Projects\\Pirates with Friends";
  const pwf = `${root}\\Game_PiratesWithFriends\\PiratesWithFriends.uproject`;
  const deathrey = `${root}\\Asset_Deathrey_58\\Pirate_Deathrey_58.uproject`;
  const routes = [
    { projectPath: pwf, port: 8000 },
    { projectPath: deathrey, port: 8001 }
  ];

  assert.throws(() => selectUnrealRoute(routes, null, root), /UNREAL_TARGET_REQUIRED/);
  assert.throws(
    () =>
      selectUnrealRoute(
        routes,
        { projectPath: `${root}\\Missing\\Missing.uproject`, port: null },
        root
      ),
    /UNREAL_TARGET_UNAVAILABLE/
  );

  const duplicate = [...routes, { projectPath: pwf, port: 8002 }];
  assert.throws(
    () => selectUnrealRoute(duplicate, { projectPath: pwf, port: null }, root),
    /UNREAL_TARGET_AMBIGUOUS/
  );
  assert.equal(
    selectUnrealRoute(duplicate, { projectPath: pwf, port: 8002 }, root).port,
    8002
  );
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
