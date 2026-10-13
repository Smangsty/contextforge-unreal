#include "ContextForgePIETools.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Misc/OutputDevice.h"
#include "Misc/Paths.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif
#include "Misc/ScopeLock.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UObject/StrongObjectPtr.h"

namespace ContextForgePIE
{
    class FSessionErrors final : public FOutputDevice
    {
    public:
        virtual bool CanBeUsedOnAnyThread() const override { return true; }

        virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity,
                               const FName& Category) override
        {
            // Strip Unreal output-device flags before classifying severity.
            const ELogVerbosity::Type Level = static_cast<ELogVerbosity::Type>(
                Verbosity & ELogVerbosity::VerbosityMask);
            if (Level != ELogVerbosity::Fatal &&
                Level != ELogVerbosity::Error &&
                Level != ELogVerbosity::Warning) return;
            FScopeLock Lock(&Mutex);
            TArray<FString>& Target = Level == ELogVerbosity::Warning ? Warnings : ErrorLines;
            if (Target.Num() >= 100) Target.RemoveAt(0);
            const FString Line = Category.ToString() + TEXT(": ") + FString(Text);
            Target.Add(Line.Left(2048));
        }

        void Reset()
        {
            FScopeLock Lock(&Mutex);
            ErrorLines.Reset();
            Warnings.Reset();
        }

        TArray<FString> GetErrors() const
        {
            FScopeLock Lock(&Mutex);
            return ErrorLines;
        }

        TArray<FString> GetWarnings() const
        {
            FScopeLock Lock(&Mutex);
            return Warnings;
        }

    private:
        mutable FCriticalSection Mutex;
        TArray<FString> ErrorLines;
        TArray<FString> Warnings;
    };

    static FSessionErrors Errors;
    static TStrongObjectPtr<ULevelEditorPlaySettings> TemporarySettings;
    static FString ExpectedMap;
    static FString SessionProject;
    static int32 ExpectedWorlds = 0;
    static bool bOwned = false;
    static bool bRequested = false;
    static bool bEndRequested = false;
    static bool bLogging = false;

    static FString Project()
    {
        return FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
    }

    static UWorld* EditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext(false).World() : nullptr;
    }

    static FString EditorMap()
    {
        UWorld* World = EditorWorld();
        return World && World->GetOutermost() ? World->GetOutermost()->GetName() : FString();
    }

    static int32 PIEWorldCount()
    {
        if (!GEngine) return 0;
        int32 Count = 0;
        for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
        {
            if (WorldContext.WorldType == EWorldType::PIE && WorldContext.World())
            {
                ++Count;
            }
        }
        return Count;
    }

    static FString Mode(ENetMode NetMode)
    {
        switch (NetMode)
        {
        case NM_Standalone: return TEXT("Standalone");
        case NM_Client: return TEXT("Client");
        case NM_ListenServer: return TEXT("ListenServer");
        case NM_DedicatedServer: return TEXT("DedicatedServer");
        default: return TEXT("Unknown");
        }
    }

    static FString Role(ENetRole NetRole)
    {
        switch (NetRole)
        {
        case ROLE_None: return TEXT("None");
        case ROLE_SimulatedProxy: return TEXT("SimulatedProxy");
        case ROLE_AutonomousProxy: return TEXT("AutonomousProxy");
        case ROLE_Authority: return TEXT("Authority");
        default: return TEXT("Unknown");
        }
    }

    static FCFPIETestReceipt Receipt(const FString& Outcome, const FString& Detail)
    {
        FCFPIETestReceipt Result;
        Result.ProjectPath = Project();
        Result.MapPackage = ExpectedMap;
        Result.Outcome = Outcome;
        Result.Detail = Detail;
        Result.bOwnsSession = bOwned;
        Result.bStartRequested = bRequested;
        Result.bEndRequested = bEndRequested;
        Result.RequestedClients = ExpectedWorlds;
        Result.ObservedPIEWorlds = PIEWorldCount();
        if (bLogging)
        {
            Result.SessionErrors = Errors.GetErrors();
            Result.SessionWarnings = Errors.GetWarnings();
        }
        return Result;
    }

    static void RemoveLog()
    {
        if (bLogging && GLog)
        {
            GLog->RemoveOutputDevice(&Errors);
        }
        bLogging = false;
    }

    static void ReleaseSession()
    {
        RemoveLog();
        TemporarySettings.Reset();
        bOwned = false;
        bRequested = false;
        bEndRequested = false;
        ExpectedWorlds = 0;
        ExpectedMap.Empty();
        SessionProject.Empty();
        Errors.Reset();
    }
}

