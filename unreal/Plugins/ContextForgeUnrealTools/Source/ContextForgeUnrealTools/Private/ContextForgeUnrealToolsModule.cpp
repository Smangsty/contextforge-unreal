#include "ContextForgeBlueprintTools.h"
#include "ContextForgeAnimationTools.h"
#include "ContextForgePIETools.h"
#include "ContextForgeDiagnosticsTools.h"

#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

class FContextForgeUnrealToolsModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        UToolsetRegistry::RegisterToolsetClass(UContextForgeBlueprintTools::StaticClass());
        UToolsetRegistry::RegisterToolsetClass(UContextForgeAnimationTools::StaticClass());
        UToolsetRegistry::RegisterToolsetClass(UContextForgePIETools::StaticClass());
        UToolsetRegistry::RegisterToolsetClass(UContextForgeDiagnosticsTools::StaticClass());
    }

    virtual void ShutdownModule() override
    {
        UToolsetRegistry::UnregisterToolsetClass(UContextForgeDiagnosticsTools::StaticClass());
        UToolsetRegistry::UnregisterToolsetClass(UContextForgePIETools::StaticClass());
        UToolsetRegistry::UnregisterToolsetClass(UContextForgeAnimationTools::StaticClass());
        UToolsetRegistry::UnregisterToolsetClass(UContextForgeBlueprintTools::StaticClass());
    }
};

IMPLEMENT_MODULE(FContextForgeUnrealToolsModule, ContextForgeUnrealTools)
