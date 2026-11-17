import test from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");

test("UE 5.8 nested Blueprint cache compatibility", (t) => {
  const launchers = process.platform === "win32"
    ? [
        ["py", ["-3"]],
        ["python", []],
        [String.raw`C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\ThirdParty\Python3\Win64\python.exe`, []],
      ]
    : [["python3", []]];
  let result;
  for (const [command, prefix] of launchers) {
    const current = spawnSync(command, [
      ...prefix, "-m", "unittest", "discover", "-s", "test",
      "-p", "test_blueprint_nested_graph_compat.py", "-v",
    ], { cwd: root, encoding: "utf8", timeout: 30000 });
    if (current.error?.code === "ENOENT") continue;
    if (process.platform === "win32" && current.status === 9009 &&
        /Python was not found/i.test((current.stdout ?? "") + (current.stderr ?? ""))) continue;
    result = current;
    break;
  }
  if (!result) {
    t.skip("Python is unavailable outside the Unreal Editor");
    return;
  }
  assert.equal(result.error, undefined, String(result.error));
  assert.equal(result.status, 0, (result.stdout ?? "") + (result.stderr ?? ""));
});