FCFPIETestReceipt UContextForgePIETools::StartPIE(
    const FString& ExpectedMapPackage, int32 Clients, bool bListenServer)
{
    if (!GEditor || !GEngine)
    {
        return ContextForgePIE::Receipt(TEXT("EDITOR_UNAVAILABLE"), TEXT("No editor engine is available."));
    }
    if (ContextForgePIE::bOwned || GEditor->IsPlayingSessionInEditor() ||
        GEditor->GetPlaySessionRequest().IsSet() || ContextForgePIE::PIEWorldCount() > 0)
    {
        return ContextForgePIE::Receipt(TEXT("PIE_ALREADY_ACTIVE"),
            TEXT("Cannot take ownership of an existing or pending play session."));
    }
    if (Clients < 1 || Clients > 4 || (Clients > 1 && !bListenServer))
    {
        return ContextForgePIE::Receipt(TEXT("INVALID_TEST_CONFIGURATION"),
            TEXT("Use 1-4 local PIE clients; multi-client testing requires listen server."));
    }
    UWorld* World = ContextForgePIE::EditorWorld();
    const FString Map = ContextForgePIE::EditorMap();
    if (!World || ExpectedMapPackage.IsEmpty() || Map != ExpectedMapPackage ||
        !World->GetOutermost() || World->GetOutermost()->IsDirty())
    {
        return ContextForgePIE::Receipt(TEXT("TEST_MAP_NOT_READY"),
            TEXT("Load the exact disposable map first and ensure that map is not dirty. No map was opened or saved."));
    }

    const ULevelEditorPlaySettings* CurrentSettings = GetDefault<ULevelEditorPlaySettings>();
    ULevelEditorPlaySettings* Copy = DuplicateObject<ULevelEditorPlaySettings>(
        CurrentSettings, GetTransientPackage());
    if (!Copy)
    {
        return ContextForgePIE::Receipt(TEXT("PLAY_SETTINGS_COPY_FAILED"),
            TEXT("Unable to duplicate editor play settings without modifying user preferences."));
    }
    ContextForgePIE::TemporarySettings.Reset(Copy);
    Copy->SetPlayNumberOfClients(Clients);
    Copy->SetRunUnderOneProcess(true);
    Copy->SetPlayNetMode(
        bListenServer ? EPlayNetMode::PIE_ListenServer : EPlayNetMode::PIE_Standalone);

    ContextForgePIE::ExpectedMap = Map;
    ContextForgePIE::SessionProject = ContextForgePIE::Project();
    ContextForgePIE::ExpectedWorlds = Clients;
    ContextForgePIE::bOwned = true;
    ContextForgePIE::bRequested = true;
    ContextForgePIE::bEndRequested = false;
    ContextForgePIE::Errors.Reset();
    if (GLog)
    {
        GLog->AddOutputDevice(&ContextForgePIE::Errors);
        ContextForgePIE::bLogging = true;
    }

    FRequestPlaySessionParams Params;
    Params.WorldType = EPlaySessionWorldType::PlayInEditor;
    Params.SessionDestination = EPlaySessionDestinationType::InProcess;
    Params.EditorPlaySettings = Copy;
    Params.bAllowOnlineSubsystem = false;

    GEditor->RequestPlaySession(Params);
    return ContextForgePIE::Receipt(TEXT("REQUESTED"),
        TEXT("PIE launch was requested. Poll VerifyPIE; dispatch success is not gameplay success."));
}

