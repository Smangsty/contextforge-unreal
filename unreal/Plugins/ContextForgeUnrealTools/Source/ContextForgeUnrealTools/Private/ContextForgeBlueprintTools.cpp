#include "ContextForgeBlueprintTools.h"
#include "BPALLayeredLayout.h"

#include "Components/ActorComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Knot.h"
#include "GraphEditor.h"
#include "SGraphPanel.h"
#include "SGraphNode.h"
#include "SGraphPin.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "Misc/SecureHash.h"

namespace ContextForgeBlueprint
{
    static FString CurrentProject()
    {
        return FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
    }

    static FString AssetObjectPath(const FString& Path)
    {
        if (!Path.StartsWith(TEXT("/Game/")) || Path.Contains(TEXT("..")) ||
            Path.Contains(TEXT("'")) || Path.Contains(TEXT(":")) ||
            Path.Contains(TEXT("\\") ) || Path.Contains(TEXT("\n")))
        {
            return FString();
        }

        const FString Package = Path.Contains(TEXT(".")) ?
            FPackageName::ObjectPathToPackageName(Path) : Path;
        if (!FPackageName::IsValidLongPackageName(Package))
        {
            return FString();
        }
        return Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPackageName::GetShortName(Path);
    }

    static UBlueprint* FindBlueprint(const FString& Path)
    {
        const FString ObjectPath = AssetObjectPath(Path);
        return ObjectPath.IsEmpty() ? nullptr : LoadObject<UBlueprint>(nullptr, *ObjectPath);
    }

    static UClass* FindInterfaceClass(const FString& Path)
    {
        if (!Path.StartsWith(TEXT("/Script/")) && !Path.StartsWith(TEXT("/Game/")))
        {
            return nullptr;
        }
        UClass* Class = LoadObject<UClass>(nullptr, *Path);
        if (!Class && Path.StartsWith(TEXT("/Game/")))
        {
            FString ObjectPath = AssetObjectPath(Path);
            if (!ObjectPath.IsEmpty())
            {
                UBlueprint* InterfaceBlueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
                if (InterfaceBlueprint)
                {
                    Class = InterfaceBlueprint->GeneratedClass;
                }
                if (!Class)
                {
                    Class = LoadObject<UClass>(nullptr, *(ObjectPath + TEXT("_C")));
                }
            }
        }
        return Class && Class->HasAnyClassFlags(CLASS_Interface) ? Class : nullptr;
    }

    static FCFBlueprintReceipt Receipt(UBlueprint* Blueprint, const FString& Path)
    {
        FCFBlueprintReceipt Result;
        Result.ProjectPath = CurrentProject();
        Result.BlueprintPath = Path;
        Result.bTargetValidated = Blueprint != nullptr;
        Result.Outcome = Blueprint ? TEXT("PREFLIGHT_PASSED") : TEXT("INVALID_BLUEPRINT_TARGET");
        Result.Detail = Blueprint ? FString() : TEXT("Expected an unambiguous /Game Blueprint package or object path.");
        return Result;
    }

