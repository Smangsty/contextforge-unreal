# Optional Unreal Engine 5.8 Authoring Toolsets

The `ContextForgeUnrealTools` companion plugin extends Epic's **official Unreal Model Context Protocol and ToolsetRegistry**. It does not implement a second MCP transport, replace Epic's toolsets, or change ContextForge's project routing and approval rules.

The Node adapter remains a thin per-`.uproject` broker. Installing this optional editor-only plugin adds the following native tools to the same Unreal Editor MCP catalog:

| Toolset | Tool | Actual behavior |
|---|---|---|
| ContextForgeBlueprintTools | ListInterfaces | Read direct and inherited native or Blueprint interfaces |
| ContextForgeBlueprintTools | AddInterface | Idempotent interface attachment with compilation and verification |
| ContextForgeBlueprintTools | RemoveInterface | Reject inherited removals; preserve implementations by default; gate destructive removal |
| ContextForgeBlueprintTools | CompileWithDiagnostics | Capture compiler messages, counts, outcome and optional node details |
| ContextForgeBlueprintTools | AddComponentVerified | SCS component addition, initialization, partial-success receipt, safe retry protection |
| ContextForgeBlueprintTools | GetGraphSnapshot | Stable node/pin/link inspection and graph fingerprint |
| ContextForgeBlueprintTools | RewireGraphConnection | Targeted, optimistic-concurrency-guarded link replacement and unrelated-link verification |
| ContextForgePIETools | StartPIE | Request a bounded local play test on an explicitly loaded, clean map |
| ContextForgePIETools | VerifyPIE | Inspect actual PIE peers, possession, expected classes, roles and component presence |
| ContextForgePIETools | StopPIE | Cancel a queued launch or request the owned play session to end |
| ContextForgePIETools | VerifyRestoration | Verify all PIE worlds stopped and the original map remains loaded |
| ContextForgeUnrealScripting | execute_python | Run explicitly approved, full-trust Python and return real stdout, stderr, traceback and project identity |

**Important:** Tool dispatch or successful PIE launch is never proof of test success. `StartPIE` returns `REQUESTED`; only `VerifyPIE` can return an evidence-backed `PASS`. Stop completion is similarly proved by `VerifyRestoration`.

## Install into a UE 5.8 project

1. Copy the whole `unreal/Plugins/ContextForgeUnrealTools` directory into the selected Unreal project's `Plugins/ContextForgeUnrealTools` directory. Keep the C++ source, `.uplugin`, and `Content/Python` files together.
2. Enable `ContextForgeUnrealTools` in the Unreal Editor and its dependencies: **Model Context Protocol**, **ToolsetRegistry**, **Python Script Plugin**, and **Editor Scripting Utilities**. Keep Epic's official toolsets enabled for their existing operations.
3. Compile the plugin with the project's Unreal 5.8 toolchain and restart that Editor. The companion plugin requires UE 5.8 APIs and has only been compiled against UE **5.8.3 on Win64**.
4. Verify the editor log contains the native `ContextForgeBlueprintTools`, `ContextForgePIETools` and Python toolset registration entries. If Unreal MCP was already running, execute `ModelContextProtocol.RefreshTools` and refresh/re-inspect the ContextForge capability catalog.
5. Approve the **new exact tool identity/revision** through ContextForge's ordinary admission and authorization process. Enabling a plugin or seeing its tools is not authority to invoke them. In particular, `execute_python` is a full-trust **WRITE** operation.

The Node npm package contains **source** for the UE companion plugin, not an engine-independent DLL. Nothing installs or enables the plugin in your production projects automatically. Existing adapter discovery, direct tool naming, routed `_contextforgeUnreal` metadata, and approvals remain in effect.

## Safety and receipts

Blueprint write operations preflight the exact `/Game/` target, isolate mutations, compile and verify readback. Optional save applies only to the requested loaded Blueprint. The normal `AssetTools.save_assets` empty-array behavior (save all dirty assets) is **never** used. Mutations that were applied before compilation fails are explicitly reported. Do not blindly retry a partly completed mutation.

The graph editor requires a fresh fingerprint, explicit stable node GUIDs and pin names, and refuses link changes that would silently remove unrelated connections. Compiler diagnostics report actual tokenized messages. Node/graph fields stay empty where the UE 5.8 public compiler API does not provide reliable ownership. Warnings-as-errors is an available compile flag.

Python is **not a sandbox** and has no guaranteed in-progress cancellation or timeout on the UE editor game thread. Only the code and exact project identity approved for that WRITE request may run. Execution output and exceptions are capped in the receipt; a transport success does not mean code success.

PIE is intended for disposable test maps. It refuses a dirty or mismatched map or a previously active PIE session. No production map, GameMode, startup map or unrelated package is switched or saved. `VerifyPIE` reads observed local actors and roles; the caller must explicitly provide expected controller/pawn classes. For multi-client authority/prediction tests, add project-specific assertions to validate the actual gameplay protocol. Do not interpret world count alone as a gameplay PASS.

## Developer verification

The repo's `test/ue58-smoke.py` provides a **disposable-project-only** live smoke script. It guards the project filename and creates transient Blueprint fixtures under `/Game/CFProbe`. Run it through the Unreal 5.8 headless editor against a separately created `CFSmoke.uproject` with the plugin installed. Inspect the `CF_SMOKE ALL_TESTS_PASSED` marker in the editor log; a headless editor process exit code of zero can still conceal script assertion failures.

The native source was compiled using `RunUAT.bat BuildPlugin` against UE 5.8.3 Win64. Single-player and multi-client PIE behaviors must also be exercised in a graphical editor before claiming full gameplay E2E coverage. 
