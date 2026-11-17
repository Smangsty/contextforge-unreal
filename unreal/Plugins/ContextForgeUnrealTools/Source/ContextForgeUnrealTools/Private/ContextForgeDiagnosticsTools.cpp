#include "ContextForgeDiagnosticsTools.h"

#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/UICommandInfo.h"
#include "Framework/Docking/TabManager.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "IAnimationProvider.h"
#include "IGameplayProvider.h"
#include "IRewindDebugger.h"
#include "IRewindDebuggerModule.h"
#include "Kismet2/DebuggerCommands.h"
#include "Misc/App.h"

#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "UObject/UnrealType.h"

namespace CFD
{
    using FObject = TSharedPtr<FJsonObject>;
    using FValues = TArray<TSharedPtr<FJsonValue>>;

    static FObject NewObject() { return MakeShared<FJsonObject>(); }
    static void Add(FValues& Items, const FObject& Obj)
    {
        Items.Add(MakeShared<FJsonValueObject>(Obj));
    }

    static FString Serialize(const FObject& Obj)
    {
        FString Result;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
        FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
        return Result;
    }

    static FString Error(const FString& Message)
    {
        FObject Obj = NewObject();
        Obj->SetBoolField(TEXT("ok"), false);
        Obj->SetStringField(TEXT("error"), Message);
        return Serialize(Obj);
    }

    static FObject Vector(const FVector& Value)
    {
        FObject Obj = NewObject();
        Obj->SetNumberField(TEXT("x"), Value.X);
        Obj->SetNumberField(TEXT("y"), Value.Y);
        Obj->SetNumberField(TEXT("z"), Value.Z);
        return Obj;
    }

    static FObject Transform(const FTransform& Value)
    {
        FObject Obj = NewObject();
        Obj->SetObjectField(TEXT("location"), Vector(Value.GetLocation()));
        Obj->SetObjectField(TEXT("scale"), Vector(Value.GetScale3D()));
        Obj->SetStringField(TEXT("rotation"), Value.Rotator().ToCompactString());
        return Obj;
    }

    static bool Contains(const FString& Source, const FString& Fragment)
    {
        return Fragment.IsEmpty() || Source.Contains(Fragment, ESearchCase::IgnoreCase);
    }

    static TArray<UWorld*> LiveWorlds()
    {
        TArray<UWorld*> Out;
        if (GEngine)
        {
            for (const FWorldContext& Context : GEngine->GetWorldContexts())
            {
                if (UWorld* World = Context.World())
                {
                    if (Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game)
                    {
                        Out.AddUnique(World);
                    }
                }
            }
        }
        return Out;
    }

    static FString NetModeName(ENetMode Mode)
    {
        switch (Mode)
        {
        case NM_Standalone: return TEXT("Standalone");
        case NM_DedicatedServer: return TEXT("DedicatedServer");
        case NM_ListenServer: return TEXT("ListenServer");
        case NM_Client: return TEXT("Client");
        default: return TEXT("Unknown");
        }
    }

    static FObject WorldInfo(UWorld* World)
    {
        FObject Obj = NewObject();
        Obj->SetStringField(TEXT("world"), World->GetName());
        Obj->SetStringField(TEXT("netMode"), NetModeName(World->GetNetMode()));
        Obj->SetNumberField(TEXT("deltaSeconds"), World->GetDeltaSeconds());
        Obj->SetNumberField(TEXT("timeSeconds"), World->GetTimeSeconds());
        return Obj;
    }

    static IRewindDebugger* Rewind() { return IRewindDebugger::Instance(); }

    static const TraceServices::IAnalysisSession* Session()
    {
        IRewindDebugger* Debugger = Rewind();
        return Debugger ? Debugger->GetAnalysisSession() : nullptr;
    }