    static void Compile(UBlueprint* Blueprint, FCFBlueprintReceipt& Result, bool bWarningsAsErrors = false)
    {
        if (!Blueprint) return;
        FCompilerResultsLog Log(false);
        Result.bCompileAttempted = true;
        Result.Diagnostics.Reset();
        Result.ErrorCount = 0;
        Result.WarningCount = 0;
        FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Log);
        Result.ErrorCount = Log.NumErrors;
        Result.WarningCount = Log.NumWarnings;
        for (const TSharedRef<FTokenizedMessage>& Message : Log.Messages)
        {
            FCFCompilerDiagnostic Diagnostic;
            Diagnostic.Message = Message->ToText().ToString();
            switch (Message->GetSeverity())
            {
            case EMessageSeverity::Error: Diagnostic.Severity = TEXT("error"); break;
            case EMessageSeverity::Warning: Diagnostic.Severity = TEXT("warning"); break;
            default: Diagnostic.Severity = TEXT("info"); break;
            }
            // GetNodesFromTokens is private in UE 5.8. Preserve the exact
            // compiler message rather than inventing a node or graph identity.
            // Graph and node fields remain empty unless a public API proves them.
            Result.Diagnostics.Add(MoveTemp(Diagnostic));
        }
        Result.bCompileSucceeded =
            Log.NumErrors == 0 && Blueprint->Status != BS_Error &&
            (!bWarningsAsErrors || Log.NumWarnings == 0);
        if (!Result.bCompileSucceeded && Result.Diagnostics.IsEmpty())
        {
            FCFCompilerDiagnostic Diagnostic;
            Diagnostic.Severity = TEXT("error");
            Diagnostic.Message = TEXT("Blueprint compile did not succeed; compiler reported no tokenized diagnostics.");
            Result.Diagnostics.Add(MoveTemp(Diagnostic));
        }
    }

    static void Complete(UBlueprint* Blueprint, FCFBlueprintReceipt& Result, bool bSave)
    {
        if (Result.bCompileAttempted && !Result.bCompileSucceeded)
        {
            Result.Outcome = Result.bMutationApplied ? TEXT("MUTATION_APPLIED_COMPILE_FAILED") : TEXT("COMPILE_FAILED");
            Result.Detail = TEXT("Inspect the compiler diagnostics. Do not blindly repeat the mutation.");
            Result.bSafeToRetryCreation = false;
            return;
        }
        if (!Result.bReadbackVerified)
        {
            Result.Outcome = Result.bMutationApplied ? TEXT("MUTATION_UNVERIFIED") : TEXT("PREFLIGHT_FAILED");
            Result.Detail = TEXT("Readback did not prove the requested state. Inspect before retrying.");
            Result.bSafeToRetryCreation = false;
            return;
        }

        if (bSave && Blueprint)
        {
            Result.bSaveAttempted = true;
            Result.bSaved = UEditorAssetLibrary::SaveLoadedAsset(Blueprint, true);
            if (!Result.bSaved)
            {
                Result.Outcome = TEXT("APPLIED_SAVE_FAILED");
                Result.Detail = TEXT("Blueprint is modified but the target package could not be saved.");
                return;
            }
        }
        Result.Outcome = Result.bMutationApplied ? TEXT("APPLIED_VERIFIED") : TEXT("ALREADY_SATISFIED");
        Result.bSafeToRetryCreation = false;
    }

    // Some UE components initialize editor-owned shared settings during a scoped save.
    // Saving is opt-in, targets only this Blueprint, and always gets a real recompile.
    static void CompleteComponent(UBlueprint* Blueprint, USCS_Node* Node,
                                  FCFBlueprintReceipt& Result, bool bSave)
    {
        Result.bInitializationCompleted = Node && Node->ComponentTemplate != nullptr;
        Compile(Blueprint, Result);
        if (bSave && !Result.bCompileSucceeded)
        {
            Result.bSaveAttempted = true;
            Result.bSaved = UEditorAssetLibrary::SaveLoadedAsset(Blueprint, true);
            if (Result.bSaved)
            {
                Compile(Blueprint, Result);
                Result.bInitializationCompleted = Node && Node->ComponentTemplate != nullptr;
            }
        }
        if (bSave && Result.bSaved && Result.bCompileSucceeded)
        {
            // Compilation after initialization may dirty the same asset again.
            Result.bSaved = UEditorAssetLibrary::SaveLoadedAsset(Blueprint, true);
        }
        Complete(Blueprint, Result, bSave && !Result.bSaveAttempted);
    }

    static UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& Name)
    {
        if (!Blueprint || Name.IsEmpty()) return nullptr;
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        UEdGraph* Found = nullptr;
        for (UEdGraph* Graph : Graphs)
        {
            if (Graph && Graph->GetName() == Name)
            {
                if (Found) return nullptr;
                Found = Graph;
            }
        }
        return Found;
    }

    static UEdGraphNode* FindNode(UEdGraph* Graph, const FString& GuidText)
    {
        FGuid Guid;
        if (!Graph || !FGuid::Parse(GuidText, Guid)) return nullptr;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->NodeGuid == Guid) return Node;
        }
        return nullptr;
    }

    // Native node coordinates are always authoritative. The live graph panel provides
    // rendered extents and socket-widget geometry only when it has been arranged.
    static FCFGraphNodeGeometry DescribeNode(UEdGraphNode* Node, SGraphPanel* Panel)
    {
        FCFGraphNodeGeometry Info;
        if (!Node) return Info;
        Info.NodeGuid = Node->NodeGuid.ToString();
        Info.NodeClass = Node->GetClass()->GetPathName();
        Info.Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
        Info.X = Node->NodePosX;
        Info.Y = Node->NodePosY;
        Info.bComment = Node->IsA<UEdGraphNode_Comment>();
        Info.CommentText = Info.bComment ? Node->NodeComment : FString();
        if (UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node))
        {
            Info.CommentDetails = Comment->NodeDetails.ToString();
            Info.CommentColor = Comment->CommentColor;
            Info.CommentFontSize = Comment->FontSize;
            Info.CommentDepth = Comment->CommentDepth;
            Info.bCommentBubbleVisible = Comment->bCommentBubbleVisible_InDetailsPanel;
            Info.bCommentBubbleColored = Comment->bColorCommentBubble;
            Info.bCommentMovesContainedNodes = Comment->MoveMode == ECommentBoxMode::GroupMovement;
            for (UObject* Member : Comment->GetNodesUnderComment())
            {
                if (const UEdGraphNode* MemberNode = Cast<UEdGraphNode>(Member))
                    Info.CommentDragMemberGuids.Add(MemberNode->NodeGuid.ToString());
            }
            Info.CommentDragMemberGuids.Sort();
        }

        int32 Inputs = 0, Outputs = 0;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->bHidden) continue;
            if (Pin->Direction == EGPD_Input) ++Inputs;
            else if (Pin->Direction == EGPD_Output) ++Outputs;
        }
        Info.Width = FMath::Max(220, Node->NodeWidth);
        Info.Height = FMath::Max(90, FMath::Max(Node->NodeHeight, 48 + 26 * FMath::Max(Inputs, Outputs)));
        if (Node->IsA<UK2Node_Knot>())
        {
            // A collapsed reroute socket is not a 220x90 Blueprint node.
            Info.Width = 32;
            Info.Height = 24;
        }
        Info.SizeSource = TEXT("ESTIMATED");

        TSharedPtr<SGraphNode> Widget;
        if (Panel) Widget = Panel->GetNodeWidgetFromGuid(Node->NodeGuid);
        if (Info.bComment && Node->NodeWidth > 0 && Node->NodeHeight > 0)
        {
            Info.Width = Node->NodeWidth;
            Info.Height = Node->NodeHeight;
            Info.SizeSource = TEXT("STORED_COMMENT");
        }
        else if (Widget.IsValid())
        {
            const FVector2f Desired = Widget->GetDesiredSize();
            const FVector2f Cached = Widget->GetCachedGeometry().GetLocalSize();
            if (Desired.X > 1.f && Desired.Y > 1.f && Cached.X > 1.f && Cached.Y > 1.f)
            {
                Info.Width = FMath::CeilToInt(Desired.X);
                Info.Height = FMath::CeilToInt(Desired.Y);
                Info.SizeSource = TEXT("RENDERED");
            }
        }

        int32 InputIndex = 0, OutputIndex = 0;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin) continue;
            FCFGraphPinGeometry PinInfo;
            PinInfo.PinGuid = Pin->PinId.ToString();
            PinInfo.PinName = Pin->PinName.ToString();
            PinInfo.Category = Pin->PinType.PinCategory.ToString();
            PinInfo.Direction = Pin->Direction == EGPD_Input ? TEXT("INPUT") : TEXT("OUTPUT");
            PinInfo.bHidden = Pin->bHidden;
            if (!Pin->bHidden)
            {
                const int32 Slot = Pin->Direction == EGPD_Input ? InputIndex++ : OutputIndex++;
                PinInfo.X = float(Info.X + (Pin->Direction == EGPD_Input ? 0 : Info.Width));
                PinInfo.Y = float(Info.Y) + FMath::Min(float(Info.Height - 14), 55.f + 26.f * Slot);
                if (Widget.IsValid() && Info.SizeSource == TEXT("RENDERED"))
                {
                    TSharedPtr<SGraphPin> PinWidget = Widget->FindWidgetForPin(Pin);
                    TSharedPtr<SWidget> Socket = PinWidget.IsValid() ? PinWidget->GetPinImageWidget() : nullptr;
                    if (Socket.IsValid())
                    {
                        const FGeometry& SocketGeometry = Socket->GetCachedGeometry();
                        if (SocketGeometry.GetLocalSize().X > 1.f && SocketGeometry.GetLocalSize().Y > 1.f)
                        {
                            const FVector2f Center = SocketGeometry.GetLocalSize() * 0.5f;
                            const FVector2f Local = Widget->GetCachedGeometry().AbsoluteToLocal(
                                SocketGeometry.LocalToAbsolute(Center));
                            if (FMath::IsFinite(Local.X) && FMath::IsFinite(Local.Y))
                            {
                                PinInfo.X = float(Info.X) + Local.X;
                                PinInfo.Y = float(Info.Y) + Local.Y;
                                PinInfo.bMeasured = true;
                            }
                        }
                    }
                }
            }
            Info.Pins.Add(MoveTemp(PinInfo));
        }
        Info.Pins.Sort([](const FCFGraphPinGeometry& A, const FCFGraphPinGeometry& B)
        {
            return A.PinGuid < B.PinGuid;
        });
        return Info;
    }

    static FCFGraphSnapshot Snapshot(
        UBlueprint* Blueprint, const FString& Path, const FString& GraphName)
    {
        FCFGraphSnapshot Result;
        Result.ProjectPath = CurrentProject();
        Result.BlueprintPath = Path;
        Result.GraphName = GraphName;
        UEdGraph* Graph = FindGraph(Blueprint, GraphName);
        if (!Graph)
        {
            Result.Error = TEXT("Graph missing or graph name ambiguous.");
            return Result;
        }
        TArray<FString> FingerprintLines;
        TArray<FString> LayoutLines;
        SGraphPanel* Panel = nullptr;
        if (TSharedPtr<SGraphEditor> Editor = SGraphEditor::FindGraphEditorForGraph(Graph))
            Panel = Editor->GetGraphPanel();
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) continue;
            const FString Guid = Node->NodeGuid.ToString();
            Result.NodeGuids.Add(Guid);
            Result.NodeGeometry.Add(DescribeNode(Node, Panel));
            LayoutLines.Add(FString::Printf(TEXT("%s|%d|%d|%d|%d|%s"),
                *Guid, Node->NodePosX, Node->NodePosY, Node->NodeWidth, Node->NodeHeight,
                *Node->NodeComment));
            if (const UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node))
            {
                LayoutLines.Add(FString::Printf(TEXT("COMMENT|%s|%s|%s|%d|%d|%d|%d|%d"),
                    *Guid, *Comment->NodeDetails.ToString(), *Comment->CommentColor.ToString(),
                    Comment->FontSize, int32(Comment->bCommentBubbleVisible_InDetailsPanel),
                    int32(Comment->bColorCommentBubble),
                    int32(Comment->MoveMode.GetValue()), Comment->CommentDepth));
            }
            // Comment boxes are visual annotations, not executable graph structure.
            // Moving, styling, creating or deleting a comment must not invalidate
            // callers that guard wiring edits with the logic Fingerprint.
            if (!Node->IsA<UEdGraphNode_Comment>())
                FingerprintLines.Add(FString::Printf(TEXT("N|%s|%s"), *Guid, *Node->GetClass()->GetPathName()));
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin) continue;
                Result.PinDetails.Add(FString::Printf(TEXT("%s|%s|%s|%d"),
                    *Guid, *Pin->PinId.ToString(), *Pin->PinName.ToString(), int32(Pin->Direction)));
                FingerprintLines.Add(FString::Printf(TEXT("P|%s|%s|%d|%s"),
                    *Guid, *Pin->PinName.ToString(), int32(Pin->Direction), *Pin->DefaultValue));
                if (Pin->Direction != EGPD_Output) continue;
                for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;
                    Result.Connections.Add(FString::Printf(TEXT("%s:%s>%s:%s"),
                        *Guid, *Pin->PinName.ToString(),
                        *LinkedPin->GetOwningNode()->NodeGuid.ToString(),
                        *LinkedPin->PinName.ToString()));
                }
            }
        }
        Result.NodeGuids.Sort();
        Result.PinDetails.Sort();
        Result.Connections.Sort();
        FingerprintLines.Append(Result.Connections);
        FingerprintLines.Sort();
        Result.Fingerprint = FMD5::HashAnsiString(*FString::Join(FingerprintLines, TEXT("\n")));
        LayoutLines.Sort();
        LayoutLines.Add(Result.Fingerprint);
        Result.LayoutFingerprint = FMD5::HashAnsiString(*FString::Join(LayoutLines, TEXT("\n")));

        // Unreal's GetNodesUnderComment describes the editor's transient drag set.
        // Calculate geometric containment separately, including nested comments;
        // neither is mislabeled as durable, authored membership.
        for (FCFGraphNodeGeometry& Comment : Result.NodeGeometry)
        {
            if (!Comment.bComment || Comment.Width <= 0 || Comment.Height <= 0) continue;
            for (FCFGraphNodeGeometry& Other : Result.NodeGeometry)
            {
                if (&Other == &Comment || Other.Width <= 0 || Other.Height <= 0) continue;
                const bool bInside = int64(Other.X) >= Comment.X &&
                    int64(Other.Y) >= Comment.Y &&
                    int64(Other.X) + Other.Width <= int64(Comment.X) + Comment.Width &&
                    int64(Other.Y) + Other.Height <= int64(Comment.Y) + Comment.Height;
                if (bInside)
                {
                    Comment.GeometricContainedNodeGuids.Add(Other.NodeGuid);
                    Other.GeometricParentCommentGuids.Add(Comment.NodeGuid);
                }
            }
            Comment.GeometricContainedNodeGuids.Sort();
        }
        for (FCFGraphNodeGeometry& Node : Result.NodeGeometry)
            Node.GeometricParentCommentGuids.Sort();

        Result.NodeGeometry.Sort([](const FCFGraphNodeGeometry& A, const FCFGraphNodeGeometry& B)
        {
            return A.NodeGuid < B.NodeGuid;
        });
        Result.bSuccess = true;
        return Result;
    }

    static UEdGraphPin* FindUniquePin(UEdGraphNode* Node, const FString& Name, EEdGraphPinDirection Direction)
    {
        if (!Node || Name.IsEmpty()) return nullptr;
        UEdGraphPin* Match = nullptr;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->PinName == FName(*Name) && Pin->Direction == Direction)
            {
                if (Match) return nullptr;
                Match = Pin;
            }
        }
        return Match;
    }

    static FString Edge(UEdGraphNode* Source, UEdGraphPin* SourcePin,
                        UEdGraphNode* Target, UEdGraphPin* TargetPin)
    {
        return FString::Printf(TEXT("%s:%s>%s:%s"), *Source->NodeGuid.ToString(),
            *SourcePin->PinName.ToString(), *Target->NodeGuid.ToString(), *TargetPin->PinName.ToString());
    }
}

