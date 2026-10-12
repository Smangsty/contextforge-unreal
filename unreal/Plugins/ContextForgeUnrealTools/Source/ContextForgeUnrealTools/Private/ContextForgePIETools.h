#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "ContextForgePIETools.generated.h"

USTRUCT(BlueprintType)
struct FCFPIEPeerProof
{
    GENERATED_BODY()

    UPROPERTY() int32 PIEInstance = INDEX_NONE;
    UPROPERTY() FString NetMode;
    UPROPERTY() FString WorldName;
    UPROPERTY() FString ControllerClass;
    UPROPERTY() FString PawnClass;
    UPROPERTY() FString PawnRole;
    UPROPERTY() FString PawnRemoteRole;
    UPROPERTY() bool bLocalControllerFound = false;
    UPROPERTY() bool bPossessionVerified = false;
    UPROPERTY() bool bExpectedClassesVerified = false;
    UPROPERTY() bool bRequiredComponentFound = false;
    UPROPERTY() FString Detail;
};

USTRUCT(BlueprintType)
struct FCFPIETestReceipt
{
    GENERATED_BODY()

    UPROPERTY() FString ProjectPath;
    UPROPERTY() FString MapPackage;
    UPROPERTY() FString Outcome;
    UPROPERTY() FString Detail;
    UPROPERTY() bool bOwnsSession = false;
    UPROPERTY() bool bStartRequested = false;
    UPROPERTY() bool bEndRequested = false;
    UPROPERTY() bool bRestored = false;
    UPROPERTY() bool bGameplayVerified = false;
    UPROPERTY() int32 RequestedClients = 0;
    UPROPERTY() int32 ObservedPIEWorlds = 0;
    UPROPERTY() TArray<FCFPIEPeerProof> Peers;
    UPROPERTY() TArray<FString> SessionErrors;
};

/** Asynchronous PIE lifecycle, evidence-based possession verification and editor restoration. */
UCLASS(BlueprintType)
class UContextForgePIETools : public UToolsetDefinition
{
    GENERATED_BODY()

public:
    /** Start PIE only on an explicitly identified, clean, already-loaded map.
     * Result REQUESTED is not a gameplay PASS: poll VerifyPIE afterward.
     */
    UFUNCTION(meta=(AICallable), Category="ContextForge Unreal|PIE")
    static FCFPIETestReceipt StartPIE(
        const FString& ExpectedMapPackage, int32 Clients = 1,
        bool bListenServer = false);

    /** Inspect actual PIE worlds and local player possession, class identity, role and component.
     * Returns PASS only after all expected peers meet the caller's assertions.
     */
    UFUNCTION(meta=(AICallable), Category="ContextForge Unreal|PIE")
    static FCFPIETestReceipt VerifyPIE(
        const FString& ExpectedControllerClass, const FString& ExpectedPawnClass,
        const FString& RequiredPawnComponentClass = TEXT(""));

    /** Request ending this tool's PIE session only. Poll VerifyRestoration for actual completion. */
    UFUNCTION(meta=(AICallable), Category="ContextForge Unreal|PIE")
    static FCFPIETestReceipt StopPIE();

    /** Verify no PIE worlds remain and the editor still owns the original map.
     * Only this check can return RESTORED.
     */
    UFUNCTION(meta=(AICallable), Category="ContextForge Unreal|PIE")
    static FCFPIETestReceipt VerifyRestoration();
};