    static FString OpenRewind()
    {
        IRewindDebuggerModule* Module = FModuleManager::Get().LoadModulePtr<IRewindDebuggerModule>(TEXT("RewindDebugger"));
        if (!Module)
        {
            return Error(TEXT("Epic RewindDebugger module is not available"));
        }
        FGlobalTabmanager::Get()->TryInvokeTab(Module->GetMainTabName());
        return FString();
    }

    static FObject ActorInfo(AActor* Actor, UWorld* World)
    {
        FObject Obj = NewObject();
        Obj->SetStringField(TEXT("name"), Actor->GetName());
        Obj->SetStringField(TEXT("path"), Actor->GetPathName());
        Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
        Obj->SetStringField(TEXT("world"), World->GetName());
        Obj->SetStringField(TEXT("netMode"), NetModeName(World->GetNetMode()));
        Obj->SetObjectField(TEXT("location"), Vector(Actor->GetActorLocation()));
        Obj->SetObjectField(TEXT("velocity"), Vector(Actor->GetVelocity()));
        return Obj;
    }

    static FObject ReadNumericProperties(UObject* Target, const FString& Filter, int32 MaxProperties)
    {
        FObject Output = NewObject();
        int32 Count = 0;
        if (!Target) { return Output; }
        for (TFieldIterator<FProperty> It(Target->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            if (Count >= MaxProperties) { break; }
            FProperty* Property = *It;
            const FString Name = Property->GetName();
            if (!Contains(Name, Filter)) { continue; }
            if (const FBoolProperty* Bool = CastField<FBoolProperty>(Property))
            {
                Output->SetBoolField(Name, Bool->GetPropertyValue_InContainer(Target));
                ++Count;
            }
            else if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
            {
                const void* Value = Property->ContainerPtrToValuePtr<void>(Target);
                if (Numeric->IsFloatingPoint())
                {
                    Output->SetNumberField(Name, Numeric->GetFloatingPointPropertyValue(Value));
                    ++Count;
                }
                else if (Numeric->IsInteger())
                {
                    Output->SetNumberField(Name, static_cast<double>(Numeric->GetSignedIntPropertyValue(Value)));
                    ++Count;
                }
            }
        }
        return Output;
    }
}

FString UContextForgeDiagnosticsTools::GetDiagnosticsCapabilities()
{
    CFD::FObject Obj = CFD::NewObject();
    Obj->SetBoolField(TEXT("ok"), true);
    Obj->SetStringField(TEXT("scope"), TEXT("Engine-wide; no project, class, or asset paths hardcoded"));
    Obj->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
    Obj->SetBoolField(TEXT("rewindDebuggerLoaded"), FModuleManager::Get().IsModuleLoaded(TEXT("RewindDebugger")));
    Obj->SetBoolField(TEXT("rewindInitialized"), CFD::Rewind() != nullptr);
    Obj->SetNumberField(TEXT("activeGameWorlds"), CFD::LiveWorlds().Num());
    Obj->SetStringField(TEXT("tools"), TEXT("Rewind status/open/start/stop; bounded animation trace; live actors, pose, animation states and properties; network diagnostics; curated stat overlays"));
    return CFD::Serialize(Obj);
}

FString UContextForgeDiagnosticsTools::GetRewindStatus()
{
    CFD::FObject Obj = CFD::NewObject();
    IRewindDebugger* Debugger = CFD::Rewind();
    Obj->SetBoolField(TEXT("ok"), true);
    Obj->SetBoolField(TEXT("initialized"), Debugger != nullptr);
    Obj->SetBoolField(TEXT("moduleLoaded"), FModuleManager::Get().IsModuleLoaded(TEXT("RewindDebugger")));
    if (Debugger)
    {
        Obj->SetBoolField(TEXT("recording"), Debugger->IsRecording());
        Obj->SetBoolField(TEXT("canRecord"), Debugger->CanStartRecording());
        Obj->SetBoolField(TEXT("pieSimulating"), Debugger->IsPIESimulating());
        Obj->SetBoolField(TEXT("analysisAvailable"), Debugger->GetAnalysisSession() != nullptr);
        Obj->SetNumberField(TEXT("recordingDurationSeconds"), Debugger->GetRecordingDuration());
        Obj->SetNumberField(TEXT("currentTraceSeconds"), Debugger->CurrentTraceTime());
        Obj->SetNumberField(TEXT("scrubSeconds"), Debugger->GetScrubTime());
        Obj->SetStringField(TEXT("rootObjectId"), LexToString(Debugger->GetRootObjectId()));
    }
    return CFD::Serialize(Obj);
}

FString UContextForgeDiagnosticsTools::OpenRewindDebugger()
{
    const FString Failure = CFD::OpenRewind();
    return Failure.IsEmpty() ? GetRewindStatus() : Failure;
}

FString UContextForgeDiagnosticsTools::StartRewindRecording()
{
    const FString Failure = CFD::OpenRewind();
    if (!Failure.IsEmpty()) { return Failure; }
    IRewindDebugger* Debugger = CFD::Rewind();
    if (!Debugger) { return CFD::Error(TEXT("RewindDebugger did not initialize")); }
    if (Debugger->IsRecording()) { return GetRewindStatus(); }
    if (!Debugger->CanStartRecording())
    {
        return CFD::Error(TEXT("Rewind recording cannot start; launch PIE and inspect Rewind status"));
    }
    Debugger->StartRecording();
    return GetRewindStatus();
}

FString UContextForgeDiagnosticsTools::StopRewindRecording()
{
    IRewindDebugger* Debugger = CFD::Rewind();
    if (!Debugger) { return CFD::Error(TEXT("RewindDebugger not initialized")); }
    if (!Debugger->IsRecording()) { return GetRewindStatus(); }

    TSharedPtr<FUICommandInfo> StopCommand = FInputBindingManager::Get().FindCommandInContext(
        FName(TEXT("RewindDebugger")), FName(TEXT("StopRecording")));
    if (!StopCommand.IsValid() || !FPlayWorldCommands::GlobalPlayWorldActions.IsValid())
    {
        return CFD::Error(TEXT("Epic's local StopRecording action is not registered"));
    }
    if (!FPlayWorldCommands::GlobalPlayWorldActions->TryExecuteAction(StopCommand.ToSharedRef()))
    {
        return CFD::Error(TEXT("Epic's local StopRecording action could not execute"));
    }
    return GetRewindStatus();
}

FString UContextForgeDiagnosticsTools::ListRewindAnimations(const FString& NameFilter, int32 MaxInstances)
{
    const TraceServices::IAnalysisSession* Session = CFD::Session();
    if (!Session) { return CFD::Error(TEXT("No Rewind analysis session is available")); }
    TraceServices::FAnalysisSessionReadScope Lock(*Session);
    const IAnimationProvider* Provider = Session->ReadProvider<IAnimationProvider>(TEXT("AnimationProvider"));
    const IGameplayProvider* Gameplay = Session->ReadProvider<IGameplayProvider>(TEXT("GameplayProvider"));
    if (!Provider) { return CFD::Error(TEXT("No AnimationProvider found in the selected trace")); }
    const int32 Limit = FMath::Clamp(MaxInstances, 1, 100);
    CFD::FValues Instances;
    int32 Total = 0;
    Provider->EnumerateAnimGraphTimelines([&](uint64 Id, const IAnimationProvider::AnimGraphTimeline&)
    {
        ++Total;
        FString Name;
        if (Gameplay)
        {
            if (const FObjectInfo* Info = Gameplay->FindObjectInfo(Id))
            {
                Name = Info->Name ? FString(Info->Name) : FString();
            }
        }
        if (!CFD::Contains(Name, NameFilter) || Instances.Num() >= Limit) { return; }
        CFD::FObject Item = CFD::NewObject();
        Item->SetStringField(TEXT("instanceId"), LexToString(Id));
        Item->SetStringField(TEXT("name"), Name);
        Instances.Add(MakeShared<FJsonValueObject>(Item));
    });
    CFD::FObject Obj = CFD::NewObject();
    Obj->SetBoolField(TEXT("ok"), true);
    Obj->SetArrayField(TEXT("instances"), Instances);
    Obj->SetNumberField(TEXT("totalRecordedInstances"), Total);
    Obj->SetBoolField(TEXT("truncated"), Instances.Num() >= Limit);
    return CFD::Serialize(Obj);
}

FString UContextForgeDiagnosticsTools::ReadRewindAnimation(const FString& AnimInstanceId, float LastSeconds, int32 MaxEvents)
{
    IRewindDebugger* Debugger = CFD::Rewind();
    const TraceServices::IAnalysisSession* Session = CFD::Session();
    if (!Debugger || !Session) { return CFD::Error(TEXT("No active Rewind analysis session")); }
        if (AnimInstanceId.IsEmpty() || AnimInstanceId.Len() > 20) { return CFD::Error(TEXT("Invalid AnimInstanceId; use a decimal ID from ListRewindAnimations")); }
    for (int32 Index = 0; Index < AnimInstanceId.Len(); ++Index)
    {
        if (!FChar::IsDigit(AnimInstanceId[Index]))
        {
            return CFD::Error(TEXT("AnimInstanceId must be a decimal integer"));
        }
    }
    const uint64 Id = FCString::Strtoui64(*AnimInstanceId, nullptr, 10);
    if (Id == 0) { return CFD::Error(TEXT("AnimInstanceId must be nonzero")); }

    const double End = Debugger->CurrentTraceTime();
    const double Start = FMath::Max(0.0, End - FMath::Clamp(static_cast<double>(LastSeconds), 0.1, 60.0));
    const int32 Limit = FMath::Clamp(MaxEvents, 1, 500);

    CFD::FValues Events;
    bool bState = false, bSequences = false, bBlendSpaces = false, bValues = false;

    TraceServices::FAnalysisSessionReadScope Lock(*Session);
    const IAnimationProvider* Provider = Session->ReadProvider<IAnimationProvider>(TEXT("AnimationProvider"));
    if (!Provider) { return CFD::Error(TEXT("AnimationProvider unavailable")); }

    auto MakeEvent = [&](const TCHAR* Type, double Time) -> CFD::FObject
    {
        CFD::FObject Item = CFD::NewObject();
        Item->SetStringField(TEXT("type"), Type);
        Item->SetNumberField(TEXT("time"), Time);
        return Item;
    };

    bState = Provider->ReadStateMachinesTimeline(Id, [&](const IAnimationProvider::StateMachinesTimeline& Timeline)
    {
        Timeline.EnumerateEvents(Start, End, [&](double S, double E, uint32 Depth, const FAnimStateMachineMessage& M)
        {
            if (Events.Num() < Limit)
            {
                CFD::FObject Item = MakeEvent(TEXT("state"), S);
                Item->SetNumberField(TEXT("machineIndex"), M.StateMachineIndex);
                Item->SetNumberField(TEXT("stateIndex"), M.StateIndex);
                Item->SetNumberField(TEXT("weight"), M.StateWeight);
                Item->SetNumberField(TEXT("elapsed"), M.ElapsedTime);
                CFD::Add(Events, Item);
            }
            return TraceServices::EEventEnumerate::Continue;
        });
    });

    bSequences = Provider->ReadAnimSequencePlayersTimeline(Id, [&](const IAnimationProvider::AnimSequencePlayersTimeline& Timeline)
    {
        Timeline.EnumerateEvents(Start, End, [&](double S, double E, uint32 Depth, const FAnimSequencePlayerMessage& M)
        {
            if (Events.Num() < Limit)
            {
                CFD::FObject Item = MakeEvent(TEXT("sequence"), S);
                Item->SetNumberField(TEXT("nodeId"), M.NodeId);
                Item->SetNumberField(TEXT("position"), M.Position);
                Item->SetNumberField(TEXT("length"), M.Length);
                CFD::Add(Events, Item);
            }
            return TraceServices::EEventEnumerate::Continue;
        });
    });

    bBlendSpaces = Provider->ReadAnimBlendSpacePlayersTimeline(Id, [&](const IAnimationProvider::BlendSpacePlayersTimeline& Timeline)
    {
        Timeline.EnumerateEvents(Start, End, [&](double S, double E, uint32 Depth, const FBlendSpacePlayerMessage& M)
        {
            if (Events.Num() < Limit)
            {
                CFD::FObject Item = MakeEvent(TEXT("blendSpace"), S);
                Item->SetNumberField(TEXT("nodeId"), M.NodeId);
                Item->SetNumberField(TEXT("x"), M.PositionX);
                Item->SetNumberField(TEXT("y"), M.PositionY);
                Item->SetNumberField(TEXT("filteredX"), M.FilteredPositionX);
                Item->SetNumberField(TEXT("filteredY"), M.FilteredPositionY);
                Item->SetStringField(TEXT("assetId"), LexToString(M.BlendSpaceId));
                CFD::Add(Events, Item);
            }
            return TraceServices::EEventEnumerate::Continue;
        });
    });

    bValues = Provider->ReadAnimNodeValuesTimeline(Id, [&](const IAnimationProvider::AnimNodeValuesTimeline& Timeline)
    {
        Timeline.EnumerateEvents(Start, End, [&](double S, double E, uint32 Depth, const FAnimNodeValueMessage& M)
        {
            if (Events.Num() < Limit)
            {
                CFD::FObject Item = MakeEvent(TEXT("nodeValue"), S);
                Item->SetNumberField(TEXT("nodeId"), M.NodeId);
                Item->SetStringField(TEXT("name"), M.Key ? FString(M.Key) : FString());
                Item->SetStringField(TEXT("value"), Provider->FormatNodeValue(M).ToString());
                CFD::Add(Events, Item);
            }
            return TraceServices::EEventEnumerate::Continue;
        });
    });

    CFD::FObject Obj = CFD::NewObject();
    Obj->SetBoolField(TEXT("ok"), true);
    Obj->SetStringField(TEXT("instanceId"), LexToString(Id));
    Obj->SetNumberField(TEXT("startTraceTime"), Start);
    Obj->SetNumberField(TEXT("endTraceTime"), End);
    Obj->SetBoolField(TEXT("stateTimeline"), bState);
    Obj->SetBoolField(TEXT("sequenceTimeline"), bSequences);
    Obj->SetBoolField(TEXT("blendSpaceTimeline"), bBlendSpaces);
    Obj->SetBoolField(TEXT("nodeValuesTimeline"), bValues);
    Obj->SetBoolField(TEXT("truncated"), Events.Num() == Limit);
    Obj->SetArrayField(TEXT("events"), Events);
    return CFD::Serialize(Obj);
}

FString UContextForgeDiagnosticsTools::ListLiveActors(const FString& ActorNameFilter, int32 MaxActors)
{
    CFD::FValues Worlds, Actors;
    const int32 Limit = FMath::Clamp(MaxActors, 1, 200);
    int32 Total = 0;
    for (UWorld* World : CFD::LiveWorlds())
    {
        CFD::Add(Worlds, CFD::WorldInfo(World));
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor || !CFD::Contains(Actor->GetName(), ActorNameFilter)) { continue; }
            ++Total;
            if (Actors.Num() < Limit) { CFD::Add(Actors, CFD::ActorInfo(Actor, World)); }
        }
    }
    CFD::FObject Obj = CFD::NewObject();
    Obj->SetBoolField(TEXT("ok"), true);
    Obj->SetArrayField(TEXT("worlds"), Worlds);
    Obj->SetArrayField(TEXT("actors"), Actors);
    Obj->SetNumberField(TEXT("matchedActors"), Total);
    Obj->SetBoolField(TEXT("truncated"), Total > Limit);
    return CFD::Serialize(Obj);
}

