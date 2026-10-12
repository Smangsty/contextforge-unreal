#!/usr/bin/env node
import {
  getEnginePluginStatus, installEnginePlugin, prepareEnginePlugin, uninstallEnginePlugin
} from "../scripts/engine-plugin.mjs";

const help = [
  "ContextForge Unreal engine-wide plugin manager",
  "",
  "Usage: contextforge-unreal-plugin <status|prepare|install|uninstall> --engine-root <UE_5.8 root> [options]",
  "",
  "Options:",
  "  --output-root <directory>  Where to place the compiled plugin (prepare)",
  "  --prepared-dir <directory> Copy a matching precompiled plugin instead of rebuilding (install)",
  "  --help                     Show this help",
  "",
  "Engine plugin installation affects every project using that Unreal Engine build.",
  "The command never touches a .uproject or project Plugins folder.",
  "Install/update/uninstall require all editors for the selected engine to be closed.",
  "Install writes inside Engine/Plugins/Marketplace, which may require elevation.",
].join("\n");

async function main(args) {
  if (args.includes("--help") || args.includes("-h") || !args.length) {
    console.log(help);
    return;
  }
  const action = args[0];
  if (!["status", "prepare", "install", "uninstall"].includes(action)) {
    throw new Error("ENGINE_PLUGIN_UNKNOWN_ACTION: " + action);
  }
  const allowed = new Set(["--engine-root", "--output-root", "--prepared-dir"]);
  const options = {};
  for (let i = 1; i < args.length; i += 2) {
    if (!allowed.has(args[i]) || !args[i + 1] || args[i + 1].startsWith("--")) {
      throw new Error("ENGINE_PLUGIN_ARGUMENT_INVALID: " + (args[i] ?? "<missing>"));
    }
    if (Object.prototype.hasOwnProperty.call(options, args[i])) {
      throw new Error("ENGINE_PLUGIN_ARGUMENT_DUPLICATE: " + args[i]);
    }
    options[args[i]] = args[i + 1];
  }
  if (!options["--engine-root"]) throw new Error("ENGINE_ROOT_REQUIRED");
  if (action !== "prepare" && options["--output-root"] !== undefined) {
    throw new Error("ENGINE_PLUGIN_ARGUMENT_INVALID: --output-root");
  }
  if (action !== "install" && options["--prepared-dir"] !== undefined) {
    throw new Error("ENGINE_PLUGIN_ARGUMENT_INVALID: --prepared-dir");
  }
  const engineRoot = options["--engine-root"];
  let receipt;
  if (action === "status") receipt = await getEnginePluginStatus(engineRoot);
  if (action === "prepare") receipt = await prepareEnginePlugin(engineRoot, {
    outputRoot: options["--output-root"]
  });
  if (action === "install") receipt = await installEnginePlugin(engineRoot, {
    preparedDir: options["--prepared-dir"]
  });
  if (action === "uninstall") receipt = await uninstallEnginePlugin(engineRoot);
  console.log(JSON.stringify(receipt, null, 2));
}

main(process.argv.slice(2)).catch(cause => {
  console.error(JSON.stringify({ success: false, code: cause.code ?? "ENGINE_PLUGIN_ERROR",
    message: cause.message }));
  process.exitCode = 1;
});
