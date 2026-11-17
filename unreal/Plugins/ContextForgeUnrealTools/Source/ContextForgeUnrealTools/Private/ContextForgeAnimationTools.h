#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "ContextForgeBlueprintTools.h"
#include "ContextForgeAnimationTools.generated.h"

/** An inspected, actual state graph and its selected single asset player. */
USTRUCT(BlueprintType)
struct FCFAnimStateInfo
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString Name;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString NodeGuid;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString GraphName;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString AssetPath;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString AssetType;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bPoseConnected = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bEntry = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") int32 PlayerCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bHasInheritedCallbacks = false;
};

/** One concrete state transition and its actual rule wiring. */
USTRUCT(BlueprintType)
struct FCFAnimTransitionInfo
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString NodeGuid;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString FromState;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString ToState;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString RuleGraph;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString RuleVariable;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bNegated = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bRuleConnected = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") float BlendSeconds = 0.f;
};

/** Bounded graph readback and stale-write precondition, not a compile receipt. */
USTRUCT(BlueprintType)
struct FCFAnimMachineSnapshot
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString ProjectPath;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString BlueprintPath;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString MachineName;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString MachineNodeGuid;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString Fingerprint;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bSuccess = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bConnectedToFinalPose = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bHasEntry = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString Error;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") TArray<FCFAnimStateInfo> States;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") TArray<FCFAnimTransitionInfo> Transitions;
};

/** A validation receipt that distinguishes structural evidence from compiler truth. */
USTRUCT(BlueprintType)
struct FCFAnimValidationReceipt
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString BlueprintPath;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString MachineName;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString Fingerprint;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bStructureValid = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bCompileAttempted = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bCompileSucceeded = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") int32 ErrorCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") int32 WarningCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") TArray<FString> Issues;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") TArray<FCFCompilerDiagnostic> Diagnostics;
};

/** Native scoped tools for UE 5.8 AnimBlueprint state graphs, not free-form Python. */
UCLASS(BlueprintType)
class UContextForgeAnimationTools : public UToolsetDefinition
{
    GENERATED_BODY()
public:
    /** Inspect the exact machine, states, assets, entry and transition rules. READ. */
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|Animation")
    static FCFAnimMachineSnapshot InspectStateMachine(
        const FString& BlueprintPath, const FString& MachineName);

    /** Add one clean state machine node to an owned AnimGraph, never connect it implicitly. */
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|Animation")
    static FCFBlueprintReceipt EnsureStateMachine(
        const FString& BlueprintPath, const FString& MachineName,
        int32 X, int32 Y, bool bSave = false);

    /** Make/update a single-asset state only after matching the machine fingerprint. */
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|Animation")
    static FCFBlueprintReceipt UpsertAnimationState(
        const FString& BlueprintPath, const FString& MachineName,
        const FString& ExpectedFingerprint, const FString& StateName,
        const FString& AnimationAssetPath, bool bLoop,
        bool bMakeEntry, int32 X, int32 Y, bool bSave = false);

    /** Make an explicit boolean AnimBP-variable rule; never copy donor callbacks. */
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|Animation")
    static FCFBlueprintReceipt UpsertStateTransition(
        const FString& BlueprintPath, const FString& MachineName,
        const FString& ExpectedFingerprint, const FString& FromState,
        const FString& ToState, const FString& BooleanVariable,
        bool bNegate, float BlendSeconds, bool bSave = false);

    /** Structural inspection plus the real Blueprint compiler with exact diagnostics. */
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|Animation")
    static FCFAnimValidationReceipt ValidateStateMachine(
        const FString& BlueprintPath, const FString& MachineName,
        bool bCompile = true);
};