FCFPIETestReceipt UContextForgePIETools::VerifyPIE(
    const FString& ExpectedControllerClass, const FString& ExpectedPawnClass,
    const FString& RequiredPawnComponentClass)
{
    if (!ContextForgePIE::bOwned || ContextForgePIE::SessionProject != ContextForgePIE::Project())
    {
        return ContextForgePIE::Receipt(TEXT("UNOWNED_SESSION"),
            TEXT("StartPIE must own the active session for this exact project."));
    }
    if (ExpectedControllerClass.IsEmpty() || ExpectedPawnClass.IsEmpty())
    {
        return ContextForgePIE::Receipt(TEXT("MISSING_EXPECTATIONS"),
            TEXT("Expected controller and pawn class paths must be explicit."));
    }
    UClass* ExpectedController = LoadObject<UClass>(nullptr, *ExpectedControllerClass);
    UClass* ExpectedPawn = LoadObject<UClass>(nullptr, *ExpectedPawnClass);
    UClass* ExpectedComponent = RequiredPawnComponentClass.IsEmpty() ? nullptr :
        LoadObject<UClass>(nullptr, *RequiredPawnComponentClass);
    if (!ExpectedController || !ExpectedController->IsChildOf(APlayerController::StaticClass()) ||
        !ExpectedPawn || !ExpectedPawn->IsChildOf(APawn::StaticClass()) ||
        (!RequiredPawnComponentClass.IsEmpty() &&
            (!ExpectedComponent || !ExpectedComponent->IsChildOf(UActorComponent::StaticClass()))))
    {
        return ContextForgePIE::Receipt(TEXT("INVALID_CLASS_EXPECTATION"),
            TEXT("Supply valid class paths for controller, pawn and optional component."));
    }

    FCFPIETestReceipt Result = ContextForgePIE::Receipt(
        TEXT("IN_PROGRESS"), TEXT("PIE results are not yet verified."));
    int32 VerifiedWorlds = 0;
    for (const FWorldContext& Context : GEngine->GetWorldContexts())
    {
        if (Context.WorldType != EWorldType::PIE || !Context.World()) continue;

        UWorld* World = Context.World();
        FCFPIEPeerProof Proof;
        Proof.PIEInstance = Context.PIEInstance;
        Proof.NetMode = ContextForgePIE::Mode(World->GetNetMode());
        Proof.WorldName = World->GetName();
        Proof.bRequiredComponentFound = ExpectedComponent == nullptr;
        int32 LocalControllers = 0;

        for (TActorIterator<APlayerController> It(World); It; ++It)
        {
            APlayerController* Controller = *It;
            if (!Controller || !Controller->IsLocalController()) continue;
            ++LocalControllers;
            Proof.bLocalControllerFound = true;
            Proof.ControllerClass = Controller->GetClass()->GetPathName();

            APawn* Pawn = Controller->GetPawn();
            Proof.bPossessionVerified = Pawn != nullptr && Pawn->GetController() == Controller;
            if (Pawn)
            {
                Proof.PawnClass = Pawn->GetClass()->GetPathName();
                Proof.PawnRole = ContextForgePIE::Role(Pawn->GetLocalRole());
                Proof.PawnRemoteRole = ContextForgePIE::Role(Pawn->GetRemoteRole());
                Proof.bRequiredComponentFound = ExpectedComponent == nullptr ||
                    Pawn->FindComponentByClass(ExpectedComponent) != nullptr;
            }
            Proof.bExpectedClassesVerified =
                Controller->IsA(ExpectedController) && Pawn && Pawn->IsA(ExpectedPawn);
        }
        Proof.Detail = LocalControllers == 1 ? TEXT("Inspected one local player controller.") :
            FString::Printf(TEXT("Expected exactly one local controller; observed %d."), LocalControllers);

        const bool bPeerVerified = LocalControllers == 1 &&
            Proof.bPossessionVerified && Proof.bExpectedClassesVerified &&
            Proof.bRequiredComponentFound;
        if (bPeerVerified) ++VerifiedWorlds;
        Result.Peers.Add(MoveTemp(Proof));
    }

    Result.ObservedPIEWorlds = Result.Peers.Num();
    if (!GEditor->IsPlayingSessionInEditor() ||
        Result.ObservedPIEWorlds < ContextForgePIE::ExpectedWorlds)
    {
        Result.Outcome = TEXT("STARTING");
        Result.Detail = TEXT("Requested play worlds not all available; retry readback later.");
    }
    else
    {
        Result.bGameplayVerified =
            VerifiedWorlds == ContextForgePIE::ExpectedWorlds &&
            Result.ObservedPIEWorlds == ContextForgePIE::ExpectedWorlds &&
            Result.SessionErrors.IsEmpty() && !ContextForgePIE::bEndRequested;
        Result.Outcome = Result.bGameplayVerified ? TEXT("PASS") : TEXT("FAIL");
        if (Result.bGameplayVerified)
        {
            Result.Detail = Result.SessionWarnings.IsEmpty() ?
                TEXT("Observed possession and classes on all requested local PIE peers.") :
                TEXT("Verified all requested peers. Advisory sessionWarnings did not affect PASS.");
        }
        else
        {
            Result.Detail = Result.SessionErrors.IsEmpty() ?
                TEXT("At least one peer, class, possession, component or session assertion failed.") :
                TEXT("Session error/fatal messages were recorded; inspect sessionErrors.");
        }
    }
    return Result;
}

