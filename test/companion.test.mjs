import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

const root = new URL("../", import.meta.url);
const read = (relativePath) => readFileSync(new URL(relativePath, root), "utf8");

test("engine-wide UE 5.8 plugin is included in the npm package", () => {
  const pkg = JSON.parse(read("package.json"));
  const descriptor = JSON.parse(
    read("unreal/Plugins/ContextForgeUnrealTools/ContextForgeUnrealTools.uplugin")
  );
  const packagedPluginPaths = pkg.files.filter(path => path.startsWith("unreal/Plugins/ContextForgeUnrealTools/"));
  assert.ok(packagedPluginPaths.includes("unreal/Plugins/ContextForgeUnrealTools/ContextForgeUnrealTools.uplugin"), "npm distribution must carry the Unreal descriptor");
  assert.ok(packagedPluginPaths.includes("unreal/Plugins/ContextForgeUnrealTools/Source"), "npm distribution must carry native source");
  assert.ok(packagedPluginPaths.includes("unreal/Plugins/ContextForgeUnrealTools/Content/Python"), "npm distribution must carry Python source");
  assert.equal(packagedPluginPaths.some(path => path.includes("/Config/") || path.includes("/Binaries") || path.includes("/Intermediate")), false, "exclude generated and binary directories");
  assert.equal(descriptor.Modules[0].Type, "Editor", "plugin must remain editor-only");
  assert.equal(descriptor.EnabledByDefault, true, "engine install must enable all UE 5.8 projects by default");
  assert.equal(descriptor.Installed, true, "engine plugin must be marked installed");
  assert.equal(descriptor.VersionName, pkg.version, "plugin and adapter use one version");
  assert.equal(pkg.bin["contextforge-unreal-plugin"], "bin/contextforge-unreal-plugin.mjs");
  assert.ok(pkg.files.includes("scripts/engine-plugin.mjs"), "the management CLI must ship with the adapter");
  for (const dependency of ["ToolsetRegistry", "ModelContextProtocol", "PythonScriptPlugin"]) {
    assert.ok(descriptor.Plugins.some(item => item.Name === dependency && item.Enabled === true));
  }
});

test("native toolsets keep exact mutation receipts and context-preserving graph guards", () => {
  const blueprintH = read("unreal/Plugins/ContextForgeUnrealTools/Source/ContextForgeUnrealTools/Private/ContextForgeBlueprintTools.h");
  const blueprintCPP = read("unreal/Plugins/ContextForgeUnrealTools/Source/ContextForgeUnrealTools/Private/ContextForgeBlueprintTools.cpp");
  for (const method of ["ListInterfaces", "AddInterface", "RemoveInterface",
    "CompileWithDiagnostics", "AddComponentVerified",
    "GetGraphSnapshot", "RewireGraphConnection"]) {
    assert.ok(blueprintH.includes(method + "("), method);
  }
  for (const field of ["bMutationApplied", "bCompileSucceeded", "bReadbackVerified",
    "bSaveAttempted", "bSafeToRetryCreation", "Diagnostics"]) {
    assert.ok(blueprintH.includes(field), field);
  }
  assert.match(blueprintCPP, /ExpectedFingerprint/);
  assert.match(blueprintCPP, /UnrelatedBefore == UnrelatedAfter/);
  assert.match(blueprintCPP, /SaveLoadedAsset\(Blueprint, true\)/);
});

test("full-trust Python is project-bound, returns execution results, and defaults to WRITE", async () => {
  const scripting = read(
    "unreal/Plugins/ContextForgeUnrealTools/Content/Python/contextforge_unreal_scripting.py"
  );
  const source = await import("../src/contextforge-unreal.mjs");
  assert.match(scripting, /PROJECT_IDENTITY_MISMATCH/);
  assert.match(scripting, /contextlib\.redirect_stdout/);
  assert.match(scripting, /traceback\.format_exc\(\)/);
  assert.match(scripting, /receipt\.success\s*=\s*bool\(success\)/);
  const pythonName = "contextforge_unreal_scripting.ContextForgeUnrealScripting.execute_python";
  assert.equal(source.annotateUnrealTool({ name: pythonName }).annotations.readOnlyHint, false);
});

test("PIE gameplay result is separate from session-start and restoration requests", () => {
  const header = read("unreal/Plugins/ContextForgeUnrealTools/Source/ContextForgeUnrealTools/Private/ContextForgePIETools.h");
  const cpp = read("unreal/Plugins/ContextForgeUnrealTools/Source/ContextForgeUnrealTools/Private/ContextForgePIETools.cpp");
  for (const method of ["StartPIE", "VerifyPIE", "StopPIE", "VerifyRestoration"]) {
    assert.ok(header.includes(method), method);
  }
  assert.match(cpp, /TEXT\("REQUESTED"\)/);
  assert.match(cpp, /TEXT\("PASS"\)/);
  assert.match(cpp, /TEXT\("RESTORED"\)/);
  assert.match(cpp, /CancelRequestPlaySession\(\)/);
  assert.match(cpp, /ExpectedMap == ContextForgePIE::EditorMap\(\)/);
});
