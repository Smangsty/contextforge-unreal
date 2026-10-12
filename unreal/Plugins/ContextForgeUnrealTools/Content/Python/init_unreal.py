"""Register the optional ContextForge Python tools with Epic's native UE 5.8 registry."""
import unreal
from toolset_registry.registration import Registration
from contextforge_unreal_scripting import ContextForgeUnrealScripting

_registration = Registration([ContextForgeUnrealScripting])
_registration.register()

if unreal.ToolsetRegistry.is_toolset_class_registered(ContextForgeUnrealScripting):
    unreal.log("[ContextForgeUnrealTools] Python toolset registered")
else:
    unreal.log_error("[ContextForgeUnrealTools] Python toolset registration failed")