FString UContextForgeDiagnosticsTools::InspectLiveActor(const FString& ActorNameFilter, const FString& ComponentFilter, const FString& BoneNamesCsv, const FString& PropertyNameFilter, int32 MaxActors)
{
    if (ActorNameFilter.IsEmpty())
    {
        return CFD::Error(TEXT("ActorNameFilter is required; discover actors with ListLiveActors first"));
    }
    const int32 Limit = FMath::Clamp(MaxActors, 1, 8);
    CFD::FValues Actors;
    TArray<FString> BoneNames;
    BoneNamesCsv.ParseIntoArray(BoneNames, TEXT(","), true);
    if (BoneNames.Num() > 12) { BoneNames.SetNum(12); }

    for (UWorld* World : CFD::LiveWorlds())
    {
        for (TActorIterator<AActor> It(World); It && Actors.Num() < Limit; ++It)
        {
            AActor* Actor = *It;
            if (!Actor || !CFD::Contains(Actor->GetName(), ActorNameFilter)) { continue; }
            CFD::FObject Info = CFD::ActorInfo(Actor, World);
            Info->SetObjectField(TEXT("actorProperties"), CFD::ReadNumericProperties(Actor, PropertyNameFilter, 40));
            CFD::FValues Components;

            TArray<USkeletalMeshComponent*> Meshes;
            Actor->GetComponents<USkeletalMeshComponent>(Meshes);
            for (USkeletalMeshComponent* Mesh : Meshes)
            {
                if (!Mesh || !CFD::Contains(Mesh->GetName(), ComponentFilter)) { continue; }
                CFD::FObject Item = CFD::NewObject();
                Item->SetStringField(TEXT("name"), Mesh->GetName());
                Item->SetStringField(TEXT("attachSocket"), Mesh->GetAttachSocketName().ToString());
                Item->SetStringField(TEXT("parent"), Mesh->GetAttachParent() ? Mesh->GetAttachParent()->GetName() : FString());
                Item->SetObjectField(TEXT("transform"), CFD::Transform(Mesh->GetComponentTransform()));
                Item->SetStringField(TEXT("skeletalMesh"), Mesh->GetSkeletalMeshAsset() ? Mesh->GetSkeletalMeshAsset()->GetPathName() : FString());
                if (UAnimInstance* Anim = Mesh->GetAnimInstance())
                {
                    Item->SetStringField(TEXT("animClass"), Anim->GetClass()->GetPathName());
                    Item->SetObjectField(TEXT("animProperties"), CFD::ReadNumericProperties(Anim, PropertyNameFilter, 65));
                    if (const UAnimBlueprintGeneratedClass* Generated = Cast<UAnimBlueprintGeneratedClass>(Anim->GetClass()))
                    {
                        CFD::FValues Machines;
                        for (const FBakedAnimationStateMachine& Machine : Generated->BakedStateMachines)
                        {
                            const int32 Index = Anim->GetStateMachineIndex(Machine.MachineName);
                            if (Index < 0) { continue; }
                            CFD::FObject State = CFD::NewObject();
                            State->SetStringField(TEXT("machine"), Machine.MachineName.ToString());
                            State->SetStringField(TEXT("activeState"), Anim->GetCurrentStateName(Index).ToString());
                            CFD::Add(Machines, State);
                        }
                        Item->SetArrayField(TEXT("stateMachines"), Machines);
                    }
                }
                CFD::FValues Bones;
                for (FString Bone : BoneNames)
                {
                    Bone.TrimStartAndEndInline();
                    const FName Name(*Bone);
                    const int32 BoneIndex = Mesh->GetBoneIndex(Name);
                    if (BoneIndex == INDEX_NONE && !Mesh->DoesSocketExist(Name)) { continue; }
                    CFD::FObject B = CFD::NewObject();
                    B->SetStringField(TEXT("name"), Bone);
                    B->SetBoolField(TEXT("isBone"), BoneIndex != INDEX_NONE);
                    B->SetObjectField(TEXT("worldTransform"), CFD::Transform(Mesh->GetSocketTransform(Name)));
                    CFD::Add(Bones, B);
                }
                Item->SetArrayField(TEXT("bonesAndSockets"), Bones);
                CFD::Add(Components, Item);
            }

            TArray<UCameraComponent*> Cameras;
            Actor->GetComponents<UCameraComponent>(Cameras);
            for (UCameraComponent* Camera : Cameras)
            {
                CFD::FObject Item = CFD::NewObject();
                Item->SetStringField(TEXT("name"), Camera->GetName());
                Item->SetStringField(TEXT("parent"), Camera->GetAttachParent() ? Camera->GetAttachParent()->GetName() : FString());
                Item->SetStringField(TEXT("socket"), Camera->GetAttachSocketName().ToString());
                Item->SetNumberField(TEXT("fieldOfView"), Camera->FieldOfView);
                Item->SetObjectField(TEXT("transform"), CFD::Transform(Camera->GetComponentTransform()));
                CFD::Add(Components, Item);
            }

            Info->SetArrayField(TEXT("components"), Components);
            CFD::Add(Actors, Info);
        }
    }
    CFD::FObject Obj = CFD::NewObject();
    Obj->SetBoolField(TEXT("ok"), true);
    Obj->SetArrayField(TEXT("actors"), Actors);
    Obj->SetBoolField(TEXT("truncated"), Actors.Num() >= Limit);
    return CFD::Serialize(Obj);
}

