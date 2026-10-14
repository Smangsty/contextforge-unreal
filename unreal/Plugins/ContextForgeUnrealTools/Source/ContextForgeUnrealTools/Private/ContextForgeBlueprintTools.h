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
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString ResultNodeGuid;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") TArray<FString> AffectedNodeGuids;
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

/** Graph-space pin center: exact when a live Slate pin-image geometry is available. */
USTRUCT(BlueprintType)
struct FCFGraphPinGeometry
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString PinGuid;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString PinName;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString Direction;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString Category;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bHidden = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bMeasured = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") float X = 0.f;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") float Y = 0.f;
};

/** An explicit absolute graph-space node placement chosen by the caller, never a solver. */
USTRUCT(BlueprintType)
struct FCFGraphNodePlacement
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ContextForge") FString NodeGuid;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ContextForge") int32 X = 0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ContextForge") int32 Y = 0;
};

/** Persisted graph-space location plus clearly labeled rendered/estimated extent. */
USTRUCT(BlueprintType)
struct FCFGraphNodeGeometry
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString NodeGuid;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString Title;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString NodeClass;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") int32 X = 0;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") int32 Y = 0;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") int32 Width = 0;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") int32 Height = 0;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString SizeSource;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bComment = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString CommentText;
    /** Comment-only fields: Unreal's authored appearance and tooltip details. */
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString CommentDetails;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FLinearColor CommentColor = FLinearColor::Transparent;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") int32 CommentFontSize = 0;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") int32 CommentDepth = 0;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bCommentBubbleVisible = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bCommentBubbleColored = false;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") bool bCommentMovesContainedNodes = false;
    /** Unreal's transient drag-membership set; not equivalent to geometry containment. */
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") TArray<FString> CommentDragMemberGuids;
    /** Nodes (including nested comments) geometrically enclosed by this comment. */
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") TArray<FString> GeometricContainedNodeGuids;
    /** Comments geometrically enclosing this node or comment. */
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") TArray<FString> GeometricParentCommentGuids;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") TArray<FCFGraphPinGeometry> Pins;
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
    /** Includes persisted node coordinates and comment bounds, unlike the wiring fingerprint above. */
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") FString LayoutFingerprint;
    UPROPERTY(BlueprintReadOnly, Category="ContextForge") TArray<FCFGraphNodeGeometry> NodeGeometry;
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

    /** Place exact GUID-addressed nodes at chosen graph coordinates, atomically and undoably. */
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|Blueprints")
    static FCFBlueprintReceipt PlaceGraphNodes(
        const FString& BlueprintPath, const FString& GraphName,
        const FString& ExpectedLayoutFingerprint, const TArray<FCFGraphNodePlacement>& Placements,
        bool bAllowCommentMembershipChange = false, bool bRequireMeasuredGeometry = false,
        bool bSave = false);

    /** Create/update exactly one styled comment at caller-selected bounds with reviewed contents. */
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|Blueprints")
    static FCFBlueprintReceipt SetGraphComment(
        const FString& BlueprintPath, const FString& GraphName,
        const FString& ExpectedLayoutFingerprint, const FString& ExistingCommentGuid,
        const TArray<FString>& EnclosedNodeGuids,
        const FString& Title, const FString& Details, const FLinearColor& CommentColor,
        int32 X, int32 Y, int32 Width, int32 Height, int32 FontSize,
        bool bShowBubble, bool bColorBubble, bool bGroupMovement, bool bSave = false);

    /** Legacy opt-in automatic layout; prefer PlaceGraphNodes for deliberate hand placement. */
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|Blueprints")
    static FCFBlueprintReceipt FormatGraphCluster(
        const FString& BlueprintPath, const FString& GraphName,
        const FString& ExpectedLayoutFingerprint, const TArray<FString>& NodeGuids,
        int32 HorizontalSpacing = 120, int32 VerticalSpacing = 60,
        bool bRequireMeasuredGeometry = false, bool bSave = false);

    /** Create or update one comment box around exactly the selected node cluster. */
    UFUNCTION(BlueprintCallable, meta=(AICallable), Category="ContextForge Unreal|Blueprints")
    static FCFBlueprintReceipt UpsertGraphComment(
        const FString& BlueprintPath, const FString& GraphName,
        const FString& ExpectedLayoutFingerprint, const TArray<FString>& NodeGuids,
        const FString& Title, const FString& ExistingCommentGuid,
        const FLinearColor& CommentColor, bool bSave = false);

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
