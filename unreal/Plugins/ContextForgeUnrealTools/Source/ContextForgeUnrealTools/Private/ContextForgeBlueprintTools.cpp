#include "ContextForgeBlueprintTools.h"

#include "Components/ActorComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
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
#include "Misc/ScopedTransaction.h"
#include "Misc/SecureHash.h"

namespace ContextForgeBlueprint
{
    static FString CurrentProject()
    {
        return FPaths::GetProjectFilePath();
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
            TArray<UEdGraphNode*> MentionedNodes;
            FCompilerResultsLog::GetNodesFromTokens(Message->GetMessageTokens(), MentionedNodes);
            if (!MentionedNodes.IsEmpty() && MentionedNodes[0])
            {
                Diagnostic.NodeGuid = MentionedNodes[0]->NodeGuid.ToString();
                const UEdGraph* Graph = MentionedNodes[0]->GetGraph();
                Diagnostic.Graph = Graph ? Graph->GetName() : FString();
            }
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
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) continue;
            const FString Guid = Node->NodeGuid.ToString();
            Result.NodeGuids.Add(Guid);
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