FString UContextForgeDiagnosticsTools::GetNetworkDiagnostics()
{
    CFD::FValues Worlds;
    for (UWorld* World : CFD::LiveWorlds())
    {
        CFD::FObject Obj = CFD::WorldInfo(World);
        if (UNetDriver* Driver = World->GetNetDriver())
        {
            Obj->SetStringField(TEXT("netDriver"), Driver->GetName());
            Obj->SetNumberField(TEXT("serverConnections"), Driver->ClientConnections.Num());
            Obj->SetBoolField(TEXT("hasServerConnection"), Driver->ServerConnection != nullptr);
        }
        CFD::Add(Worlds, Obj);
    }
    CFD::FObject Obj = CFD::NewObject();
    Obj->SetBoolField(TEXT("ok"), true);
    Obj->SetArrayField(TEXT("worlds"), Worlds);
    Obj->SetNumberField(TEXT("frameCounter"), static_cast<double>(GFrameCounter));
    Obj->SetNumberField(TEXT("appDeltaSeconds"), FApp::GetDeltaTime());
    return CFD::Serialize(Obj);
}

FString UContextForgeDiagnosticsTools::ToggleDebugOverlay(const FString& Overlay)
{
    static const TSet<FString> Allowed = {
        TEXT("stat fps"), TEXT("stat unit"), TEXT("stat unitgraph"),
        TEXT("stat game"), TEXT("stat anim"), TEXT("stat net"),
        TEXT("stat physics"), TEXT("showdebug animation"),
        TEXT("showdebug enhancedinput"), TEXT("showdebug mover")
    };
    FString Command = Overlay.TrimStartAndEnd().ToLower();
    if (!Allowed.Contains(Command))
    {
        return CFD::Error(TEXT("Only curated stat/showdebug overlays are allowed"));
    }
    UWorld* World = CFD::LiveWorlds().IsEmpty() ? nullptr : CFD::LiveWorlds()[0];
    if (!World || !GEngine) { return CFD::Error(TEXT("No active game/PIE world")); }
    const bool bDispatched = GEngine->Exec(World, *Command);
    CFD::FObject Obj = CFD::NewObject();
    Obj->SetBoolField(TEXT("ok"), bDispatched);
    Obj->SetStringField(TEXT("command"), Command);
    Obj->SetStringField(TEXT("world"), World->GetName());
    return CFD::Serialize(Obj);
}