FCFInterfaceList UContextForgeBlueprintTools::ListInterfaces(const FString& BlueprintPath)
{
    FCFInterfaceList Result;
    Result.ProjectPath = ContextForgeBlueprint::CurrentProject();
    Result.BlueprintPath = BlueprintPath;
    UBlueprint* Blueprint = ContextForgeBlueprint::FindBlueprint(BlueprintPath);
    if (!Blueprint)
    {
        Result.Error = TEXT("Invalid, missing or ambiguous Blueprint.");
        return Result;
    }
    TArray<UClass*> Classes;
    FBlueprintEditorUtils::FindImplementedInterfaces(Blueprint, true, Classes);
    for (UClass* Class : Classes)
    {
        if (!Class) continue;
        FCFInterfaceInfo Interface;
        Interface.ClassPath = Class->GetPathName();
        Interface.bDirect = FBlueprintEditorUtils::ImplementsInterface(Blueprint, false, Class);
        Interface.bInherited = !Interface.bDirect;
        Result.Interfaces.Add(MoveTemp(Interface));
    }
    Result.bSuccess = true;
    return Result;
}

FCFBlueprintReceipt UContextForgeBlueprintTools::AddInterface(
    const FString& BlueprintPath, const FString& InterfaceClassPath, bool bSave)
{
    UBlueprint* Blueprint = ContextForgeBlueprint::FindBlueprint(BlueprintPath);
    FCFBlueprintReceipt Result = ContextForgeBlueprint::Receipt(Blueprint, BlueprintPath);
    if (!Blueprint) return Result;
    UClass* Interface = ContextForgeBlueprint::FindInterfaceClass(InterfaceClassPath);
    if (!Interface)
    {
        Result.Outcome = TEXT("INVALID_INTERFACE");
        return Result;
    }
    if (FBlueprintEditorUtils::ImplementsInterface(Blueprint, true, Interface))
    {
        Result.bReadbackVerified = true;
        ContextForgeBlueprint::Complete(Blueprint, Result, false);
        return Result;
    }
    if (!FKismetEditorUtilities::CanBlueprintImplementInterface(Blueprint, Interface))
    {
        Result.Outcome = TEXT("INCOMPATIBLE_INTERFACE");
        return Result;
    }
    Result.bMutationAttempted = true;
    const FScopedTransaction Transaction(FText::FromString(TEXT("ContextForge Add Interface")));
    Blueprint->Modify();
    Result.bMutationApplied = FBlueprintEditorUtils::ImplementNewInterface(
        Blueprint, Interface->GetClassPathName());
    Result.bInitializationCompleted = Result.bMutationApplied;
    Result.bReadbackVerified = FBlueprintEditorUtils::ImplementsInterface(
        Blueprint, false, Interface);
    if (Result.bMutationApplied)
    {
        ContextForgeBlueprint::Compile(Blueprint, Result);
    }
    ContextForgeBlueprint::Complete(Blueprint, Result, bSave);
    return Result;
}

FCFBlueprintReceipt UContextForgeBlueprintTools::RemoveInterface(
    const FString& BlueprintPath, const FString& InterfaceClassPath,
    bool bPreserveFunctions, bool bConfirmDestructive, bool bSave)
{
    UBlueprint* Blueprint = ContextForgeBlueprint::FindBlueprint(BlueprintPath);
    FCFBlueprintReceipt Result = ContextForgeBlueprint::Receipt(Blueprint, BlueprintPath);
    if (!Blueprint) return Result;
    UClass* Interface = ContextForgeBlueprint::FindInterfaceClass(InterfaceClassPath);
    if (!Interface)
    {
        Result.Outcome = TEXT("INVALID_INTERFACE");
        return Result;
    }
    if (!FBlueprintEditorUtils::ImplementsInterface(Blueprint, false, Interface))
    {
        if (FBlueprintEditorUtils::ImplementsInterface(Blueprint, true, Interface))
        {
            Result.Outcome = TEXT("INHERITED_INTERFACE_CANNOT_REMOVE");
            return Result;
        }
        Result.bReadbackVerified = true;
        ContextForgeBlueprint::Complete(Blueprint, Result, false);
        return Result;
    }
    if (!bPreserveFunctions && !bConfirmDestructive)
    {
        Result.Outcome = TEXT("DESTRUCTIVE_CONFIRMATION_REQUIRED");
        Result.Detail = TEXT("Set bConfirmDestructive only after reviewing interface implementations.");
        return Result;
    }

    Result.bMutationAttempted = true;
    const FScopedTransaction Transaction(FText::FromString(TEXT("ContextForge Remove Interface")));
    Blueprint->Modify();
    FBlueprintEditorUtils::RemoveInterface(Blueprint, Interface->GetClassPathName(), bPreserveFunctions);
    Result.bMutationApplied = true;
    Result.bInitializationCompleted = true;
    Result.bReadbackVerified = !FBlueprintEditorUtils::ImplementsInterface(Blueprint, false, Interface);
    ContextForgeBlueprint::Compile(Blueprint, Result);
    ContextForgeBlueprint::Complete(Blueprint, Result, bSave);
    return Result;
}

FCFBlueprintReceipt UContextForgeBlueprintTools::CompileWithDiagnostics(
    const FString& BlueprintPath, bool bWarningsAsErrors)
{
    UBlueprint* Blueprint = ContextForgeBlueprint::FindBlueprint(BlueprintPath);
    FCFBlueprintReceipt Result = ContextForgeBlueprint::Receipt(Blueprint, BlueprintPath);
    if (!Blueprint) return Result;
    Result.bReadbackVerified = true;
    ContextForgeBlueprint::Compile(Blueprint, Result, bWarningsAsErrors);
    ContextForgeBlueprint::Complete(Blueprint, Result, false);
    return Result;
}

FCFBlueprintReceipt UContextForgeBlueprintTools::AddComponentVerified(
    const FString& BlueprintPath, const FString& ComponentClassPath,
    const FString& ComponentName, bool bSave)
{
    UBlueprint* Blueprint = ContextForgeBlueprint::FindBlueprint(BlueprintPath);
    FCFBlueprintReceipt Result = ContextForgeBlueprint::Receipt(Blueprint, BlueprintPath);
    if (!Blueprint) return Result;
    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    UClass* Class = LoadObject<UClass>(nullptr, *ComponentClassPath);
    if (!SCS || !Class || !Class->IsChildOf(UActorComponent::StaticClass()) ||
        ComponentName.IsEmpty() || ComponentName.Contains(TEXT(".")) ||
        ComponentName.Contains(TEXT("/")))
    {
        Result.Outcome = TEXT("INVALID_COMPONENT_PREFLIGHT");
        return Result;
    }
    if (Class->HasAnyClassFlags(CLASS_Abstract))
    {
        Result.Outcome = TEXT("ABSTRACT_COMPONENT_CLASS");
        Result.Detail = TEXT("The requested ActorComponent class is abstract and cannot be instantiated.");
        return Result;
    }

    const FName RequestedName(*ComponentName);
    for (USCS_Node* Node : SCS->GetAllNodes())
    {
        if (Node && Node->GetVariableName() == RequestedName)
        {
            if (Node->ComponentClass.Get() != Class)
            {
                Result.Outcome = TEXT("COMPONENT_NAME_CONFLICT");
                return Result;
            }
            Result.bInitializationCompleted = Node->ComponentTemplate != nullptr;
            Result.bReadbackVerified = true;
            ContextForgeBlueprint::CompleteComponent(Blueprint, Node, Result, bSave);
            return Result;
        }
    }

    Result.bMutationAttempted = true;
    const FScopedTransaction Transaction(FText::FromString(TEXT("ContextForge Add Component")));
    Blueprint->Modify();
    SCS->Modify();
    USCS_Node* Node = SCS->CreateNode(Class, RequestedName);
    if (!Node)
    {
        Result.Outcome = TEXT("COMPONENT_CREATION_FAILED");
        Result.bSafeToRetryCreation = true;
        return Result;
    }
    SCS->AddNode(Node);
    Result.bMutationApplied = true;
    Result.bInitializationCompleted = Node->ComponentTemplate != nullptr;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    for (USCS_Node* Candidate : SCS->GetAllNodes())
    {
        if (Candidate && Candidate->GetVariableName() == RequestedName &&
            Candidate->ComponentClass.Get() == Class)
        {
            Result.bReadbackVerified = true;
            break;
        }
    }
    ContextForgeBlueprint::CompleteComponent(Blueprint, Node, Result, bSave);
    return Result;
}

