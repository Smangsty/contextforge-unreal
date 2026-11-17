#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "ContextForgeDiagnosticsTools.generated.h"

/**
 * Engine-wide, project-independent diagnostics. These tools inspect whichever
 * Unreal Editor world and actors are currently active. No game classes or asset
 * paths are assumed. Read methods never mutate gameplay assets.
 */
UCLASS(BlueprintType)
class UContextForgeDiagnosticsTools : public UToolsetDefinition
{
    GENERATED_BODY()

public:
    /** Report the native debug capabilities and available editor sessions. */
    UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "Diagnostics")
    static FString GetDiagnosticsCapabilities();

    /** Inspect Rewind Debugger availability, recording state and trace times. */
    UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "Diagnostics|Rewind")
    static FString GetRewindStatus();

    /** Open Epic's Rewind Debugger tab without changing game assets. */
    UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "Diagnostics|Rewind")
    static FString OpenRewindDebugger();

    /** Start Rewind Debugger's native PIE recording. */
    UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "Diagnostics|Rewind")
    static FString StartRewindRecording();

    /** Stop the local recording via Epic's registered Rewind command. */
    UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "Diagnostics|Rewind")
    static FString StopRewindRecording();

    /** Enumerate AnimInstance trace IDs in the currently selected Rewind trace. */
    UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "Diagnostics|Rewind")
    static FString ListRewindAnimations(const FString& NameFilter, int32 MaxInstances);

    /** Read a bounded window of animation state, sequence and BlendSpace events. */
    UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "Diagnostics|Rewind")
    static FString ReadRewindAnimation(const FString& AnimInstanceId, float LastSeconds, int32 MaxEvents);

    /** List active PIE/game worlds and matching actor instances. */
    UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "Diagnostics|Runtime")
    static FString ListLiveActors(const FString& ActorNameFilter, int32 MaxActors);

    /** Inspect actor, skeletal poses, animation booleans, states and cameras. */
    UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "Diagnostics|Runtime")
    static FString InspectLiveActor(const FString& ActorNameFilter, const FString& ComponentFilter, const FString& BoneNamesCsv, const FString& PropertyNameFilter, int32 MaxActors);

    /** Inspect per-world PIE network topology, client counts and connection state. */
    UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "Diagnostics|Network")
    static FString GetNetworkDiagnostics();

    /** Run one curated Epic stat/showdebug visualization, never arbitrary console commands. */
    UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "Diagnostics|Visual")
    static FString ToggleDebugOverlay(const FString& Overlay);
};
