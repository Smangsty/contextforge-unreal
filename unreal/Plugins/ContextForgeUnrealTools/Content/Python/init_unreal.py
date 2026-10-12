"""Register optional editor-side ContextForge Python tools with Epic's ToolsetRegistry."""
import unreal
from contextforge_unreal_scripting import ContextForgeUnrealScripting

if not unreal.ToolsetRegistry.is_toolset_class_registered(ContextForgeUnrealScripting):
    unreal.ToolsetRegistry.register_toolset_class(ContextForgeUnrealScripting)

if unreal.ToolsetRegistry.is_toolset_class_registered(ContextForgeUnrealScripting):
    unreal.log("[ContextForgeUnrealTools] Python toolset registered")
else:
    unreal.log_error("[ContextForgeUnrealTools] Python toolset registration failed")
