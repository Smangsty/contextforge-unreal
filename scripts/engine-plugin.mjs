import { spawn, execFile } from "node:child_process";
import { createHash, randomUUID } from "node:crypto";
import { promisify } from "node:util";
import { cp, lstat, mkdir, mkdtemp, readFile, realpath, rename, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const execFileAsync = promisify(execFile);
export const PLUGIN_NAME = "ContextForgeUnrealTools";
export const BUILD_MARKER = ".contextforge-build.json";
export const INSTALL_MARKER = ".contextforge-managed.json";
export const ENGINE_MAJOR = 5;
export const ENGINE_MINOR = 8;
const PLUGIN_RELATIVE_PATH = ["Engine", "Plugins", "Marketplace", PLUGIN_NAME];
const SOURCE_DIR = fileURLToPath(new URL("../unreal/Plugins/ContextForgeUnrealTools/", import.meta.url));
const PACKAGE_JSON = fileURLToPath(new URL("../package.json", import.meta.url));
const REQUIRED_DEPENDENCIES = ["ToolsetRegistry", "ModelContextProtocol", "PythonScriptPlugin"];
const CHECKSUM_FILES = [
  "ContextForgeUnrealTools.uplugin",
  "Binaries/Win64/UnrealEditor-ContextForgeUnrealTools.dll",
  "Content/Python/init_unreal.py",
  "Content/Python/contextforge_unreal_scripting.py"
];

function error(code, detail = "") {
  const result = new Error(detail ? code + ": " + detail : code);
  result.code = code;
  return result;
}

async function isPresent(filePath) {
  try { await lstat(filePath); return true; }
  catch (cause) { if (cause.code === "ENOENT") return false; throw cause; }
}

async function requireOrdinaryFile(filePath) {
  const info = await lstat(filePath);
  if (!info.isFile() || info.isSymbolicLink()) {
    throw error("ENGINE_PLUGIN_NONORDINARY_PATH", filePath);
  }
  return filePath;
}

async function requireDirectoryNotLink(filePath) {
  const info = await lstat(filePath);
  if (!info.isDirectory() || info.isSymbolicLink()) {
    throw error("ENGINE_PLUGIN_NONORDINARY_PATH", filePath);
  }
}

async function normalizedWindowsPath(filePath) {
  let normalized = resolve(filePath);
  try { normalized = await realpath(normalized); }
  catch { /* A terminated process may no longer have an on-disk image. */ }
  return normalized.replace(/[\\/]+$/, "").toLowerCase();
}

export async function inspectEngine(engineRoot, { platform = process.platform } = {}) {
  if (platform !== "win32") throw error("ENGINE_PLUGIN_WINDOWS_ONLY");
  if (typeof engineRoot !== "string" || engineRoot.trim() === "") {
    throw error("ENGINE_ROOT_REQUIRED");
  }
  const root = resolve(engineRoot);
  if (root.toLowerCase().endsWith(".uproject")) throw error("ENGINE_ROOT_REQUIRED");
  await requireDirectoryNotLink(root);
  const actual = await realpath(root);
  const versionPath = join(actual, "Engine", "Build", "Build.version");
  const exe = join(actual, "Engine", "Binaries", "Win64", "UnrealEditor.exe");
  const uat = join(actual, "Engine", "Build", "BatchFiles", "RunUAT.bat");
  for (const file of [versionPath, exe, uat]) await requireOrdinaryFile(file);
  let version;
  try { version = JSON.parse(await readFile(versionPath, "utf8")); }
  catch { throw error("ENGINE_VERSION_INVALID", versionPath); }
  if (version.MajorVersion !== ENGINE_MAJOR || version.MinorVersion !== ENGINE_MINOR) {
    throw error("ENGINE_VERSION_UNSUPPORTED", String(version.MajorVersion) + "." + String(version.MinorVersion));
  }
  if (!Number.isInteger(version.PatchVersion)) throw error("ENGINE_VERSION_INVALID");
  const versionId = [version.MajorVersion, version.MinorVersion, version.PatchVersion,
    version.Changelist ?? 0, version.BranchName ?? ""].join(":");
  return { root: actual, exe, uat, versionPath, versionId,
    version: [version.MajorVersion, version.MinorVersion, version.PatchVersion].join("."),
    installDir: join(actual, ...PLUGIN_RELATIVE_PATH) };
}

export async function readSourceVersion() {
  const pkg = JSON.parse(await readFile(PACKAGE_JSON, "utf8"));
  const descriptor = JSON.parse(await readFile(join(SOURCE_DIR, PLUGIN_NAME + ".uplugin"), "utf8"));
  if (descriptor.VersionName !== pkg.version ||
      descriptor.EnabledByDefault !== true ||
      descriptor.Installed !== true ||
      descriptor.Modules?.[0]?.Type !== "Editor" ||
      !REQUIRED_DEPENDENCIES.every(name => descriptor.Plugins?.some(p => p.Name === name && p.Enabled))) {
    throw error("ENGINE_PLUGIN_SOURCE_MISMATCH");
  }
  return pkg.version;
}

export async function listRunningUnrealEditors() {
  if (process.platform !== "win32") throw error("ENGINE_PLUGIN_WINDOWS_ONLY");
  const script = "$ErrorActionPreference = 'Stop'; " +
    "$items = @(Get-CimInstance Win32_Process -Filter \"Name = 'UnrealEditor.exe'\" | " +
    "ForEach-Object { @{ pid = $_.ProcessId; path = $_.ExecutablePath } }); " +
    "ConvertTo-Json -InputObject $items -Compress -Depth 3";
  let output;
  try {
    output = (await execFileAsync("powershell.exe",
      ["-NoProfile", "-NonInteractive", "-Command", script],
      { timeout: 20000, maxBuffer: 1048576, windowsHide: true })).stdout.trim();
  } catch (cause) {
    throw error("ENGINE_EDITOR_PROCESS_CHECK_FAILED", cause.message);
  }
  let records;
  try { records = JSON.parse(output || "[]"); }
  catch { throw error("ENGINE_EDITOR_PROCESS_CHECK_FAILED", "Invalid process report"); }
  const items = Array.isArray(records) ? records : [records];
  if (items.some(p => !p || !Number.isInteger(p.pid) || typeof p.path !== "string" || !p.path)) {
    throw error("ENGINE_EDITOR_PROCESS_CHECK_FAILED", "Unresolved Unreal Editor executable path");
  }
  return items;
}

export async function assertNoRunningEditors(engine, listEditors = listRunningUnrealEditors) {
  const processes = await listEditors();
  const target = await normalizedWindowsPath(engine.exe);
  const blockers = [];
  for (const process of processes) {
    if (await normalizedWindowsPath(process.path) === target) blockers.push(process);
  }
  if (blockers.length) {
    throw error("ENGINE_EDITOR_RUNNING", "Close editors from Unreal " + engine.version +
      " (PIDs: " + blockers.map(p => p.pid).join(", ") + ") before modifying engine plugins");
  }
  return processes;
}

async function ensurePluginParent(engine) {
  const base = join(engine.root, "Engine", "Plugins");
  await requireDirectoryNotLink(base);
  const marketplace = join(base, "Marketplace");
  if (await isPresent(marketplace)) await requireDirectoryNotLink(marketplace);
  else {
    await mkdir(marketplace);
    await requireDirectoryNotLink(marketplace);
  }
  return marketplace;
}

async function readManagedState(installDir) {
  if (!(await isPresent(installDir))) return { installed: false, managed: false, version: null };
  await requireDirectoryNotLink(installDir);
  const marker = join(installDir, INSTALL_MARKER);
  if (!(await isPresent(marker))) return { installed: true, managed: false, version: null };
  await requireOrdinaryFile(marker);
  let metadata;
  try { metadata = JSON.parse(await readFile(marker, "utf8")); }
  catch { throw error("ENGINE_PLUGIN_MANAGED_MARKER_INVALID", marker); }
  if (metadata.managedBy !== "contextforge-unreal" || metadata.plugin !== PLUGIN_NAME ||
      typeof metadata.version !== "string" || typeof metadata.fingerprint !== "string") {
    throw error("ENGINE_PLUGIN_MANAGED_MARKER_INVALID", marker);
  }
  if (metadata.fingerprint !== await packageFingerprint(installDir)) {
    throw error("ENGINE_PLUGIN_INSTALL_TAMPERED", installDir);
  }
  return { installed: true, managed: true, version: metadata.version, metadata };
}

export async function getEnginePluginStatus(engineRoot, options = {}) {
  const engine = await inspectEngine(engineRoot, options);
  const state = await readManagedState(engine.installDir);
  const processes = await (options.listEditors ?? listRunningUnrealEditors)();
  const target = await normalizedWindowsPath(engine.exe);
  const runningEditors = [];
  for (const process of processes) {
    if (await normalizedWindowsPath(process.path) === target) runningEditors.push(process.pid);
  }
  return { engineRoot: engine.root, engineVersion: engine.version,
    installDir: engine.installDir, ...state, runningEditors };
}

function rejectCommandMetacharacters(value) {
  if (/["\r\n%!^&|<>]/.test(value)) throw error("ENGINE_PLUGIN_UNSAFE_BUILD_PATH", value);
}

export async function runUatBuild({ engine, descriptor, packageDir }) {
  for (const name of [engine.uat, descriptor, packageDir]) rejectCommandMetacharacters(name);
  const args = ["BuildPlugin", "-Plugin=" + descriptor, "-Package=" + packageDir,
    "-TargetPlatforms=Win64", "-Unattended", "-NoP4"];
  await new Promise((done, fail) => {
    // PowerShell invokes a quoted .bat path without cmd.exe /S quote-stripping.
    // Each argument is separately quoted; no shell:true argument concatenation.
    const quote = value => "\'" + value.replaceAll("\'", "\'\'") + "\'";
    const script = "& " + [engine.uat, ...args].map(quote).join(" ") +
      "; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }";
    const child = spawn("powershell.exe",
      ["-NoProfile", "-NonInteractive", "-Command", script],
      { cwd: engine.root, stdio: "inherit", windowsHide: true });
    child.once("error", fail);
    child.once("exit", (code, signal) => code === 0 ? done() :
      fail(error("ENGINE_PLUGIN_BUILD_FAILED", String(code ?? signal))));
  });
}

async function sha256File(filePath) {
  return createHash("sha256").update(await readFile(filePath)).digest("hex");
}

async function packageFingerprint(root) {
  const hash = createHash("sha256");
  for (const relativePath of CHECKSUM_FILES) {
    const absolute = join(root, ...relativePath.split("/"));
    await requireOrdinaryFile(absolute);
    hash.update(relativePath + "\u0000" + await sha256File(absolute) + "\n");
  }
  return hash.digest("hex");
}

async function inspectPreparedBundle(packageDir, engine, version) {
  const actual = await realpath(packageDir);
  await requireDirectoryNotLink(actual);
  const metadataFile = join(actual, BUILD_MARKER);
  await requireOrdinaryFile(metadataFile);
  let metadata;
  try { metadata = JSON.parse(await readFile(metadataFile, "utf8")); }
  catch { throw error("ENGINE_PLUGIN_BUILD_MARKER_INVALID"); }
  if (metadata.managedBy !== "contextforge-unreal" ||
      metadata.plugin !== PLUGIN_NAME || metadata.version !== version ||
      metadata.engineBuild !== engine.versionId ||
      metadata.fingerprint !== await packageFingerprint(actual)) {
    throw error("ENGINE_PLUGIN_BUILD_MISMATCH",
      "Prepared binary, engine build, or package version does not match");
  }
  const descriptor = JSON.parse(await readFile(join(actual, PLUGIN_NAME + ".uplugin"), "utf8"));
  if (descriptor.VersionName !== version || descriptor.EnabledByDefault !== true ||
      descriptor.Modules?.[0]?.Type !== "Editor") {
    throw error("ENGINE_PLUGIN_BUILD_DESCRIPTOR_INVALID");
  }
  return { path: actual, metadata };
}

export async function prepareEnginePlugin(engineRoot, options = {}) {
  const engine = await inspectEngine(engineRoot, options);
  const version = await readSourceVersion();
  const outputRoot = options.outputRoot
    ? resolve(options.outputRoot)
    : await mkdtemp(join(tmpdir(), "contextforge-ue-plugin-"));
  if (options.outputRoot) await mkdir(outputRoot, { recursive: true });
  const packageDir = join(outputRoot, PLUGIN_NAME);
  if (await isPresent(packageDir)) throw error("ENGINE_PLUGIN_PACKAGE_EXISTS", packageDir);
  const descriptor = join(SOURCE_DIR, PLUGIN_NAME + ".uplugin");
  await (options.runBuild ?? runUatBuild)({ engine, descriptor, packageDir });
  await requireDirectoryNotLink(packageDir);
  const builtDescriptorPath = join(packageDir, PLUGIN_NAME + ".uplugin");
  const builtDescriptor = JSON.parse(await readFile(builtDescriptorPath, "utf8"));
  if (builtDescriptor.VersionName !== version || builtDescriptor.Modules?.[0]?.Type !== "Editor" ||
      !REQUIRED_DEPENDENCIES.every(name => builtDescriptor.Plugins?.some(p => p.Name === name && p.Enabled))) {
    throw error("ENGINE_PLUGIN_BUILD_DESCRIPTOR_INVALID");
  }
  // UAT BuildPlugin strips EnabledByDefault from its packaged descriptor.
  // Restore it in the prepared artifact so the ENGINE installation loads in every project.
  builtDescriptor.EnabledByDefault = true;
  builtDescriptor.Installed = true;
  await writeFile(builtDescriptorPath, JSON.stringify(builtDescriptor, null, 2) + "\n");
  const digest = await packageFingerprint(packageDir);
  const buildRecord = { managedBy: "contextforge-unreal", plugin: PLUGIN_NAME,
    version, engineBuild: engine.versionId, fingerprint: digest };
  await writeFile(join(packageDir, BUILD_MARKER), JSON.stringify(buildRecord, null, 2) + "\n",
    { flag: "wx" });
  return { preparedDir: packageDir, engineVersion: engine.version, version, fingerprint: digest };
}

export async function installEnginePlugin(engineRoot, options = {}) {
  const engine = await inspectEngine(engineRoot, options);
  const version = await readSourceVersion();
  await assertNoRunningEditors(engine, options.listEditors);
  let preparedDir = options.preparedDir;
  if (!preparedDir) preparedDir = (await prepareEnginePlugin(engineRoot, options)).preparedDir;
  const bundle = await inspectPreparedBundle(preparedDir, engine, version);
  const original = await readManagedState(engine.installDir);
  if (original.installed && !original.managed) throw error("ENGINE_PLUGIN_UNMANAGED_CONFLICT");
  // Recheck after compilation, immediately before touching the engine folder.
  await assertNoRunningEditors(engine, options.listEditors);
  const marketplace = await ensurePluginParent(engine);
  const suffix = randomUUID();
  const staging = join(marketplace, "." + PLUGIN_NAME + ".stage-" + suffix);
  const backup = join(marketplace, "." + PLUGIN_NAME + ".backup-" + suffix);
  let oldMoved = false, newMoved = false;
  try {
    await cp(bundle.path, staging, { recursive: true, errorOnExist: true, force: false });
    await inspectPreparedBundle(staging, engine, version);
    const installRecord = { managedBy: "contextforge-unreal", plugin: PLUGIN_NAME,
      version, engineBuild: engine.versionId, fingerprint: bundle.metadata.fingerprint };
    await writeFile(join(staging, INSTALL_MARKER), JSON.stringify(installRecord, null, 2) + "\n",
      { flag: "wx" });
    // Recheck immediately before replacement; never assume a stale process snapshot.
    await assertNoRunningEditors(engine, options.listEditors);
    if (original.installed) {
      await rename(engine.installDir, backup);
      oldMoved = true;
    }
    await rename(staging, engine.installDir);
    newMoved = true;
    const finalState = await readManagedState(engine.installDir);
    if (!finalState.managed || finalState.version !== version) throw error("ENGINE_PLUGIN_INSTALL_VERIFY_FAILED");
    // Installation is committed now. A backup cleanup failure must NOT roll back
    // to a partially deleted backup or discard a verified new installation.
    let retainedBackup = null;
    if (oldMoved) {
      try { await rm(backup, { recursive: true }); }
      catch { retainedBackup = backup; }
    }
    return { status: original.installed ? "updated" : "installed",
      plugin: PLUGIN_NAME, version, engineVersion: engine.version,
      installDir: engine.installDir, requiresEditorRestart: true, projectFilesChanged: false,
      backupCleanupRequired: retainedBackup !== null, retainedBackup };
  } catch (cause) {
    // Best effort rollback; retain any backup on a failed restoration.
    try {
      if (newMoved) await rename(engine.installDir, staging);
      if (oldMoved) await rename(backup, engine.installDir);
      if (await isPresent(staging)) await rm(staging, { recursive: true });
    } catch (rollbackCause) {
      throw error("ENGINE_PLUGIN_ROLLBACK_FAILED",
        cause.message + "; recovery required at " + backup + ": " + rollbackCause.message);
    }
    throw cause;
  }
}

export async function uninstallEnginePlugin(engineRoot, options = {}) {
  const engine = await inspectEngine(engineRoot, options);
  await assertNoRunningEditors(engine, options.listEditors);
  const state = await readManagedState(engine.installDir);
  if (!state.installed) return { status: "already_absent", installDir: engine.installDir };
  if (!state.managed) throw error("ENGINE_PLUGIN_UNMANAGED_CONFLICT");
  const backup = join(dirname(engine.installDir),
    "." + PLUGIN_NAME + ".remove-" + randomUUID());
  await assertNoRunningEditors(engine, options.listEditors);
  await rename(engine.installDir, backup);
  try { await rm(backup, { recursive: true }); }
  catch (cause) {
    // Deletion may have removed some backup files already. Restoring that backup
    // would risk silently reinstalling an incomplete native plugin.
    throw error("ENGINE_PLUGIN_REMOVE_INCOMPLETE",
      "Inspect retained backup " + backup + ": " + cause.message);
  }
  return { status: "removed", engineVersion: engine.version,
    installDir: engine.installDir, requiresEditorRestart: true, projectFilesChanged: false };
}