FCFGraphSnapshot UContextForgeBlueprintTools::GetGraphSnapshot(
    const FString& BlueprintPath, const FString& GraphName)
{
    return ContextForgeBlueprint::Snapshot(
        ContextForgeBlueprint::FindBlueprint(BlueprintPath), BlueprintPath, GraphName);
}

FCFBlueprintReceipt UContextForgeBlueprintTools::RewireGraphConnection(
    const FString& BlueprintPath, const FString& GraphName,
    const FString& ExpectedFingerprint,
    const FString& SourceNodeGuid, const FString& SourcePin,
    const FString& OldTargetNodeGuid, const FString& OldTargetPin,
    const FString& NewTargetNodeGuid, const FString& NewTargetPin,
    bool bSave)
{
    UBlueprint* Blueprint = ContextForgeBlueprint::FindBlueprint(BlueprintPath);
    FCFBlueprintReceipt Result = ContextForgeBlueprint::Receipt(Blueprint, BlueprintPath);
    if (!Blueprint) return Result;
    const FCFGraphSnapshot Before = ContextForgeBlueprint::Snapshot(
        Blueprint, BlueprintPath, GraphName);
    if (!Before.bSuccess || ExpectedFingerprint.IsEmpty() ||
        Before.Fingerprint != ExpectedFingerprint)
    {
        Result.Outcome = TEXT("STALE_OR_INVALID_GRAPH");
        Result.Detail = TEXT("Get a fresh GetGraphSnapshot fingerprint and inspect the live graph.");
        return Result;
    }
    UEdGraph* Graph = ContextForgeBlueprint::FindGraph(Blueprint, GraphName);
    UEdGraphNode* Source = ContextForgeBlueprint::FindNode(Graph, SourceNodeGuid);
    UEdGraphNode* OldNode = ContextForgeBlueprint::FindNode(Graph, OldTargetNodeGuid);
    UEdGraphNode* NewNode = ContextForgeBlueprint::FindNode(Graph, NewTargetNodeGuid);
    UEdGraphPin* From = ContextForgeBlueprint::FindUniquePin(Source, SourcePin, EGPD_Output);
    UEdGraphPin* Old = ContextForgeBlueprint::FindUniquePin(OldNode, OldTargetPin, EGPD_Input);
    UEdGraphPin* New = ContextForgeBlueprint::FindUniquePin(NewNode, NewTargetPin, EGPD_Input);
    if (!From || !Old || !New || From->Direction != EGPD_Output ||
        Old->Direction != EGPD_Input || New->Direction != EGPD_Input ||
        !From->LinkedTo.Contains(Old) || !New->LinkedTo.IsEmpty() ||
        !Graph->GetSchema())
    {
        Result.Outcome = TEXT("CONNECTION_PREFLIGHT_FAILED");
        return Result;
    }
    const FPinConnectionResponse Response = Graph->GetSchema()->CanCreateConnection(From, New);
    // A single-link output may legitimately need its existing link broken.
    // Only permit BREAK_OTHERS when the sole affected link is the reviewed old edge.
    const bool bOnlyOldLink = From->LinkedTo.Num() == 1 && From->LinkedTo[0] == Old;
    if (Response.Response != CONNECT_RESPONSE_MAKE &&
        !(bOnlyOldLink && (Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_A ||
                           Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B)))
    {
        Result.Outcome = TEXT("CONNECTION_WOULD_AFFECT_OTHER_LINKS");
        return Result;
    }

    Result.bMutationAttempted = true;
    const FScopedTransaction Transaction(FText::FromString(TEXT("ContextForge Rewire Connection")));
    Blueprint->Modify();
    Graph->Modify();
    Source->Modify();
    OldNode->Modify();
    NewNode->Modify();
    if (!Graph->GetSchema()->TryCreateConnection(From, New))
    {
        Result.Outcome = TEXT("NEW_CONNECTION_REJECTED");
        return Result;
    }
    Result.bMutationApplied = true;
    From->BreakLinkTo(Old);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    const FCFGraphSnapshot After = ContextForgeBlueprint::Snapshot(
        Blueprint, BlueprintPath, GraphName);
    const FString OldEdge = ContextForgeBlueprint::Edge(Source, From, OldNode, Old);
    const FString NewEdge = ContextForgeBlueprint::Edge(Source, From, NewNode, New);
    TArray<FString> UnrelatedBefore = Before.Connections;
    TArray<FString> UnrelatedAfter = After.Connections;
    UnrelatedBefore.RemoveSingle(OldEdge);
    UnrelatedAfter.RemoveSingle(NewEdge);
    Result.bInitializationCompleted = true;
    Result.bReadbackVerified = After.bSuccess &&
        Before.NodeGuids == After.NodeGuids &&
        UnrelatedBefore == UnrelatedAfter &&
        !After.Connections.Contains(OldEdge) && After.Connections.Contains(NewEdge);
    if (Result.bReadbackVerified)
    {
        ContextForgeBlueprint::Compile(Blueprint, Result);
    }
    ContextForgeBlueprint::Complete(Blueprint, Result, bSave);
    return Result;
}


namespace ContextForgeBlueprint
{
    static const FCFGraphNodeGeometry* GeometryFor(const FCFGraphSnapshot& SnapshotData, const FGuid& Guid)
    {
        for (const FCFGraphNodeGeometry& Item : SnapshotData.NodeGeometry)
        {
            if (Item.NodeGuid == Guid.ToString()) return &Item;
        }
        return nullptr;
    }

    static const FCFGraphPinGeometry* PinGeometryFor(const FCFGraphNodeGeometry* Info, const UEdGraphPin* Pin)
    {
        if (!Info || !Pin) return nullptr;
        for (const FCFGraphPinGeometry& Item : Info->Pins)
        {
            if (Item.PinGuid == Pin->PinId.ToString()) return &Item;
        }
        return nullptr;
    }

    // Caller supplies explicit node IDs, never "everything inside a rectangle".
    static bool SelectGraphNodes(UEdGraph* Graph, const TArray<FString>& Guids,
                                 TArray<UEdGraphNode*>& Selected, FCFBlueprintReceipt& ReceiptData)
    {
        if (!Graph || Guids.IsEmpty())
        {
            ReceiptData.Outcome = TEXT("EMPTY_OR_INVALID_SELECTION");
            return false;
        }
        TArray<FString> Sorted = Guids;
        Sorted.Sort();
        FString Previous;
        for (const FString& Id : Sorted)
        {
            if (Id == Previous)
            {
                ReceiptData.Outcome = TEXT("DUPLICATE_NODE_GUID");
                return false;
            }
            Previous = Id;
            UEdGraphNode* Node = FindNode(Graph, Id);
            if (!Node || Node->IsA<UEdGraphNode_Comment>())
            {
                ReceiptData.Outcome = TEXT("UNKNOWN_OR_COMMENT_NODE");
                ReceiptData.Detail = Id;
                return false;
            }
            Selected.Add(Node);
            ReceiptData.AffectedNodeGuids.Add(Node->NodeGuid.ToString());
        }
        return true;
    }

    static bool Intersects(int32 AX, int32 AY, int32 AW, int32 AH,
                           int32 BX, int32 BY, int32 BW, int32 BH, int32 Margin = 0)
    {
        return AX < BX + BW + Margin && BX < AX + AW + Margin &&
               AY < BY + BH + Margin && BY < AY + AH + Margin;
    }

