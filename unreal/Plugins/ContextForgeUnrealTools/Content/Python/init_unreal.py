"""Only register full-trust Python execution when explicitly opted in."""
import os
import unreal

if os.environ.get("CONTEXTFORGE_UNREAL_ENABLE_FULL_TRUST_PYTHON") == "1":
    from contextforge_unreal_scripting import ContextForgeUnrealScripting

    if not unreal.ToolsetRegistry.is_toolset_class_registered(ContextForgeUnrealScripting):
        unreal.ToolsetRegistry.register_toolset_class(ContextForgeUnrealScripting)

    if unreal.ToolsetRegistry.is_toolset_class_registered(ContextForgeUnrealScripting):
        unreal.log("[ContextForgeUnrealTools] Full-trust Python toolset registered by explicit opt-in")
    else:
        unreal.log_error("[ContextForgeUnrealTools] Python toolset registration failed")
else:
    unreal.log("[ContextForgeUnrealTools] Full-trust Python execution disabled (default)")
