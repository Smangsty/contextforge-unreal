"""Only register full-trust Python execution when explicitly opted in."""
import os
import unreal

# Restore Epic BlueprintTools' node discovery and creation on nested animation
# state graphs without changing installed Unreal Engine source files.
# Full-trust Python execution remains opt-in and unrelated to this patch.
try:
    from contextforge_unreal_blueprint_compat import install as _install_blueprint_compat
    _compat_result = _install_blueprint_compat()
    unreal.log("[ContextForgeUnrealTools] Nested graph compatibility: " + _compat_result)
except Exception as exc:
    unreal.log_error("[ContextForgeUnrealTools] Nested graph compatibility failed: " + str(exc))

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