    static bool SelectedCollision(const FCFGraphSnapshot& Before,
                                  const TArray<FString>& SelectedIds,
                                  int32 X, int32 Y, int32 Width, int32 Height,
                                  const FString& IgnoreCommentId = FString())
    {
        for (const FCFGraphNodeGeometry& Other : Before.NodeGeometry)
        {
            if (SelectedIds.Contains(Other.NodeGuid) || Other.NodeGuid == IgnoreCommentId) continue;
            if (Intersects(X, Y, Width, Height, Other.X, Other.Y, Other.Width, Other.Height, 8))
                return true;
        }
        return false;
    }

    static bool ValidateGraphLayout(const FCFGraphSnapshot& Before,
                                    const FString& ExpectedFingerprint, FCFBlueprintReceipt& ReceiptData)
    {
        if (!Before.bSuccess || ExpectedFingerprint.IsEmpty() ||
            Before.LayoutFingerprint != ExpectedFingerprint)
        {
            ReceiptData.Outcome = TEXT("STALE_OR_INVALID_LAYOUT");
            ReceiptData.Detail = TEXT("Read a fresh GetGraphSnapshot before editing this graph.");
            return false;
        }
        return true;
    }
}


FCFBlueprintReceipt UContextForgeBlueprintTools::PlaceGraphNodes(
    const FString& BlueprintPath, const FString& GraphName,
    const FString& ExpectedLayoutFingerprint, const TArray<FCFGraphNodePlacement>& Placements,
    bool bAllowCommentMembershipChange, bool bRequireMeasuredGeometry, bool bSave)
{
    UBlueprint* Blueprint = ContextForgeBlueprint::FindBlueprint(BlueprintPath);
    FCFBlueprintReceipt Result = ContextForgeBlueprint::Receipt(Blueprint, BlueprintPath);
    if (!Blueprint) return Result;
    UEdGraph* Graph = ContextForgeBlueprint::FindGraph(Blueprint, GraphName);
    if (!Graph || !Graph->GetSchema() || !Graph->GetSchema()->IsA<UEdGraphSchema_K2>())
    {
        Result.Outcome = TEXT("UNSUPPORTED_OR_AMBIGUOUS_GRAPH");
        return Result;
    }
    const FCFGraphSnapshot Before = ContextForgeBlueprint::Snapshot(Blueprint, BlueprintPath, GraphName);
    if (!ContextForgeBlueprint::ValidateGraphLayout(Before, ExpectedLayoutFingerprint, Result))
        return Result;
    if (Placements.IsEmpty())
    {
        Result.Outcome = TEXT("EMPTY_PLACEMENT_BATCH");
        return Result;
    }
    struct FMove
    {
        UEdGraphNode* Node = nullptr;
        const FCFGraphNodeGeometry* Geometry = nullptr;
        int32 X = 0;
        int32 Y = 0;
    };
    TArray<FMove> Moves;
    TSet<FGuid> Seen;
    bool bEstimated = false;
    bool bNeedsChange = false;
    for (const FCFGraphNodePlacement& Placement : Placements)
    {
        FGuid Parsed;
        if (!FGuid::Parse(Placement.NodeGuid, Parsed) || Seen.Contains(Parsed))
        {
            Result.Outcome = TEXT("INVALID_OR_DUPLICATE_NODE_GUID");
            Result.Detail = Placement.NodeGuid;
            return Result;
        }
        Seen.Add(Parsed);
        UEdGraphNode* Node = ContextForgeBlueprint::FindNode(Graph, Placement.NodeGuid);
        if (!Node || Node->IsA<UEdGraphNode_Comment>())
        {
            Result.Outcome = TEXT("UNKNOWN_OR_COMMENT_NODE");
            Result.Detail = Placement.NodeGuid;
            return Result;
        }
        const FCFGraphNodeGeometry* Info = ContextForgeBlueprint::GeometryFor(Before, Parsed);
        if (!Info || Info->Width <= 0 || Info->Height <= 0 ||
            (bRequireMeasuredGeometry && Info->SizeSource != TEXT("RENDERED")))
        {
            Result.Outcome = TEXT("NODE_GEOMETRY_NOT_MEASURED");
            Result.Detail = Placement.NodeGuid;
            return Result;
        }
        if (int64(Placement.X) + Info->Width > MAX_int32 ||
            int64(Placement.Y) + Info->Height > MAX_int32)
        {
            Result.Outcome = TEXT("PLACEMENT_BOUNDS_OVERFLOW");
            return Result;
        }
        bEstimated |= Info->SizeSource != TEXT("RENDERED");
        bNeedsChange |= Node->NodePosX != Placement.X || Node->NodePosY != Placement.Y;
        FMove& Move = Moves.AddDefaulted_GetRef();
        Move.Node = Node;
        Move.Geometry = Info;
        Move.X = Placement.X;
        Move.Y = Placement.Y;
        Result.AffectedNodeGuids.Add(Node->NodeGuid.ToString());
    }

    if (!bNeedsChange)
    {
        Result.bInitializationCompleted = true;
        Result.bReadbackVerified = true;
        ContextForgeBlueprint::Complete(Blueprint, Result, bSave);
        return Result;
    }

    // Authoritative geometry may require an open, measured editor.
    // Comment rectangles intentionally overlap their member nodes. They are
    // checked for membership changes, not mistaken for collision obstacles.
    for (const FMove& Move : Moves)
    {
        for (const FCFGraphNodeGeometry& Other : Before.NodeGeometry)
        {
            if (Other.NodeGuid == Move.Geometry->NodeGuid ||
                Result.AffectedNodeGuids.Contains(Other.NodeGuid)) continue;
            if (Other.bComment)
            {
                const auto Contains = [&Other](int32 X, int32 Y, int32 W, int32 H)
                {
                    return int64(X) >= Other.X && int64(Y) >= Other.Y &&
                        int64(X) + W <= int64(Other.X) + Other.Width &&
                        int64(Y) + H <= int64(Other.Y) + Other.Height;
                };
                if (!bAllowCommentMembershipChange &&
                    Contains(Move.Geometry->X, Move.Geometry->Y,
                             Move.Geometry->Width, Move.Geometry->Height) !=
                    Contains(Move.X, Move.Y, Move.Geometry->Width, Move.Geometry->Height))
                {
                    Result.Outcome = TEXT("COMMENT_MEMBERSHIP_WOULD_CHANGE");
                    Result.Detail = Other.NodeGuid;
                    return Result;
                }
            }
            else if (ContextForgeBlueprint::Intersects(
                Move.X, Move.Y, Move.Geometry->Width, Move.Geometry->Height,
                Other.X, Other.Y, Other.Width, Other.Height, 8))
            {
                Result.Outcome = TEXT("PLACEMENT_COLLIDES_WITH_UNSELECTED");
                Result.Detail = Other.NodeGuid;
                return Result;
            }
        }
    }
    for (int32 A = 0; A < Moves.Num(); ++A)
    {
        for (int32 B = A + 1; B < Moves.Num(); ++B)
        {
            const FMove& One = Moves[A];
            const FMove& Two = Moves[B];
            if (ContextForgeBlueprint::Intersects(
                One.X, One.Y, One.Geometry->Width, One.Geometry->Height,
                Two.X, Two.Y, Two.Geometry->Width, Two.Geometry->Height, 8))
            {
                Result.Outcome = TEXT("PLACEMENT_NODE_OVERLAP");
                Result.Detail = One.Geometry->NodeGuid + TEXT(" / ") + Two.Geometry->NodeGuid;
                return Result;
            }
        }
    }

    Result.bMutationAttempted = true;
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("ContextForge Position Blueprint Nodes")));
        Blueprint->Modify();
        Graph->Modify();
        for (const FMove& Move : Moves)
        {
            if (Move.Node->NodePosX == Move.X && Move.Node->NodePosY == Move.Y) continue;
            Move.Node->Modify();
            Move.Node->NodePosX = Move.X;
            Move.Node->NodePosY = Move.Y;
        }
        Graph->NotifyGraphChanged();
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    }
    Result.bMutationApplied = true;
    Result.bInitializationCompleted = true;
    const FCFGraphSnapshot After = ContextForgeBlueprint::Snapshot(Blueprint, BlueprintPath, GraphName);
    Result.bReadbackVerified = After.bSuccess && Before.Fingerprint == After.Fingerprint &&
        Before.PinDetails == After.PinDetails && Before.Connections == After.Connections &&
        Before.NodeGuids == After.NodeGuids;
    for (const FMove& Move : Moves)
    {
        const FCFGraphNodeGeometry* Current =
            ContextForgeBlueprint::GeometryFor(After, Move.Node->NodeGuid);
        Result.bReadbackVerified &= Current && Current->X == Move.X && Current->Y == Move.Y;
    }
    for (const FCFGraphNodeGeometry& Old : Before.NodeGeometry)
    {
        if (Result.AffectedNodeGuids.Contains(Old.NodeGuid)) continue;
        FGuid Id;
        if (!FGuid::Parse(Old.NodeGuid, Id)) { Result.bReadbackVerified = false; continue; }
        const FCFGraphNodeGeometry* Current = ContextForgeBlueprint::GeometryFor(After, Id);
        Result.bReadbackVerified &= Current && Old.X == Current->X && Old.Y == Current->Y &&
            Old.Width == Current->Width && Old.Height == Current->Height;
    }
    if (bEstimated)
        Result.Detail = TEXT("Node sizes were estimated; inspect visually when graph widgets are measured.");
    ContextForgeBlueprint::Complete(Blueprint, Result, bSave);
    return Result;
}

