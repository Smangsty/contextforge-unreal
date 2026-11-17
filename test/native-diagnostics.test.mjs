import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

import { annotateUnrealTool, isReviewedReadOnlyTool } from "../src/contextforge-unreal.mjs";

const root = fileURLToPath(new URL("../", import.meta.url));
const pluginDir = join(root, "unreal", "Plugins", "ContextForgeUnrealTools");
const nativeDir = join(pluginDir, "Source", "ContextForgeUnrealTools");

const readMethods = [
  "GetDiagnosticsCapabilities",
  "GetNetworkDiagnostics",
  "GetRewindStatus",
  "InspectLiveActor",
  "ListLiveActors",
  "ListRewindAnimations",
  "ReadRewindAnimation"
];

const writeMethods = [
  "OpenRewindDebugger",
  "StartRewindRecording",
  "StopRewindRecording",
  "ToggleDebugOverlay"
];

const tool = (method) => "ContextForgeUnrealTools.ContextForgeDiagnosticsTools." + method;

test("2.6 diagnostics are integrated into the managed engine-wide editor module", async () => {
  const [descriptorRaw, rules, module, header, native] = await Promise.all([
    readFile(join(pluginDir, "ContextForgeUnrealTools.uplugin"), "utf8"),
    readFile(join(nativeDir, "ContextForgeUnrealTools.Build.cs"), "utf8"),
    readFile(join(nativeDir, "Private", "ContextForgeUnrealToolsModule.cpp"), "utf8"),
    readFile(join(nativeDir, "Private", "ContextForgeDiagnosticsTools.h"), "utf8"),
    readFile(join(nativeDir, "Private", "ContextForgeDiagnosticsTools.cpp"), "utf8")
  ]);
  const descriptor = JSON.parse(descriptorRaw);
  assert.equal(descriptor.VersionName, "2.6.2");
  assert.equal(descriptor.Version, 20602);
  assert.equal(descriptor.Modules.length, 1, "extend existing module, never launch second MCP server");
  assert.equal(descriptor.Modules[0].Name, "ContextForgeUnrealTools");
  assert.equal(descriptor.Modules[0].Type, "Editor");
  assert.ok(descriptor.Plugins.some(p => p.Name === "GameplayInsights" && p.Enabled));
  for (const dependency of ["LevelEditor", "RewindDebuggerInterface", "GameplayInsights", "TraceServices", "TraceAnalysis", "Json"]) {
    assert.ok(rules.includes('"' + dependency + '"'), "missing native dependency " + dependency);
  }
  assert.match(module, /RegisterToolsetClass\(UContextForgeDiagnosticsTools::StaticClass\(\)\)/);
  assert.match(module, /UnregisterToolsetClass\(UContextForgeDiagnosticsTools::StaticClass\(\)\)/);
  assert.match(header, /class UContextForgeDiagnosticsTools : public UToolsetDefinition/);
  assert.match(header, /#include "ContextForgeDiagnosticsTools.generated.h"/);
  for (const method of [...readMethods, ...writeMethods]) {
    assert.match(header, new RegExp("UFUNCTION\\(BlueprintCallable[^\\n]+\\)\\s+static FString " + method + "\\("),
      "MCP-invisible method: " + method);
    assert.ok(native.includes("UContextForgeDiagnosticsTools::" + method + "("),
      "native implementation missing: " + method);
  }
  const fullSource = [header, native, module, rules, descriptorRaw].join("\n");
  assert.doesNotMatch(fullSource, /Project_TIB|C:\\Unreal Projects|\/Game\/TIB|BP_Character_Player/,
    "diagnostics must not depend on a specific Unreal project or Blueprint");
});

test("Rewind initializes through the Level Editor tab manager", async () => {
  const native = await readFile(join(nativeDir, "Private", "ContextForgeDiagnosticsTools.cpp"), "utf8");
  assert.match(native, /GetLevelEditorTabManager\(\)/);
  assert.match(native, /LevelEditorTabs->TryInvokeTab\(Module->GetMainTabName\(\)\)/);
  assert.doesNotMatch(native, /FGlobalTabmanager::Get\(\)->TryInvokeTab/);
  assert.match(native, /Rewind Debugger tab opened without an initialized debugger instance/);
});

test("Rewind trace readers use Epic provider-scoped read locks", async () => {
  const native = await readFile(join(nativeDir, "Private", "ContextForgeDiagnosticsTools.cpp"), "utf8");
  assert.match(native, /#include "Common\/ProviderLock\.h"/);
  assert.equal((native.match(/FProviderReadScopeLock AnimationReadScope\(\*Provider\)/g) ?? []).length, 2,
    "listing and timeline reading must both hold animation provider locks");
  assert.match(native, /FProviderReadScopeLock GameplayReadScope\(\*Gameplay\)/,
    "gameplay provider names require their own read lock");
  assert.match(native, /FAnalysisSessionReadScope Lock\(\*Session\)/);
});

test("recorded trace IDs remain strings and diagnostic queries are bounded", async () => {
  const native = await readFile(join(nativeDir, "Private", "ContextForgeDiagnosticsTools.cpp"), "utf8");
  const header = await readFile(join(nativeDir, "Private", "ContextForgeDiagnosticsTools.h"), "utf8");
  assert.match(header, /ReadRewindAnimation\(const FString& AnimInstanceId/);
  assert.match(native, /FCString::Strtoui64\(\*AnimInstanceId/);
  assert.match(native, /FMath::Clamp\(MaxInstances, 1, 100\)/);
  assert.match(native, /FMath::Clamp\(MaxEvents, 1, 500\)/);
  assert.match(native, /FMath::Clamp\(MaxActors, 1, 200\)/);
  assert.match(native, /FMath::Clamp\(MaxActors, 1, 8\)/);
  assert.match(native, /FMath::Clamp\(static_cast<double>\(LastSeconds\), 0\.1, 60\.0\)/);
  assert.match(native, /BoneNames\.Num\(\) > 12/);
  assert.match(native, /if \(!Allowed\.Contains\(Command\)\)/);
  assert.match(native, /const TraceServices::IAnalysisSession\* Session/);
});

test("seven reviewed inspection tools are READ and four controls remain WRITE", () => {
  for (const method of readMethods) {
    const name = tool(method);
    assert.equal(isReviewedReadOnlyTool(name), true, name);
    assert.equal(annotateUnrealTool({ name }).annotations.readOnlyHint, true, name);
  }
  for (const method of writeMethods) {
    const name = tool(method);
    assert.equal(isReviewedReadOnlyTool(name), false, name);
    assert.notEqual(annotateUnrealTool({ name }).annotations.readOnlyHint, true, name);
  }
});