FCFPIETestReceipt UContextForgePIETools::StopPIE()
{
    if (!ContextForgePIE::bOwned || ContextForgePIE::SessionProject != ContextForgePIE::Project())
    {
        return ContextForgePIE::Receipt(TEXT("UNOWNED_SESSION"),
            TEXT("Refusing to end someone else's PIE session."));
    }
    if (!GEditor)
    {
        return ContextForgePIE::Receipt(TEXT("EDITOR_UNAVAILABLE"),
            TEXT("Cannot request PIE stop without an editor engine."));
    }
    if (!ContextForgePIE::bEndRequested)
    {
        // A queued PIE request is not yet an active session. Cancel it instead
        // of leaving a pending launch alive after a stop receipt.
        if (GEditor->GetPlaySessionRequest().IsSet())
        {
            GEditor->CancelRequestPlaySession();
        }
        else if (GEditor->IsPlayingSessionInEditor())
        {
            GEditor->RequestEndPlayMap();
        }
    }
    ContextForgePIE::bEndRequested = true;
    return ContextForgePIE::Receipt(TEXT("STOP_REQUESTED"),
        TEXT("Stop requested; poll VerifyRestoration for proof that PIE ended."));
}

FCFPIETestReceipt UContextForgePIETools::VerifyRestoration()
{
    if (!ContextForgePIE::bOwned || ContextForgePIE::SessionProject != ContextForgePIE::Project())
    {
        return ContextForgePIE::Receipt(TEXT("UNOWNED_SESSION"),
            TEXT("No owned test session is available for restoration verification."));
    }
    if (!ContextForgePIE::bEndRequested)
    {
        return ContextForgePIE::Receipt(TEXT("STOP_NOT_REQUESTED"),
            TEXT("StopPIE must be requested first."));
    }
    if (!GEditor || GEditor->IsPlayingSessionInEditor() ||
        GEditor->GetPlaySessionRequest().IsSet() || ContextForgePIE::PIEWorldCount() != 0)
    {
        return ContextForgePIE::Receipt(TEXT("STOPPING"),
            TEXT("PIE has not fully stopped; inspect again."));
    }
    FCFPIETestReceipt Result;
    const FString ExpectedMap = ContextForgePIE::ExpectedMap;
    const bool bRestored = ExpectedMap == ContextForgePIE::EditorMap();
    Result = ContextForgePIE::Receipt(
        bRestored ? TEXT("RESTORED") : TEXT("RESTORE_MISMATCH"),
        bRestored ? TEXT("All PIE worlds ended; original editor map is still loaded.") :
            TEXT("Editor map changed; inspect manually before continuing."));
    Result.bRestored = bRestored;
    if (bRestored)
    {
        ContextForgePIE::ReleaseSession();
    }
    return Result;
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FContextForgePIELogSeverityTest,
    "ContextForge.Unreal.PIE.SessionLogSeverity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FContextForgePIELogSeverityTest::RunTest(const FString& Parameters)
{
    ContextForgePIE::FSessionErrors Capture;
    const FName Category(TEXT("LogAudioMixerWasapi"));
    Capture.Serialize(TEXT("Device sample rate conversion will be used."),
                      ELogVerbosity::Warning, Category);
    TestEqual(TEXT("Advisory warning recorded"), Capture.GetWarnings().Num(), 1);
    TestEqual(TEXT("Warning alone is not a blocking error"), Capture.GetErrors().Num(), 0);

    Capture.Serialize(TEXT("Actual PIE failure"), ELogVerbosity::Error, Category);
    Capture.Serialize(TEXT("Fatal PIE failure"), ELogVerbosity::Fatal, Category);
    Capture.Serialize(TEXT("Ordinary display message"), ELogVerbosity::Display, Category);
    TestEqual(TEXT("Errors and fatals block PIE verification"), Capture.GetErrors().Num(), 2);
    TestEqual(TEXT("Only warning severity enters advisory list"), Capture.GetWarnings().Num(), 1);

    Capture.Reset();
    TestEqual(TEXT("Reset removes blocking errors"), Capture.GetErrors().Num(), 0);
    TestEqual(TEXT("Reset removes advisory warnings"), Capture.GetWarnings().Num(), 0);
    return true;
}
#endif
