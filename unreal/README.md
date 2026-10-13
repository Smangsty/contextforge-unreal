# Engine-wide ContextForge Unreal 5.8 authoring plugin

ContextForgeUnrealTools is an editor extension shipped **inside the same npm package and release as contextforge-unreal**. It extends Epic's official Model Context Protocol and ToolsetRegistry. It is not another MCP server. Both components share the adapter's version.

Installing it once in an Unreal Engine installation makes its tools available to every project using that installation, unless a project explicitly disables the plugin. No per-project plugin folder or .uproject edit is needed.

The implementation was previously smoke-tested against Unreal 5.8.3 on Win64. The installer rejects other minor versions until their API compatibility is validated.

## Installation and updates

First install/update the contextforge-unreal package through normal ContextForge Discovery review. That same package includes a contextforge-unreal-plugin command for status, preparation, installation, update, and removal.

Run via **the approved ContextForge Command capability** or a trusted terminal on Windows:

    node <package-root>\bin\contextforge-unreal-plugin.mjs status --engine-root "C:\Program Files\Epic Games\UE_5.8"
    node <package-root>\bin\contextforge-unreal-plugin.mjs prepare --engine-root "C:\Program Files\Epic Games\UE_5.8" --output-root "C:\keel-contextforge\.build\ue-engine-plugin"
    node <package-root>\bin\contextforge-unreal-plugin.mjs install --engine-root "C:\Program Files\Epic Games\UE_5.8" --prepared-dir "C:\keel-contextforge\.build\ue-engine-plugin\ContextForgeUnrealTools"

Or omit --prepared-dir from install to build and install in one step. Agent-driven, approval-gated updates use the same package and CLI, rather than requiring users to move folders into each project. Updating the adapter alone **never silently modifies the engine**.

**Prepare** invokes Unreal's RunUAT.bat BuildPlugin against the exact selected engine to produce a Win64 editor plugin. It does not change engine installation files or any project. A Windows C++ build toolchain is necessary.

**Install** validates the adapter/plugin version, the exact Engine/Build/Build.version identity, compiled DLL, and the prepared package's checksums, then stages it at:

    <Engine Root>\Engine\Plugins\Marketplace\ContextForgeUnrealTools

The manager creates a sibling staging folder, temporarily backs up the previous **managed** plugin, verifies the replacement, and attempts rollback on failure. Unknown/unmanaged installations are never overwritten or removed. Running editors of the targeted engine block install, update and removal. Engine files under Program Files may require administrative privileges, but the manager never auto-elevates. Close that engine's editors and restart after installation.

To remove the managed plugin, once relevant editors are closed:

    node <package-root>\bin\contextforge-unreal-plugin.mjs uninstall --engine-root "C:\Program Files\Epic Games\UE_5.8"

After launching Unreal, verify the ContextForgeBlueprintTools native methods appear in the UE MCP catalog and re-inspect the updated ContextForge capability. Mere discovery does not grant permission to invoke new tool identities.

## Native operations

| Toolset | Tool | Behavior |
|---|---|---|
| ContextForgeBlueprintTools | ListInterfaces | Lists direct and inherited native/Blueprint interfaces |
| ContextForgeBlueprintTools | AddInterface | Adds an interface idempotently, compiles and checks the result |
| ContextForgeBlueprintTools | RemoveInterface | Refuses inherited removal and gates destructive removal |
| ContextForgeBlueprintTools | CompileWithDiagnostics | Reports actual compile warnings/errors and status |
| ContextForgeBlueprintTools | AddComponentVerified | Rejects abstract component classes before SCS allocation; verifies concrete components by compilation/readback |
| ContextForgeBlueprintTools | GetGraphSnapshot | Inspects stable nodes, pins and graph fingerprint |
| ContextForgeBlueprintTools | RewireGraphConnection | Precondition-guarded targeted rewiring preserving unrelated links |
| ContextForgePIETools | StartPIE | Requests bounded PIE on an expected clean map |
| ContextForgePIETools | VerifyPIE | Inspects peers, possession and class expectations; advisory `sessionWarnings` do not fail gameplay, but errors/fatals block it |
| ContextForgePIETools | StopPIE | Cancels/stops only an owned PIE session |
| ContextForgePIETools | VerifyRestoration | Checks all PIE worlds ended and the original map remains |
| ContextForgeUnrealScripting | execute_python | Explicit opt-in full-trust Python, with project checks and execution receipts |

Interface add/list/remove uses the proven UE 5.8 C++ BlueprintEditorUtils operations, not Slate clicks. A transport-successful tool call does not prove that a mutation compiled, saved or verified: inspect its structured receipt. Never blindly retry a partially applied write.

## Full-trust Python is off by default

An engine-wide auto-enabled plugin could otherwise expose arbitrary Python execution through the UE MCP server in every project. The full-trust execute_python tool does **not register by default**. Opt in explicitly by launching that trusted editor process with:

    CONTEXTFORGE_UNREAL_ENABLE_FULL_TRUST_PYTHON=1

This setting is not a sandbox or authorization bypass. ContextForge must still classify the tool as WRITE and enforce its approval process. The native Blueprint and PIE tools do not require this opt-in.

## Verification and boundaries

- The plugin is editor-only and preserves Epic's official MCP server, transport and direct tool schemas.
- Prebuilt binaries match **one exact Unreal Engine build**. Rebuild after engine updates or for another installation.
- Tool changes are discoverable through the adapter's existing native MCP tool-list refresh.
- StartPIE returns only REQUESTED; VerifyPIE and project-specific assertions are required for actual gameplay validation. VerifyRestoration proves the test session ended.
- The regression suite includes a disposable CFSmoke.uproject fixture and test/ue58-smoke.py. Only the marker CF_SMOKE ALL_TESTS_PASSED from the editor log is accepted as native smoke success. Graphical multiplayer gameplay testing remains separate.
- If a deployment is interrupted, inspect status and any retained .ContextForgeUnrealTools.backup-* folder before repair. Do not delete unknown engine files.
