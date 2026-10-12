#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "ContextForgeBlueprintTools.generated.h"

/** One actual compiler message; graph/node identity is provided when Unreal exposes it. */
USTRUCT(BlueprintType)
struct FCFCompilerDiagnostic
{
    GENERATED_BODY()

    UPROPERTY() FString Severity;
    UPROPERTY() FString Message;
    UPROPERTY() FString Graph;
    UPROPERTY() FString NodeGuid;
};

/** Distinguishes mutation, compiler and optional save stages, including partial success. */
USTRUCT(BlueprintType)
struct FCFBlueprintReceipt
{
    GENERATED_BODY()

    UPROPERTY() FString ProjectPath;
    UPROPERTY() FString BlueprintPath;
    UPROPERTY() FString Outcome;
    UPROPERTY() FString Detail;
    UPROPERTY() bool bTargetValidated = false;
    UPROPERTY() bool bMutationAttempted = false;
    UPROPERTY() bool bMutationApplied = false;
    UPROPERTY() bool bInitializationCompleted = false;
    UPROPERTY() bool bReadbackVerified = false;
    UPROPERTY() bool bCompileAttempted = false;
    UPROPERTY() bool bCompileSucceeded = false;
    UPROPERTY() int32 ErrorCount = 0;
    UPROPERTY() int32 WarningCount = 0;
    UPROPERTY() bool bSaveAttempted = false;
    UPROPERTY() bool bSaved = false;
    UPROPERTY() bool bSafeToRetryCreation = false;
    UPROPERTY() TArray<FCFCompilerDiagnostic> Diagnostics;
};

/** Native and Blueprint interfaces, including interfaces inherited through parent classes. */
USTRUCT(BlueprintType)
struct FCFInterfaceInfo
{
    GENERATED_BODY()

    UPROPERTY() FString ClassPath;
    UPROPERTY() bool bDirect = false;
    UPROPERTY() bool bInherited = false;
};

USTRUCT(BlueprintType)
struct FCFInterfaceList
{
    GENERATED_BODY()

    UPROPERTY() FString ProjectPath;
    UPROPERTY() FString BlueprintPath;
    UPROPERTY() bool bSuccess = false;
    UPROPERTY() FString Error;
    UPROPERTY() TArray<FCFInterfaceInfo> Interfaces;
};

/** Stable node identity and sorted pin/link pairs for graph preflight and readback. */
USTRUCT(BlueprintType)
struct FCFGraphSnapshot
{
    GENERATED_BODY()

    UPROPERTY() FString ProjectPath;
    UPROPERTY() FString BlueprintPath;
    UPROPERTY() FString GraphName;
    UPROPERTY() bool bSuccess = false;
    UPROPERTY() FString Error;
    UPROPERTY() TArray<FString> NodeGuids;
    UPROPERTY() TArray<FString> PinDetails;
    UPROPERTY() TArray<FString> Connections;
    UPROPERTY() FString Fingerprint;
};

/** Safe additions, removals, compilation and narrowly scoped graph edits. */
UCLASS(BlueprintType)
class UContextForgeBlueprintTools : public UToolsetDefinition
{
    GENERATED_BODY()

public:
    /** List interfaces, distinguishing direct and inherited declarations. */
    UFUNCTION(meta=(AICallable), Category="ContextForge Unreal|Blueprints")
    static FCFInterfaceList ListInterfaces(const FString& BlueprintPath);

    /** Add a native or Blueprint interface idempotently and verify readback. */
    UFUNCTION(meta=(AICallable), Category="ContextForge Unreal|Blueprints")
    static FCFBlueprintReceipt AddInterface(
        const FString& BlueprintPath, const FString& InterfaceClassPath, bool bSave = false);

    /** Remove only a directly implemented interface. Removing graphs requires explicit consent. */
    UFUNCTION(meta=(AICallable), Category="ContextForge Unreal|Blueprints")
    static FCFBlueprintReceipt RemoveInterface(
        const FString& BlueprintPath, const FString& InterfaceClassPath,
        bool bPreserveFunctions = true, bool bConfirmDestructive = false, bool bSave = false);

    /** Compile and return actual compiler messages, counts and node identity when present. */
    UFUNCTION(meta=(AICallable), Category="ContextForge Unreal|Blueprints")
    static FCFBlueprintReceipt CompileWithDiagnostics(
        const FString& BlueprintPath, bool bWarningsAsErrors = false);

    /** Create one SCS component without duplicates, reporting creation even if compile fails. */
    UFUNCTION(meta=(AICallable), Category="ContextForge Unreal|Blueprints")
    static FCFBlueprintReceipt AddComponentVerified(
        const FString& BlueprintPath, const FString& ComponentClassPath,
        const FString& ComponentName, bool bSave = false);

    /** Read stable graph node GUIDs and connections without modifying the Blueprint. */
    UFUNCTION(meta=(AICallable), Category="ContextForge Unreal|Blueprints")
    static FCFGraphSnapshot GetGraphSnapshot(
        const FString& BlueprintPath, const FString& GraphName);

    /** Replace exactly one existing link after strict preconditions; preserve every other link. */
    UFUNCTION(meta=(AICallable), Category="ContextForge Unreal|Blueprints")
    static FCFBlueprintReceipt RewireGraphConnection(
        const FString& BlueprintPath, const FString& GraphName,
        const FString& ExpectedFingerprint,
        const FString& SourceNodeGuid, const FString& SourcePin,
        const FString& OldTargetNodeGuid, const FString& OldTargetPin,
        const FString& NewTargetNodeGuid, const FString& NewTargetPin,
        bool bSave = false);
};
