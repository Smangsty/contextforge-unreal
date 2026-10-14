using UnrealBuildTool;

public class ContextForgeUnrealTools : ModuleRules
{
    public ContextForgeUnrealTools(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "ToolsetRegistry" });
        PrivateDependencyModuleNames.AddRange(new string[] {
            "UnrealEd", "BlueprintGraph", "Kismet", "KismetCompiler",
            "EditorScriptingUtilities", "MessageLog", "Projects",
            "GraphEditor", "Slate", "SlateCore"
        });
    }
}
