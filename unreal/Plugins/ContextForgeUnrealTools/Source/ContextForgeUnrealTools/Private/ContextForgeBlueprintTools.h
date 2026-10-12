#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "ContextForgeBlueprintTools.generated.h"

/** One actual compiler message; graph/node identity is provided when Unreal exposes it. */
USTRUCT(BlueprintType)
struct FCFCompilerDiagnostic
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString Severity;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString Message;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString Graph;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString NodeGuid;
};

/** Distinguishes mutation, compiler and optional save stages, including partial success. */
USTRUCT(BlueprintType)
struct FCFBlueprintReceipt
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString ProjectPath;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString BlueprintPath;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString Outcome;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString Detail;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bTargetValidated = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bMutationAttempted = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bMutationApplied = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bInitializationCompleted = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bReadbackVerified = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bCompileAttempted = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bCompileSucceeded = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") int32 ErrorCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") int32 WarningCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bSaveAttempted = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bSaved = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bSafeToRetryCreation = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") TArray<FCFCompilerDiagnostic> Diagnostics;
};

/** Native and Blueprint interfaces, including interfaces inherited through parent classes. */
USTRUCT(BlueprintType)
struct FCFInterfaceInfo
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString ClassPath;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bDirect = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bInherited = false;
};

USTRUCT(BlueprintType)
struct FCFInterfaceList
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString ProjectPath;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString BlueprintPath;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bSuccess = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString Error;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") TArray<FCFInterfaceInfo> Interfaces;
};

/** Stable node identity and sorted pin/link pairs for graph preflight and readback. */
USTRUCT(BlueprintType)
struct FCFGraphSnapshot
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString ProjectPath;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString BlueprintPath;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString GraphName;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bSuccess = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString Error;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") TArray<FString> NodeGuids;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") TArray<FString> PinDetails;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") TArray<FString> Connections;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString Fingerprint;
};

/** Safe additions, removals, compilation and narrowly scoped graph edits. */
UCLASS(BlueprintType)
class UContextForgeBlueprintTools : public UToolsetDefinition
{
    GENERATED_BODY()

public:
    /** List interfaces, distinguishing direct and inherited declarations. */
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|Blueprints")
    static FCFInterfaceList ListInterfaces(const FString& BlueprintPath);

    /** Add a native or Blueprint interface idempotently and verify readback. */
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|Blueprints")
    static FCFBlueprintReceipt AddInterface(
        const FString& BlueprintPath, const FString& InterfaceClassPath, bool bSave = false);

    /** Remove only a directly implemented interface. Removing graphs requires explicit consent. */
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|Blueprints")
    static FCFBlueprintReceipt RemoveInterface(
        const FString& BlueprintPath, const FString& InterfaceClassPath,
        bool bPreserveFunctions = true, bool bConfirmDestructive = false, bool bSave = false);

    /** Compile and return actual compiler messages, counts and node identity when present. */
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|Blueprints")
    static FCFBlueprintReceipt CompileWithDiagnostics(
        const FString& BlueprintPath, bool bWarningsAsErrors = false);

    /** Create one SCS component without duplicates, reporting creation even if compile fails. */
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|Blueprints")
    static FCFBlueprintReceipt AddComponentVerified(
        const FString& BlueprintPath, const FString& ComponentClassPath,
        const FString& ComponentName, bool bSave = false);

    /** Read stable graph node GUIDs and connections without modifying the Blueprint. */
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|Blueprints")
    static FCFGraphSnapshot GetGraphSnapshot(
        const FString& BlueprintPath, const FString& GraphName);

    /** Replace exactly one existing link after strict preconditions; preserve every other link. */
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|Blueprints")
    static FCFBlueprintReceipt RewireGraphConnection(
        const FString& BlueprintPath, const FString& GraphName,
        const FString& ExpectedFingerprint,
        const FString& SourceNodeGuid, const FString& SourcePin,
        const FString& OldTargetNodeGuid, const FString& OldTargetPin,
        const FString& NewTargetNodeGuid, const FString& NewTargetPin,
        bool bSave = false);
};