FCFBlueprintReceipt UContextForgeBlueprintTools::SetGraphComment(
    const FString& BlueprintPath, const FString& GraphName,
    const FString& ExpectedLayoutFingerprint, const FString& ExistingCommentGuid,
    const TArray<FString>& EnclosedNodeGuids,
    const FString& Title, const FString& Details, const FLinearColor& CommentColor,
    int32 X, int32 Y, int32 Width, int32 Height, int32 FontSize,
    bool bShowBubble, bool bColorBubble, bool bGroupMovement, bool bSave)
{
    UBlueprint* Blueprint = ContextForgeBlueprint::FindBlueprint(BlueprintPath);
    FCFBlueprintReceipt Result = ContextForgeBlueprint::Receipt(Blueprint, BlueprintPath);
    if (!Blueprint) return Result;
    UEdGraph* Graph = ContextForgeBlueprint::FindGraph(Blueprint, GraphName);
    if (!Graph || !Graph->GetSchema() || !Graph->GetSchema()->IsA<UEdGraphSchema_K2>())
    {
        Result.Outcome = TEXT("UNSUPPORTED_OR_AMBIGUOUS_GRAPH");
        return Result;
    }
    const FCFGraphSnapshot Before = ContextForgeBlueprint::Snapshot(Blueprint, BlueprintPath, GraphName);
    if (!ContextForgeBlueprint::ValidateGraphLayout(Before, ExpectedLayoutFingerprint, Result))
        return Result;
    if (EnclosedNodeGuids.IsEmpty() || Title.TrimStartAndEnd().IsEmpty() ||
        Title.Len() > 240 || Details.Len() > 4096 ||
        Width <= 0 || Height <= 0 || FontSize < 1 || FontSize > 1000 ||
        int64(X) + Width > MAX_int32 || int64(Y) + Height > MAX_int32 ||
        !FMath::IsFinite(CommentColor.R) || !FMath::IsFinite(CommentColor.G) ||
        !FMath::IsFinite(CommentColor.B) || !FMath::IsFinite(CommentColor.A) ||
        CommentColor.R < 0.f || CommentColor.R > 1.f ||
        CommentColor.G < 0.f || CommentColor.G > 1.f ||
        CommentColor.B < 0.f || CommentColor.B > 1.f ||
        CommentColor.A < 0.f || CommentColor.A > 1.f)
    {
        Result.Outcome = TEXT("INVALID_COMMENT_ARGUMENTS");
        return Result;
    }

    UEdGraphNode_Comment* Comment = ExistingCommentGuid.IsEmpty() ? nullptr :
        Cast<UEdGraphNode_Comment>(ContextForgeBlueprint::FindNode(Graph, ExistingCommentGuid));
    if (!ExistingCommentGuid.IsEmpty() && !Comment)
    {
        Result.Outcome = TEXT("COMMENT_GUID_NOT_FOUND");
        return Result;
    }
    // An identical position/title reuses the comment when no GUID was supplied.
    if (!Comment)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            auto* Candidate = Cast<UEdGraphNode_Comment>(Node);
            if (!Candidate || Candidate->NodeComment != Title ||
                Candidate->NodePosX != X || Candidate->NodePosY != Y ||
                Candidate->NodeWidth != Width || Candidate->NodeHeight != Height) continue;
            if (Comment)
            {
                Result.Outcome = TEXT("AMBIGUOUS_EXISTING_COMMENT");
                return Result;
            }
            Comment = Candidate;
        }
    }

    TSet<FString> ExpectedMembers;
    TArray<UEdGraphNode*> Selected;
    for (const FString& Id : EnclosedNodeGuids)
    {
        FGuid Parsed;
        UEdGraphNode* Node = ContextForgeBlueprint::FindNode(Graph, Id);
        if (!FGuid::Parse(Id, Parsed) || !Node ||
            (Comment && Node == Comment) || ExpectedMembers.Contains(Parsed.ToString()))
        {
            Result.Outcome = TEXT("INVALID_OR_DUPLICATE_COMMENT_CONTENTS");
            Result.Detail = Id;
            return Result;
        }
        ExpectedMembers.Add(Parsed.ToString());
        Selected.Add(Node);
        Result.AffectedNodeGuids.Add(Parsed.ToString());
    }

    const auto Contains = [X, Y, Width, Height](const FCFGraphNodeGeometry& Info)
    {
        return int64(Info.X) >= X && int64(Info.Y) >= Y &&
            int64(Info.X) + Info.Width <= int64(X) + Width &&
            int64(Info.Y) + Info.Height <= int64(Y) + Height;
    };
    // A parent comment fully containing this box is permitted. Any other
    // partially overlapping node, including a comment, is refused.
    for (const FCFGraphNodeGeometry& Info : Before.NodeGeometry)
    {
        if (Comment && Info.NodeGuid == Comment->NodeGuid.ToString()) continue;
        if (ExpectedMembers.Contains(Info.NodeGuid))
        {
            if (!Contains(Info))
            {
                Result.Outcome = TEXT("COMMENT_DOES_NOT_CONTAIN_REVIEWED_NODE");
                Result.Detail = Info.NodeGuid;
                return Result;
            }
            continue;
        }
        const bool bParentComment = Info.bComment &&
            int64(Info.X) <= X && int64(Info.Y) <= Y &&
            int64(Info.X) + Info.Width >= int64(X) + Width &&
            int64(Info.Y) + Info.Height >= int64(Y) + Height;
        if (!bParentComment && ContextForgeBlueprint::Intersects(
            X, Y, Width, Height, Info.X, Info.Y, Info.Width, Info.Height))
        {
            Result.Outcome = TEXT("COMMENT_WOULD_ENCLOSE_OR_OVERLAP_UNSELECTED");
            Result.Detail = Info.NodeGuid;
            return Result;
        }
    }

    const bool bNeedsChange = !Comment || Comment->NodePosX != X ||
        Comment->NodePosY != Y || Comment->NodeWidth != Width ||
        Comment->NodeHeight != Height || Comment->NodeComment != Title ||
        Comment->NodeDetails.ToString() != Details ||
        !Comment->CommentColor.Equals(CommentColor) ||
        Comment->FontSize != FontSize ||
        bool(Comment->bCommentBubbleVisible_InDetailsPanel) != bShowBubble ||
        bool(Comment->bColorCommentBubble) != bColorBubble ||
        bool(Comment->MoveMode == ECommentBoxMode::GroupMovement) != bGroupMovement;
    Result.bMutationAttempted = bNeedsChange;
    if (bNeedsChange)
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("ContextForge Edit Blueprint Comment")));
        Blueprint->Modify();
        Graph->Modify();
        if (!Comment)
        {
            Comment = NewObject<UEdGraphNode_Comment>(Graph, NAME_None, RF_Transactional);
            Comment->CreateNewGuid();
            Graph->AddNode(Comment, false, false);
        }
        Comment->Modify();
        Comment->NodePosX = X;
        Comment->NodePosY = Y;
        Comment->NodeWidth = Width;
        Comment->NodeHeight = Height;
        Comment->NodeComment = Title;
        Comment->NodeDetails = FText::FromString(Details);
        Comment->CommentColor = CommentColor;
        Comment->FontSize = FontSize;
        Comment->bCommentBubbleVisible_InDetailsPanel = bShowBubble;
        Comment->bColorCommentBubble = bColorBubble;
        Comment->MoveMode = bGroupMovement ?
            ECommentBoxMode::GroupMovement : ECommentBoxMode::NoGroupMovement;
        // Unreal already exposes the editor drag set. Refresh it from the
        // caller-reviewed GUID set instead of guessing based on a rectangle.
        Comment->ClearNodesUnderComment();
        for (UEdGraphNode* SelectedNode : Selected)
            Comment->AddNodeUnderComment(SelectedNode);
        Graph->NotifyGraphChanged();
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        Result.bMutationApplied = true;
    }
    Result.ResultNodeGuid = Comment->NodeGuid.ToString();

    const FCFGraphSnapshot After = ContextForgeBlueprint::Snapshot(Blueprint, BlueprintPath, GraphName);
    Result.bInitializationCompleted = true;
    Result.bReadbackVerified = After.bSuccess &&
        Before.Connections == After.Connections && Before.PinDetails == After.PinDetails &&
        After.NodeGuids.Num() == Before.NodeGuids.Num() + (Before.NodeGuids.Contains(Result.ResultNodeGuid) ? 0 : 1);
    for (const FString& Id : Before.NodeGuids)
        Result.bReadbackVerified &= After.NodeGuids.Contains(Id);
    for (const FCFGraphNodeGeometry& Old : Before.NodeGeometry)
    {
        if (Old.NodeGuid == Result.ResultNodeGuid) continue;
        FGuid Id;
        if (!FGuid::Parse(Old.NodeGuid, Id)) { Result.bReadbackVerified = false; continue; }
        const FCFGraphNodeGeometry* Current = ContextForgeBlueprint::GeometryFor(After, Id);
        Result.bReadbackVerified &= Current && Old.X == Current->X && Old.Y == Current->Y;
    }
    const FCFGraphNodeGeometry* Info = ContextForgeBlueprint::GeometryFor(After, Comment->NodeGuid);
    Result.bReadbackVerified &= Info && Info->bComment &&
        Info->X == X && Info->Y == Y && Info->Width == Width && Info->Height == Height &&
        Info->CommentText == Title && Info->CommentDetails == Details &&
        Info->CommentColor.Equals(CommentColor) && Info->CommentFontSize == FontSize &&
        Info->bCommentBubbleVisible == bShowBubble &&
        Info->bCommentBubbleColored == bColorBubble &&
        Info->bCommentMovesContainedNodes == bGroupMovement;
    if (Info)
    {
        TArray<FString> ExpectedSorted = ExpectedMembers.Array();
        ExpectedSorted.Sort();
        Result.bReadbackVerified &= Info->GeometricContainedNodeGuids == ExpectedSorted;
    }
    ContextForgeBlueprint::Complete(Blueprint, Result, bSave);
    return Result;
}

