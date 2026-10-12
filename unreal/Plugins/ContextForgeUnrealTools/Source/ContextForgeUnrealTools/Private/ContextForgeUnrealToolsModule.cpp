#include "ContextForgeBlueprintTools.h"
#include "ContextForgePIETools.h"

#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

class FContextForgeUnrealToolsModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        if (UToolsetRegistry::IsAvailable())
        {
            UToolsetRegistry::RegisterToolsetClass(UContextForgeBlueprintTools::StaticClass());
            UToolsetRegistry::RegisterToolsetClass(UContextForgePIETools::StaticClass());
        }
    }

    virtual void ShutdownModule() override
    {
        if (UObjectInitialized() && UToolsetRegistry::IsAvailable())
        {
            UToolsetRegistry::UnregisterToolsetClass(UContextForgePIETools::StaticClass());
            UToolsetRegistry::UnregisterToolsetClass(UContextForgeBlueprintTools::StaticClass());
        }
    }
};

IMPLEMENT_MODULE(FContextForgeUnrealToolsModule, ContextForgeUnrealTools)
