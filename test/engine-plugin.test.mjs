import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { cp, mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

import {
  PLUGIN_NAME, BUILD_MARKER, INSTALL_MARKER,
  assertNoRunningEditors, getEnginePluginStatus, inspectEngine,
  installEnginePlugin, prepareEnginePlugin, readSourceVersion, uninstallEnginePlugin
} from "../scripts/engine-plugin.mjs";

const source = fileURLToPath(new URL("../unreal/Plugins/ContextForgeUnrealTools/", import.meta.url));
const root = fileURLToPath(new URL("../", import.meta.url));
const noEditors = async () => [];

async function fixture(t, patch = 3) {
  const work = await mkdtemp(join(tmpdir(), "cf-plugin-manager-test-"));
  t.after(() => rm(work, { recursive: true, force: true }));
  const engineRoot = join(work, "UE_5.8");
  const version = join(engineRoot, "Engine", "Build", "Build.version");
  const exe = join(engineRoot, "Engine", "Binaries", "Win64", "UnrealEditor.exe");
  const uat = join(engineRoot, "Engine", "Build", "BatchFiles", "RunUAT.bat");
  const marketplace = join(engineRoot, "Engine", "Plugins", "Marketplace");
  for (const dir of [dirname(version), dirname(exe), dirname(uat), marketplace]) {
    await mkdir(dir, { recursive: true });
  }
  await writeFile(version, JSON.stringify({ MajorVersion: 5, MinorVersion: 8, PatchVersion: patch,
    Changelist: 12345, BranchName: "test" }));
  await writeFile(exe, "test editor");
  await writeFile(uat, "test UAT");
  return { work, engineRoot, exe, installDir: join(marketplace, PLUGIN_NAME) };
}

async function fakeBuild({ packageDir }) {
  await cp(source, packageDir, { recursive: true });
  // Mirror UE BuildPlugin: packaged descriptors omit EnabledByDefault.
  const descriptorPath = join(packageDir, PLUGIN_NAME + ".uplugin");
  const descriptor = JSON.parse(await readFile(descriptorPath, "utf8"));
  delete descriptor.EnabledByDefault;
  await writeFile(descriptorPath, JSON.stringify(descriptor));
  const dll = join(packageDir, "Binaries", "Win64", "UnrealEditor-ContextForgeUnrealTools.dll");
  await mkdir(dirname(dll), { recursive: true });
  await writeFile(dll, "fixture prebuilt native editor DLL");
}

async function prepared(t, patch = 3) {
  const fx = await fixture(t, patch);
  const build = await prepareEnginePlugin(fx.engineRoot,
    { platform: "win32", outputRoot: join(fx.work, "prepared"), runBuild: fakeBuild });
  return { ...fx, build };
}

test("versioned adapter includes native management CLI and matches plugin descriptor", async () => {
  const pkg = JSON.parse(await readFile(join(root, "package.json"), "utf8"));
  const descriptor = JSON.parse(await readFile(join(source, PLUGIN_NAME + ".uplugin"), "utf8"));
  assert.equal(await readSourceVersion(), pkg.version);
  assert.equal(descriptor.VersionName, pkg.version);
  assert.equal(pkg.bin["contextforge-unreal-plugin"], "bin/contextforge-unreal-plugin.mjs");
  assert.ok(pkg.files.includes("scripts/engine-plugin.mjs"));
  assert.ok(descriptor.EnabledByDefault);
  assert.ok(descriptor.Installed);
  assert.equal(descriptor.Modules[0].Type, "Editor");
});

test("prepare builds only outside engine and records exact build and binary fingerprint", async t => {
  const fx = await prepared(t);
  const info = await inspectEngine(fx.engineRoot, { platform: "win32" });
  assert.equal(info.version, "5.8.3");
  assert.equal(fx.build.version, await readSourceVersion());
  assert.equal(fx.build.engineVersion, "5.8.3");
  const marker = JSON.parse(await readFile(join(fx.build.preparedDir, BUILD_MARKER), "utf8"));
  assert.equal(marker.engineBuild, info.versionId);
  assert.ok(marker.fingerprint);
  const packaged = JSON.parse(await readFile(join(fx.build.preparedDir, PLUGIN_NAME + ".uplugin"), "utf8"));
  assert.equal(packaged.EnabledByDefault, true, "UAT-stripped default must be restored");
  assert.equal(packaged.Installed, true);
  const initial = await getEnginePluginStatus(fx.engineRoot,
    { platform: "win32", listEditors: noEditors });
  assert.equal(initial.installed, false);
});

test("engine-wide install, idempotent update and removal do not touch project files", async t => {
  const fx = await prepared(t);
  const projectFile = join(fx.work, "SomeProject", "SomeProject.uproject");
  await mkdir(dirname(projectFile), { recursive: true });
  await writeFile(projectFile, '{"Plugins":[]}');
  const opts = { platform: "win32", preparedDir: fx.build.preparedDir, listEditors: noEditors };
  const first = await installEnginePlugin(fx.engineRoot, opts);
  assert.equal(first.status, "installed");
  assert.equal(first.projectFilesChanged, false);
  assert.equal((await getEnginePluginStatus(fx.engineRoot, opts)).managed, true);
  assert.equal((await readFile(projectFile, "utf8")), '{"Plugins":[]}');
  assert.equal((await readFile(join(fx.installDir, INSTALL_MARKER), "utf8")).includes("contextforge-unreal"), true);
  assert.equal((await installEnginePlugin(fx.engineRoot, opts)).status, "updated");
  assert.equal((await uninstallEnginePlugin(fx.engineRoot, opts)).status, "removed");
  assert.equal((await uninstallEnginePlugin(fx.engineRoot, opts)).status, "already_absent");
  assert.equal((await readFile(projectFile, "utf8")), '{"Plugins":[]}');
});

test("installer refuses to alter an engine with running Unreal editors", async t => {
  const fx = await prepared(t);
  const opts = { platform: "win32", preparedDir: fx.build.preparedDir,
    listEditors: async () => [{ pid: 1234, path: fx.exe }] };
  await assert.rejects(installEnginePlugin(fx.engineRoot, opts), /ENGINE_EDITOR_RUNNING/);
  assert.deepEqual((await getEnginePluginStatus(fx.engineRoot, opts)).runningEditors, [1234]);
  assert.equal((await getEnginePluginStatus(fx.engineRoot, opts)).installed, false);
});

test("existing unmanaged engine plugin is never overwritten or uninstalled", async t => {
  const fx = await prepared(t);
  await mkdir(fx.installDir);
  const foreign = join(fx.installDir, "foreign.txt");
  await writeFile(foreign, "DO NOT TOUCH");
  const opts = { platform: "win32", preparedDir: fx.build.preparedDir, listEditors: noEditors };
  await assert.rejects(installEnginePlugin(fx.engineRoot, opts), /UNMANAGED_CONFLICT/);
  await assert.rejects(uninstallEnginePlugin(fx.engineRoot, opts), /UNMANAGED_CONFLICT/);
  assert.equal(await readFile(foreign, "utf8"), "DO NOT TOUCH");
});

test("prebuilt binaries cannot be installed into the wrong Unreal patch/build", async t => {
  const fx = await prepared(t, 3);
  const other = await fixture(t, 4);
  await assert.rejects(installEnginePlugin(other.engineRoot, {
    platform: "win32", preparedDir: fx.build.preparedDir, listEditors: noEditors
  }), /BUILD_MISMATCH/);
  assert.equal((await getEnginePluginStatus(other.engineRoot,
    { platform: "win32", listEditors: noEditors })).installed, false);
});

test("tampering with prebuilt native DLL is rejected before installation", async t => {
  const fx = await prepared(t);
  const dll = join(fx.build.preparedDir, "Binaries", "Win64",
    "UnrealEditor-ContextForgeUnrealTools.dll");
  await writeFile(dll, "tampered binary");
  await assert.rejects(installEnginePlugin(fx.engineRoot, {
    platform: "win32", preparedDir: fx.build.preparedDir, listEditors: noEditors
  }), /BUILD_MISMATCH/);
});

test("unsupported engine versions and source mismatch fail closed", async t => {
  const fx = await fixture(t, 3);
  const file = join(fx.engineRoot, "Engine", "Build", "Build.version");
  await writeFile(file, JSON.stringify({ MajorVersion: 5, MinorVersion: 7, PatchVersion: 4 }));
  await assert.rejects(inspectEngine(fx.engineRoot, { platform: "win32" }), /ENGINE_VERSION_UNSUPPORTED/);
  await assert.rejects(inspectEngine(fx.engineRoot, { platform: "linux" }), /ENGINE_PLUGIN_WINDOWS_ONLY/);
});

test("CLI documents one approved engine install rather than a per-project step", () => {
  const cli = fileURLToPath(new URL("../bin/contextforge-unreal-plugin.mjs", import.meta.url));
  const result = execFileSync(process.execPath, [cli, "--help"], { encoding: "utf8" });
  assert.match(result, /install/);
  assert.match(result, /Engine\/Plugins\/Marketplace/);
  assert.match(result, /never touches a \.uproject/i);
});

test("installed plugin content tampering blocks subsequent updates and removal", async t => {
  const fx = await prepared(t);
  const opts = { platform: "win32", preparedDir: fx.build.preparedDir, listEditors: noEditors };
  await installEnginePlugin(fx.engineRoot, opts);
  const init = join(fx.installDir, "Content", "Python", "init_unreal.py");
  await writeFile(init, "unexpected modification");
  await assert.rejects(installEnginePlugin(fx.engineRoot, opts), /ENGINE_PLUGIN_INSTALL_TAMPERED/);
  await assert.rejects(uninstallEnginePlugin(fx.engineRoot, opts), /ENGINE_PLUGIN_INSTALL_TAMPERED/);
});

test("opt-in full-trust Python requires explicit environment choice", async () => {
  const script = await readFile(join(source, "Content", "Python", "init_unreal.py"), "utf8");
  assert.match(script, /CONTEXTFORGE_UNREAL_ENABLE_FULL_TRUST_PYTHON/);
});
