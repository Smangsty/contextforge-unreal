#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "ContextForgePIETools.generated.h"

USTRUCT(BlueprintType)
struct FCFPIEPeerProof
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="ContextForge") int32 PIEInstance = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString NetMode;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString WorldName;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString ControllerClass;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString PawnClass;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString PawnRole;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString PawnRemoteRole;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bLocalControllerFound = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bPossessionVerified = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bExpectedClassesVerified = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bRequiredComponentFound = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString Detail;
};

USTRUCT(BlueprintType)
struct FCFPIETestReceipt
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString ProjectPath;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString MapPackage;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString Outcome;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString Detail;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bOwnsSession = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bStartRequested = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bEndRequested = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bRestored = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bGameplayVerified = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") int32 RequestedClients = 0;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") int32 ObservedPIEWorlds = 0;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") TArray<FCFPIEPeerProof> Peers;
    /** Only errors and fatals block gameplay verification. */
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") TArray<FString> SessionErrors;
    /** Advisory warnings are visible but do not block a gameplay PASS. */
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") TArray<FString> SessionWarnings;
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
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|PIE")
    static FCFPIETestReceipt StartPIE(
        const FString& ExpectedMapPackage, int32 Clients = 1,
        bool bListenServer = false);

    /** Inspect actual PIE worlds and local player possession, class identity, role and component.
     * Returns PASS only after all expected peers meet the caller's assertions.
     */
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|PIE")
    static FCFPIETestReceipt VerifyPIE(
        const FString& ExpectedControllerClass, const FString& ExpectedPawnClass,
        const FString& RequiredPawnComponentClass = TEXT(""));

    /** Request ending this tool's PIE session only. Poll VerifyRestoration for actual completion. */
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|PIE")
    static FCFPIETestReceipt StopPIE();

    /** Verify no PIE worlds remain and the editor still owns the original map.
     * Only this check can return RESTORED.
     */
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|PIE")
    static FCFPIETestReceipt VerifyRestoration();
};