FCFBlueprintReceipt UContextForgeBlueprintTools::FormatGraphCluster(
    const FString& BlueprintPath, const FString& GraphName,
    const FString& ExpectedLayoutFingerprint, const TArray<FString>& NodeGuids,
    int32 HorizontalSpacing, int32 VerticalSpacing,
    bool bRequireMeasuredGeometry, bool bSave)
{
    UBlueprint* Blueprint = ContextForgeBlueprint::FindBlueprint(BlueprintPath);
    FCFBlueprintReceipt Result = ContextForgeBlueprint::Receipt(Blueprint, BlueprintPath);
    if (!Blueprint) return Result;
    UEdGraph* Graph = ContextForgeBlueprint::FindGraph(Blueprint, GraphName);
    if (!Graph || !Graph->GetSchema() || !Graph->GetSchema()->IsA<UEdGraphSchema_K2>())
    {
        Result.Outcome = TEXT("UNSUPPORTED_OR_AMBIGUOUS_GRAPH");
        return Result;
    }
    const FCFGraphSnapshot Before = ContextForgeBlueprint::Snapshot(Blueprint, BlueprintPath, GraphName);
    if (!ContextForgeBlueprint::ValidateGraphLayout(Before, ExpectedLayoutFingerprint, Result))
        return Result;
    if (HorizontalSpacing < 40 || VerticalSpacing < 20)
    {
        Result.Outcome = TEXT("INVALID_LAYOUT_SPACING");
        return Result;
    }
    TArray<UEdGraphNode*> Selected;
    if (!ContextForgeBlueprint::SelectGraphNodes(Graph, NodeGuids, Selected, Result))
        return Result;

    bpal::FLayeredConfig Config;
    Config.RankSpacingX = float(HorizontalSpacing);
    Config.NodeSpacingY = float(VerticalSpacing);
    bpal::FLayeredGraph Layout(Config);
    TMap<FGuid, int32> VertexIds;
    int32 BaseX = MAX_int32;
    int32 BaseY = MAX_int32;
    bool bUsedEstimatedGeometry = false;
    for (UEdGraphNode* Node : Selected)
    {
        const FCFGraphNodeGeometry* Info = ContextForgeBlueprint::GeometryFor(Before, Node->NodeGuid);
        if (!Info || (bRequireMeasuredGeometry && Info->SizeSource != TEXT("RENDERED")))
        {
            Result.Outcome = TEXT("NODE_GEOMETRY_NOT_MEASURED");
            Result.Detail = Node->NodeGuid.ToString() + TEXT(" needs an open, laid-out Blueprint editor.");
            return Result;
        }
        bUsedEstimatedGeometry |= Info->SizeSource != TEXT("RENDERED");
        VertexIds.Add(Node->NodeGuid, Layout.AddVertex(float(Info->Width), float(Info->Height)));
        BaseX = FMath::Min(BaseX, Node->NodePosX);
        BaseY = FMath::Min(BaseY, Node->NodePosY);
    }

    for (UEdGraphNode* Node : Selected)
    {
        const int32* SourceId = VertexIds.Find(Node->NodeGuid);
        const FCFGraphNodeGeometry* SourceInfo = ContextForgeBlueprint::GeometryFor(Before, Node->NodeGuid);
        for (UEdGraphPin* From : Node->Pins)
        {
            if (!From || From->Direction != EGPD_Output) continue;
            for (UEdGraphPin* To : From->LinkedTo)
            {
                if (!To || !To->GetOwningNode()) continue;
                const int32* TargetId = VertexIds.Find(To->GetOwningNode()->NodeGuid);
                if (!TargetId) continue;
                const FCFGraphNodeGeometry* TargetInfo =
                    ContextForgeBlueprint::GeometryFor(Before, To->GetOwningNode()->NodeGuid);
                const FCFGraphPinGeometry* SourcePin = ContextForgeBlueprint::PinGeometryFor(SourceInfo, From);
                const FCFGraphPinGeometry* TargetPin = ContextForgeBlueprint::PinGeometryFor(TargetInfo, To);
                if (bRequireMeasuredGeometry &&
                    (!SourcePin || !TargetPin || !SourcePin->bMeasured || !TargetPin->bMeasured))
                {
                    Result.Outcome = TEXT("PIN_GEOMETRY_NOT_MEASURED");
                    Result.Detail = From->PinId.ToString() + TEXT(" -> ") + To->PinId.ToString();
                    return Result;
                }
                bUsedEstimatedGeometry |= !SourcePin || !TargetPin ||
                                          !SourcePin->bMeasured || !TargetPin->bMeasured;
                const float FromY = SourcePin ? SourcePin->Y - float(SourceInfo->Y) : -1.f;
                const float ToY = TargetPin ? TargetPin->Y - float(TargetInfo->Y) : -1.f;
                const bool bExec = From->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
                Layout.AddEdge(*SourceId, *TargetId, FromY, ToY, bExec);
            }
        }
    }
    Layout.Solve();
    const auto& Vertices = Layout.Vertices();
    float MinX = MAX_flt, MinY = MAX_flt;
    for (UEdGraphNode* Node : Selected)
    {
        const auto& V = Vertices[VertexIds.FindChecked(Node->NodeGuid)];
        MinX = FMath::Min(MinX, V.X);
        MinY = FMath::Min(MinY, V.Y);
    }

    struct FMove
    {
        UEdGraphNode* Node = nullptr;
        int32 X = 0;
        int32 Y = 0;
    };
    TArray<FMove> Moves;
    for (UEdGraphNode* Node : Selected)
    {
        const auto& V = Vertices[VertexIds.FindChecked(Node->NodeGuid)];
        const FCFGraphNodeGeometry* Info = ContextForgeBlueprint::GeometryFor(Before, Node->NodeGuid);
        FMove& Move = Moves.AddDefaulted_GetRef();
        Move.Node = Node;
        Move.X = BaseX + FMath::RoundToInt(V.X - MinX);
        Move.Y = BaseY + FMath::RoundToInt(V.Y - MinY);
        if (ContextForgeBlueprint::SelectedCollision(Before, Result.AffectedNodeGuids,
                                                    Move.X, Move.Y, Info->Width, Info->Height))
        {
            Result.Outcome = TEXT("LAYOUT_COLLIDES_WITH_UNSELECTED");
            Result.Detail = TEXT("Format a smaller cluster or move neighboring nodes explicitly.");
            return Result;
        }
    }
    for (int32 A = 0; A < Moves.Num(); ++A)
    {
        const auto* GA = ContextForgeBlueprint::GeometryFor(Before, Moves[A].Node->NodeGuid);
        for (int32 B = A + 1; B < Moves.Num(); ++B)
        {
            const auto* GB = ContextForgeBlueprint::GeometryFor(Before, Moves[B].Node->NodeGuid);
            if (ContextForgeBlueprint::Intersects(Moves[A].X, Moves[A].Y, GA->Width, GA->Height,
                                                  Moves[B].X, Moves[B].Y, GB->Width, GB->Height, 0))
            {
                Result.Outcome = TEXT("LAYOUT_SOLVER_COLLISION");
                return Result;
            }
        }
    }

    for (const FMove& Move : Moves)
    {
        if (Move.Node->NodePosX != Move.X || Move.Node->NodePosY != Move.Y)
        {
            Result.bMutationAttempted = true;
            break;
        }
    }
    if (Result.bMutationAttempted)
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("ContextForge Format Blueprint Cluster")));
        Blueprint->Modify();
        Graph->Modify();
        for (const FMove& Move : Moves)
        {
            if (Move.Node->NodePosX == Move.X && Move.Node->NodePosY == Move.Y) continue;
            Move.Node->Modify();
            Move.Node->NodePosX = Move.X;
            Move.Node->NodePosY = Move.Y;
        }
        Graph->NotifyGraphChanged();
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        Result.bMutationApplied = true;
    }

    const FCFGraphSnapshot After = ContextForgeBlueprint::Snapshot(Blueprint, BlueprintPath, GraphName);
    Result.bInitializationCompleted = true;
    Result.bReadbackVerified = After.bSuccess &&
        Before.Fingerprint == After.Fingerprint && Before.NodeGuids == After.NodeGuids;
    for (const FMove& Move : Moves)
    {
        const FCFGraphNodeGeometry* Info = ContextForgeBlueprint::GeometryFor(After, Move.Node->NodeGuid);
        Result.bReadbackVerified &= Info && Info->X == Move.X && Info->Y == Move.Y;
    }
    if (bUsedEstimatedGeometry)
        Result.Detail = TEXT("Layout used explicitly labeled estimated geometry; inspect visually.");
    ContextForgeBlueprint::Complete(Blueprint, Result, bSave);
    return Result;
}

FCFBlueprintReceipt UContextForgeBlueprintTools::UpsertGraphComment(
    const FString& BlueprintPath, const FString& GraphName,
    const FString& ExpectedLayoutFingerprint, const TArray<FString>& NodeGuids,
    const FString& Title, const FString& ExistingCommentGuid,
    const FLinearColor& CommentColor, bool bSave)
{
    UBlueprint* Blueprint = ContextForgeBlueprint::FindBlueprint(BlueprintPath);
    FCFBlueprintReceipt Result = ContextForgeBlueprint::Receipt(Blueprint, BlueprintPath);
    if (!Blueprint) return Result;
    UEdGraph* Graph = ContextForgeBlueprint::FindGraph(Blueprint, GraphName);
    if (!Graph || !Graph->GetSchema() || !Graph->GetSchema()->IsA<UEdGraphSchema_K2>())
    {
        Result.Outcome = TEXT("UNSUPPORTED_OR_AMBIGUOUS_GRAPH");
        return Result;
    }
    const FCFGraphSnapshot Before = ContextForgeBlueprint::Snapshot(Blueprint, BlueprintPath, GraphName);
    if (!ContextForgeBlueprint::ValidateGraphLayout(Before, ExpectedLayoutFingerprint, Result))
        return Result;
    if (Title.TrimStartAndEnd().IsEmpty() || Title.Len() > 240 ||
        !FMath::IsFinite(CommentColor.R) || !FMath::IsFinite(CommentColor.G) ||
        !FMath::IsFinite(CommentColor.B) || !FMath::IsFinite(CommentColor.A) ||
        CommentColor.R < 0.f || CommentColor.R > 1.f ||
        CommentColor.G < 0.f || CommentColor.G > 1.f ||
        CommentColor.B < 0.f || CommentColor.B > 1.f ||
        CommentColor.A < 0.f || CommentColor.A > 1.f)
    {
        Result.Outcome = TEXT("INVALID_COMMENT_ARGUMENTS");
        return Result;
    }
    TArray<UEdGraphNode*> Selected;
    if (!ContextForgeBlueprint::SelectGraphNodes(Graph, NodeGuids, Selected, Result))
        return Result;

    int32 Left = MAX_int32, Top = MAX_int32;
    int32 Right = MIN_int32, Bottom = MIN_int32;
    for (UEdGraphNode* Node : Selected)
    {
        const FCFGraphNodeGeometry* Info = ContextForgeBlueprint::GeometryFor(Before, Node->NodeGuid);
        if (!Info)
        {
            Result.Outcome = TEXT("NODE_GEOMETRY_UNAVAILABLE");
            return Result;
        }
        Left = FMath::Min(Left, Info->X);
        Top = FMath::Min(Top, Info->Y);
        Right = FMath::Max(Right, Info->X + Info->Width);
        Bottom = FMath::Max(Bottom, Info->Y + Info->Height);
    }
    // A roomy title strip and perimeter, without accidentally wrapping a neighbor.
    const int32 BoxX = Left - 44, BoxY = Top - 76;
    const int32 BoxW = Right - Left + 88, BoxH = Bottom - Top + 120;

    UEdGraphNode_Comment* Comment = ExistingCommentGuid.IsEmpty() ? nullptr :
        Cast<UEdGraphNode_Comment>(ContextForgeBlueprint::FindNode(Graph, ExistingCommentGuid));
    if (!ExistingCommentGuid.IsEmpty() && !Comment)
    {
        Result.Outcome = TEXT("COMMENT_GUID_NOT_FOUND");
        return Result;
    }
    if (!Comment)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UEdGraphNode_Comment* Candidate = Cast<UEdGraphNode_Comment>(Node);
            if (!Candidate || Candidate->NodeComment != Title) continue;
            // Only reuse a matching comment around this very cluster, not an unrelated namesake.
            if (Candidate->NodePosX <= Left && Candidate->NodePosY <= Top &&
                Candidate->NodePosX + Candidate->NodeWidth >= Right &&
                Candidate->NodePosY + Candidate->NodeHeight >= Bottom)
            {
                if (Comment)
                {
                    Result.Outcome = TEXT("AMBIGUOUS_EXISTING_COMMENT");
                    return Result;
                }
                Comment = Candidate;
            }
        }
    }

    if (ContextForgeBlueprint::SelectedCollision(
        Before, Result.AffectedNodeGuids, BoxX, BoxY, BoxW, BoxH,
        Comment ? Comment->NodeGuid.ToString() : FString()))
    {
        Result.Outcome = TEXT("COMMENT_WOULD_ENCLOSE_UNSELECTED");
        Result.Detail = TEXT("Reposition or format the cluster before wrapping it.");
        return Result;
    }

    const bool bNeedsChange = !Comment || Comment->NodePosX != BoxX ||
        Comment->NodePosY != BoxY || Comment->NodeWidth != BoxW ||
        Comment->NodeHeight != BoxH || Comment->NodeComment != Title ||
        !Comment->CommentColor.Equals(CommentColor);
    Result.bMutationAttempted = bNeedsChange;
    if (bNeedsChange)
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("ContextForge Wrap Blueprint Nodes")));
        Blueprint->Modify();
        Graph->Modify();
        if (!Comment)
        {
            Comment = NewObject<UEdGraphNode_Comment>(Graph, NAME_None, RF_Transactional);
            Comment->CreateNewGuid();
            Graph->AddNode(Comment, false, false);
        }
        Comment->Modify();
        Comment->NodePosX = BoxX;
        Comment->NodePosY = BoxY;
        Comment->NodeWidth = BoxW;
        Comment->NodeHeight = BoxH;
        Comment->NodeComment = Title;
        Comment->CommentColor = CommentColor;
        Graph->NotifyGraphChanged();
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        Result.bMutationApplied = true;
    }
    Result.ResultNodeGuid = Comment->NodeGuid.ToString();

    const FCFGraphSnapshot After = ContextForgeBlueprint::Snapshot(Blueprint, BlueprintPath, GraphName);
    Result.bInitializationCompleted = true;
    Result.bReadbackVerified = After.bSuccess &&
        Before.Connections == After.Connections &&
        After.NodeGuids.Contains(Result.ResultNodeGuid) &&
        After.NodeGuids.Num() == Before.NodeGuids.Num() + (Before.NodeGuids.Contains(Result.ResultNodeGuid) ? 0 : 1);
    for (UEdGraphNode* Node : Selected)
    {
        const FCFGraphNodeGeometry* OldGeom = ContextForgeBlueprint::GeometryFor(Before, Node->NodeGuid);
        const FCFGraphNodeGeometry* NewGeom = ContextForgeBlueprint::GeometryFor(After, Node->NodeGuid);
        Result.bReadbackVerified &= OldGeom && NewGeom &&
            OldGeom->X == NewGeom->X && OldGeom->Y == NewGeom->Y;
    }
    const FCFGraphNodeGeometry* CommentGeom = ContextForgeBlueprint::GeometryFor(After, Comment->NodeGuid);
    Result.bReadbackVerified &= CommentGeom && CommentGeom->bComment &&
        CommentGeom->X == BoxX && CommentGeom->Y == BoxY &&
        CommentGeom->Width == BoxW && CommentGeom->Height == BoxH &&
        CommentGeom->CommentText == Title;
    ContextForgeBlueprint::Complete(Blueprint, Result, bSave);
    return Result;
}
