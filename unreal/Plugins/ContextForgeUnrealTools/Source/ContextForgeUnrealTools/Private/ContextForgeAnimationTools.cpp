#include "ContextForgeAnimationTools.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimationAsset.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/BlendSpace.h"
#include "AnimationGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "AnimationTransitionGraph.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimStateNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateTransitionNode.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphUtilities.h"
#include "EdGraphSchema_K2.h"
#include "Engine/MemberReference.h"
#include "K2Node_VariableGet.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

namespace CFAnim
{
    static FString ProjectPath()
    {
        return FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
    }

    static FString ObjectPath(const FString& InPath)
    {
        if (!InPath.StartsWith(TEXT("/Game/")) || InPath.Contains(TEXT("..")) ||
            InPath.Contains(TEXT(":")) || InPath.Contains(TEXT("'")) ||
            InPath.Contains(TEXT("\\")) || InPath.Contains(TEXT("\n")))
        {
            return FString();
        }
        const FString Package = InPath.Contains(TEXT(".")) ?
            FPackageName::ObjectPathToPackageName(InPath) : InPath;
        if (!FPackageName::IsValidLongPackageName(Package))
        {
            return FString();
        }
        return InPath.Contains(TEXT(".")) ?
            InPath : InPath + TEXT(".") + FPackageName::GetShortName(InPath);
    }

    static bool NameOK(const FString& Name)
    {
        if (Name.IsEmpty() || Name.Len() > 90)
        {
            return false;
        }
        for (TCHAR C : Name)
        {
            if (!FChar::IsAlnum(C) && C != TEXT('_'))
            {
                return false;
            }
        }
        return true;
    }

    static UAnimBlueprint* LoadAnimBP(const FString& Path)
    {
        const FString Full = ObjectPath(Path);
        return Full.IsEmpty() ? nullptr : LoadObject<UAnimBlueprint>(nullptr, *Full);
    }

    static void CollectGraphs(UAnimBlueprint* BP, TArray<UEdGraph*>& Out)
    {
        if (!BP) return;
        TSet<UEdGraph*> Seen;
        TFunction<void(UEdGraph*)> Visit = [&](UEdGraph* Graph)
        {
            if (!Graph || Seen.Contains(Graph)) return;
            Seen.Add(Graph);
            Out.Add(Graph);
            for (UEdGraph* Child : Graph->SubGraphs) Visit(Child);
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node) continue;
                for (UEdGraph* Child : Node->GetSubGraphs()) Visit(Child);
            }
        };
        for (UEdGraph* G : BP->UbergraphPages) Visit(G);
        for (UEdGraph* G : BP->FunctionGraphs) Visit(G);
        for (UEdGraph* G : BP->MacroGraphs) Visit(G);
        for (UEdGraph* G : BP->DelegateSignatureGraphs) Visit(G);
    }

    static UAnimationGraph* FindRootAnimGraph(UAnimBlueprint* BP)
    {
        if (!BP) return nullptr;
        TArray<UEdGraph*> AllGraphs;
        CollectGraphs(BP, AllGraphs);
        for (UEdGraph* Graph : AllGraphs)
        {
            if (Graph && Graph->GetName() == TEXT("AnimGraph"))
            {
                if (UAnimationGraph* Anim = Cast<UAnimationGraph>(Graph))
                {
                    return Anim;
                }
            }
        }
        return nullptr;
    }

    static UAnimGraphNode_StateMachine* FindMachine(UAnimBlueprint* BP, const FString& MachineName)
    {
        if (!BP || !NameOK(MachineName)) return nullptr;
        TArray<UEdGraph*> AllGraphs;
        CollectGraphs(BP, AllGraphs);
        for (UEdGraph* Graph : AllGraphs)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (UAnimGraphNode_StateMachine* SM = Cast<UAnimGraphNode_StateMachine>(Node))
                {
                    if (SM->EditorStateMachineGraph &&
                        SM->EditorStateMachineGraph->GetName() == MachineName)
                    {
                        return SM;
                    }
                }
            }
        }
        return nullptr;
    }

    static UAnimStateNode* FindState(UAnimationStateMachineGraph* Machine, const FString& StateName)
    {
        if (!Machine) return nullptr;
        for (UEdGraphNode* Node : Machine->Nodes)
        {
            if (UAnimStateNode* State = Cast<UAnimStateNode>(Node))
            {
                if (State->GetStateName() == StateName)
                {
                    return State;
                }
            }
        }
        return nullptr;
    }

    static UAnimStateTransitionNode* FindTransition(UAnimationStateMachineGraph* Machine,
                                                       UAnimStateNode* From, UAnimStateNode* To)
    {
        if (!Machine || !From || !To) return nullptr;
        for (UEdGraphNode* Node : Machine->Nodes)
        {
            if (UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Node))
            {
                if (Transition->GetPreviousState() == From && Transition->GetNextState() == To)
                {
                    return Transition;
                }
            }
        }
        return nullptr;
    }

    static UEdGraphPin* FindBoolInput(UEdGraphNode* Node)
    {
        if (!Node) return nullptr;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input &&
                Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
            {
                return Pin;
            }
        }
        return nullptr;
    }

    static UEdGraphPin* FindBoolOutput(UEdGraphNode* Node)
    {
        if (!Node) return nullptr;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output &&
                Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
            {
                return Pin;
            }
        }
        return nullptr;
    }

    static UEdGraphPin* FindPoseOutput(UEdGraphNode* Node)
    {
        if (!Node) return nullptr;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output &&
                Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
            {
                return Pin;
            }
        }
        return nullptr;
    }

    static int32 ReadAssetPlayer(UAnimStateNode* State, FString& AssetPath,
                                 FString& AssetType, UEdGraphPin*& Output)
    {
        AssetPath.Empty();
        AssetType.Empty();
        Output = nullptr;
        int32 Count = 0;
        if (!State || !State->BoundGraph) return 0;
        for (UEdGraphNode* Node : State->BoundGraph->Nodes)
        {
            if (UAnimGraphNode_SequencePlayer* Seq = Cast<UAnimGraphNode_SequencePlayer>(Node))
            {
                ++Count;
                if (Seq->Node.GetSequence()) AssetPath = Seq->Node.GetSequence()->GetPathName();
                AssetType = TEXT("sequence");
                Output = FindPoseOutput(Seq);
            }
            else if (UAnimGraphNode_BlendSpacePlayer* Blend = Cast<UAnimGraphNode_BlendSpacePlayer>(Node))
            {
                ++Count;
                if (Blend->Node.GetBlendSpace()) AssetPath = Blend->Node.GetBlendSpace()->GetPathName();
                AssetType = TEXT("blendspace");
                Output = FindPoseOutput(Blend);
            }
        }
        return Count;
    }

    static bool HasStateCallbacks(UAnimStateNode* State)
    {
        if (!State) return false;
        if (UAnimGraphNode_StateResult* Sink = State->GetResultNodeInsideState())
        {
            return !Sink->StateEntryFunction.GetMemberName().IsNone() ||
                   !Sink->StateExitFunction.GetMemberName().IsNone() ||
                   !Sink->StateFullyBlendedInFunction.GetMemberName().IsNone();
        }
        return false;
    }

    static FString RuleVariable(UAnimStateTransitionNode* T, bool& bNegated,
                                bool& bConnected)
    {
        bNegated = false;
        bConnected = false;
        if (!T) return FString();
        UAnimationTransitionGraph* Graph = Cast<UAnimationTransitionGraph>(T->BoundGraph);
        if (!Graph || !Graph->GetResultNode()) return FString();
        UEdGraphPin* ResultPin = FindBoolInput(Graph->GetResultNode());
        bConnected = ResultPin && ResultPin->LinkedTo.Num() > 0;
        FString Found;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(Node))
            {
                if (!Found.IsEmpty()) return TEXT("<multiple variable getters>");
                Found = Getter->GetVarName().ToString();
            }
            if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
            {
                if (Call->FunctionReference.GetMemberName() == TEXT("Not_PreBool"))
                {
                    bNegated = true;
                }
            }
        }
        return Found;
    }

    static FString Fingerprint(UAnimGraphNode_StateMachine* SM)
    {
        if (!SM || !SM->EditorStateMachineGraph) return FString();
        TArray<FString> Parts;
        TFunction<void(const UEdGraph*)> Visit = [&](const UEdGraph* G)
        {
            if (!G) return;
            Parts.Add(G->GetName());
            for (const UEdGraphNode* Node : G->Nodes)
            {
                if (!Node) continue;
                Parts.Add(Node->NodeGuid.ToString() + TEXT(":") + Node->GetClass()->GetName());
                if (const UAnimStateNode* State = Cast<UAnimStateNode>(Node))
                {
                    Parts.Add(State->GetStateName());
                }
                if (const UAnimGraphNode_SequencePlayer* Seq = Cast<UAnimGraphNode_SequencePlayer>(Node))
                {
                    Parts.Add(Seq->Node.GetSequence() ? Seq->Node.GetSequence()->GetPathName() : TEXT("<none>"));
                }
                if (const UAnimGraphNode_BlendSpacePlayer* Blend = Cast<UAnimGraphNode_BlendSpacePlayer>(Node))
                {
                    Parts.Add(Blend->Node.GetBlendSpace() ? Blend->Node.GetBlendSpace()->GetPathName() : TEXT("<none>"));
                }
                for (const UEdGraphPin* Pin : Node->Pins)
                {
                    if (!Pin) continue;
                    FString Links = Pin->PinName.ToString() + TEXT("=") + Pin->DefaultValue;
                    for (const UEdGraphPin* Link : Pin->LinkedTo)
                    {
                        if (Link && Link->GetOwningNode())
                        {
                            Links += Link->GetOwningNode()->NodeGuid.ToString() + Link->PinName.ToString();
                        }
                    }
                    Parts.Add(Links);
                }
            }
            for (const UEdGraph* Child : G->SubGraphs)
            {
                Visit(Child);
            }
        };
        Visit(SM->EditorStateMachineGraph);
        Parts.Sort();
        return FMD5::HashAnsiString(*FString::Join(Parts, TEXT("|")));
    }

    static FCFBlueprintReceipt BaseReceipt(UAnimBlueprint* BP, const FString& Path)
    {
        FCFBlueprintReceipt R;
        R.ProjectPath = ProjectPath();
        R.BlueprintPath = Path;
        R.bTargetValidated = BP != nullptr;
        R.Outcome = BP ? TEXT("PREFLIGHT_PASSED") : TEXT("INVALID_ANIM_BLUEPRINT");
        R.Detail = BP ? FString() : TEXT("Expected an existing /Game/ Animation Blueprint.");
        return R;
    }

    static void Finished(UAnimBlueprint* BP, const FString& Path,
                         FCFBlueprintReceipt& R, bool bSave)
    {
        if (!R.bReadbackVerified)
        {
            R.Outcome = R.bMutationApplied ? TEXT("APPLIED_UNVERIFIED") : TEXT("PREFLIGHT_FAILED");
            R.bSafeToRetryCreation = false;
            return;
        }
        R.Outcome = R.bMutationApplied ? TEXT("APPLIED_VERIFIED") : TEXT("ALREADY_SATISFIED");
        if (bSave)
        {
            const FCFBlueprintReceipt Compiled =
                UContextForgeBlueprintTools::CompileWithDiagnostics(Path, true);
            R.bCompileAttempted = Compiled.bCompileAttempted;
            R.bCompileSucceeded = Compiled.bCompileSucceeded;
            R.ErrorCount = Compiled.ErrorCount;
            R.WarningCount = Compiled.WarningCount;
            R.Diagnostics = Compiled.Diagnostics;
            if (!R.bCompileSucceeded)
            {
                R.Outcome = TEXT("APPLIED_COMPILE_FAILED_NOT_SAVED");
                R.Detail = TEXT("Keep the editor open, inspect compiler diagnostics and correct graph.");
                return;
            }
            R.bSaveAttempted = true;
            R.bSaved = UEditorAssetLibrary::SaveLoadedAsset(BP, true);
            R.Outcome = R.bSaved ? TEXT("COMPILED_SAVED") : TEXT("VERIFIED_SAVE_FAILED");
        }
    }

    static bool BoolVariableExists(UAnimBlueprint* BP, const FString& Name)
    {
        if (!BP || !NameOK(Name)) return false;
        for (const FBPVariableDescription& Var : BP->NewVariables)
        {
            if (Var.VarName.ToString() == Name &&
                Var.VarType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
            {
                return true;
            }
        }
        for (UClass* Class = BP->SkeletonGeneratedClass; Class; Class = Class->GetSuperClass())
        {
            if (FindFProperty<FBoolProperty>(Class, FName(*Name))) return true;
        }
        return false;
    }

    static bool FingerprintMatches(UAnimGraphNode_StateMachine* SM, const FString& Expected)
    {
        return !Expected.IsEmpty() && Fingerprint(SM) == Expected;
    }
}

FCFAnimMachineSnapshot UContextForgeAnimationTools::InspectStateMachine(
    const FString& BlueprintPath, const FString& MachineName)
{
    FCFAnimMachineSnapshot R;
    R.ProjectPath = CFAnim::ProjectPath();
    R.BlueprintPath = BlueprintPath;
    R.MachineName = MachineName;
    UAnimBlueprint* BP = CFAnim::LoadAnimBP(BlueprintPath);
    if (!BP)
    {
        R.Error = TEXT("Blueprint missing or path invalid.");
        return R;
    }
    UAnimGraphNode_StateMachine* SM = CFAnim::FindMachine(BP, MachineName);
    if (!SM || !SM->EditorStateMachineGraph)
    {
        R.Error = TEXT("Exact named state machine not found.");
        return R;
    }
    UAnimationStateMachineGraph* Graph = SM->EditorStateMachineGraph;
    R.MachineNodeGuid = SM->NodeGuid.ToString();
    R.Fingerprint = CFAnim::Fingerprint(SM);
    for (const UEdGraphPin* Pin : SM->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0)
        {
            R.bConnectedToFinalPose = true;
        }
    }
    R.bHasEntry = Graph->EntryNode && Graph->EntryNode->Pins.Num() > 0 &&
                  Graph->EntryNode->Pins[0]->LinkedTo.Num() == 1;
    UAnimStateNode* EntryState = nullptr;
    if (R.bHasEntry)
    {
        EntryState = Cast<UAnimStateNode>(
            Graph->EntryNode->Pins[0]->LinkedTo[0]->GetOwningNode());
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UAnimStateNode* State = Cast<UAnimStateNode>(Node))
        {
            FCFAnimStateInfo Info;
            Info.Name = State->GetStateName();
            Info.NodeGuid = State->NodeGuid.ToString();
            Info.GraphName = State->BoundGraph ? State->BoundGraph->GetName() : FString();
            UEdGraphPin* Output = nullptr;
            Info.PlayerCount = CFAnim::ReadAssetPlayer(State, Info.AssetPath, Info.AssetType, Output);
            UEdGraphPin* Sink = State->GetPoseSinkPinInsideState();
            Info.bPoseConnected = Info.PlayerCount == 1 && Sink && Output &&
                                  Sink->LinkedTo.Contains(Output);
            Info.bEntry = State == EntryState;
            Info.bHasInheritedCallbacks = CFAnim::HasStateCallbacks(State);
            R.States.Add(MoveTemp(Info));
        }
        else if (UAnimStateTransitionNode* T = Cast<UAnimStateTransitionNode>(Node))
        {
            FCFAnimTransitionInfo Info;
            Info.NodeGuid = T->NodeGuid.ToString();
            Info.FromState = T->GetPreviousState() ? T->GetPreviousState()->GetStateName() : FString();
            Info.ToState = T->GetNextState() ? T->GetNextState()->GetStateName() : FString();
            Info.RuleGraph = T->BoundGraph ? T->BoundGraph->GetName() : FString();
            Info.BlendSeconds = T->CrossfadeDuration;
            Info.RuleVariable = CFAnim::RuleVariable(T, Info.bNegated, Info.bRuleConnected);
            R.Transitions.Add(MoveTemp(Info));
        }
    }
    R.bSuccess = true;
    return R;
}

FCFBlueprintReceipt UContextForgeAnimationTools::EnsureStateMachine(
    const FString& BlueprintPath, const FString& MachineName,
    int32 X, int32 Y, bool bSave)
{
    UAnimBlueprint* BP = CFAnim::LoadAnimBP(BlueprintPath);
    FCFBlueprintReceipt R = CFAnim::BaseReceipt(BP, BlueprintPath);
    if (!BP) return R;
    if (!CFAnim::NameOK(MachineName))
    {
        R.Outcome = TEXT("INVALID_MACHINE_NAME");
        return R;
    }
    if (UAnimGraphNode_StateMachine* Existing = CFAnim::FindMachine(BP, MachineName))
    {
        R.ResultNodeGuid = Existing->NodeGuid.ToString();
        R.bReadbackVerified = Existing->EditorStateMachineGraph != nullptr;
        CFAnim::Finished(BP, BlueprintPath, R, bSave);
        return R;
    }
    UAnimationGraph* AnimGraph = CFAnim::FindRootAnimGraph(BP);
    if (!AnimGraph)
    {
        R.Outcome = TEXT("ANIM_GRAPH_NOT_FOUND");
        return R;
    }
    const FScopedTransaction Transaction(NSLOCTEXT("ContextForge", "CreateAnimMachine", "Create animation state machine"));
    AnimGraph->Modify();
    FGraphNodeCreator<UAnimGraphNode_StateMachine> Creator(*AnimGraph);
    UAnimGraphNode_StateMachine* SM = Creator.CreateNode();
    SM->NodePosX = X;
    SM->NodePosY = Y;
    Creator.Finalize();
    R.bMutationAttempted = R.bMutationApplied = true;
    if (SM && SM->EditorStateMachineGraph)
    {
        FBlueprintEditorUtils::RenameGraph(SM->EditorStateMachineGraph, MachineName);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        R.ResultNodeGuid = SM->NodeGuid.ToString();
        R.bInitializationCompleted = true;
        R.bReadbackVerified = CFAnim::FindMachine(BP, MachineName) == SM;
    }
    CFAnim::Finished(BP, BlueprintPath, R, bSave);
    return R;
}

FCFBlueprintReceipt UContextForgeAnimationTools::UpsertAnimationState(
    const FString& BlueprintPath, const FString& MachineName,
    const FString& ExpectedFingerprint, const FString& StateName,
    const FString& AnimationAssetPath, bool bLoop,
    bool bMakeEntry, int32 X, int32 Y, bool bSave)
{
    UAnimBlueprint* BP = CFAnim::LoadAnimBP(BlueprintPath);
    FCFBlueprintReceipt R = CFAnim::BaseReceipt(BP, BlueprintPath);
    if (!BP) return R;
    UAnimGraphNode_StateMachine* SM = CFAnim::FindMachine(BP, MachineName);
    if (!SM || !SM->EditorStateMachineGraph || !CFAnim::NameOK(StateName))
    {
        R.Outcome = TEXT("INVALID_MACHINE_OR_STATE");
        return R;
    }
    if (!CFAnim::FingerprintMatches(SM, ExpectedFingerprint))
    {
        R.Outcome = TEXT("STALE_MACHINE_FINGERPRINT");
        R.Detail = TEXT("InspectStateMachine and supply its current fingerprint before mutation.");
        return R;
    }
    const FString Path = CFAnim::ObjectPath(AnimationAssetPath);
    UAnimationAsset* Asset = Path.IsEmpty() ? nullptr : LoadObject<UAnimationAsset>(nullptr, *Path);
    UAnimSequenceBase* Sequence = Cast<UAnimSequenceBase>(Asset);
    UBlendSpace* BlendSpace = Cast<UBlendSpace>(Asset);
    if ((!Sequence && !BlendSpace) || !BP->TargetSkeleton ||
        Asset->GetSkeleton() != BP->TargetSkeleton)
    {
        R.Outcome = TEXT("ASSET_TYPE_OR_SKELETON_MISMATCH");
        R.Detail = TEXT("Require a valid /Game/ sequence or BlendSpace targeting the exact AnimBlueprint skeleton.");
        return R;
    }
    UAnimationStateMachineGraph* Graph = SM->EditorStateMachineGraph;
    UAnimStateNode* State = CFAnim::FindState(Graph, StateName);
    if (State && (!State->BoundGraph || CFAnim::HasStateCallbacks(State)))
    {
        R.Outcome = TEXT("UNSAFE_EXISTING_STATE");
        R.Detail = TEXT("Cannot overwrite a broken or callback-bearing donor state.");
        return R;
    }
    FString OldAssetPath, OldAssetType;
    UEdGraphPin* OldPose = nullptr;
    const int32 ExistingPlayers = CFAnim::ReadAssetPlayer(State, OldAssetPath, OldAssetType, OldPose);
    if (ExistingPlayers > 1)
    {
        R.Outcome = TEXT("STATE_HAS_MULTIPLE_POSE_PLAYERS");
        return R;
    }
    if (ExistingPlayers == 1 && OldAssetType != (Sequence ? TEXT("sequence") : TEXT("blendspace")))
    {
        R.Outcome = TEXT("EXISTING_PLAYER_TYPE_MISMATCH");
        return R;
    }

    const FScopedTransaction Transaction(NSLOCTEXT("ContextForge", "UpsertAnimState", "Upsert animation state"));
    Graph->Modify();
    R.bMutationAttempted = true;
    if (!State)
    {
        State = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(
            Graph, NewObject<UAnimStateNode>(), FVector2f(X, Y), false);
        if (!State || !State->BoundGraph)
        {
            R.Outcome = TEXT("STATE_CREATION_FAILED");
            return R;
        }
        FBlueprintEditorUtils::RenameGraph(State->BoundGraph, StateName);
        R.bMutationApplied = true;
    }
    State->Modify();
    State->BoundGraph->Modify();

    if (Sequence)
    {
        UAnimGraphNode_SequencePlayer* Player = nullptr;
        for (UEdGraphNode* N : State->BoundGraph->Nodes)
        {
            Player = Cast<UAnimGraphNode_SequencePlayer>(N);
            if (Player) break;
        }
        if (!Player)
        {
            FGraphNodeCreator<UAnimGraphNode_SequencePlayer> Creator(*State->BoundGraph);
            Player = Creator.CreateNode();
            Player->NodePosX = -260;
            Player->NodePosY = 40;
            Creator.Finalize();
            R.bMutationApplied = true;
        }
        Player->Modify();
        if (Player->Node.GetSequence() != Sequence || !OldPose)
        {
            Player->SetAnimationAsset(Asset);
            Player->ReconstructNode();
            R.bMutationApplied = true;
        }
        Player->Node.SetLoopAnimation(bLoop);
    }
    else
    {
        UAnimGraphNode_BlendSpacePlayer* Player = nullptr;
        for (UEdGraphNode* N : State->BoundGraph->Nodes)
        {
            Player = Cast<UAnimGraphNode_BlendSpacePlayer>(N);
            if (Player) break;
        }
        if (!Player)
        {
            FGraphNodeCreator<UAnimGraphNode_BlendSpacePlayer> Creator(*State->BoundGraph);
            Player = Creator.CreateNode();
            Player->NodePosX = -260;
            Player->NodePosY = 40;
            Creator.Finalize();
            R.bMutationApplied = true;
        }
        Player->Modify();
        if (Player->Node.GetBlendSpace() != BlendSpace || !OldPose)
        {
            Player->SetAnimationAsset(Asset);
            Player->ReconstructNode();
            R.bMutationApplied = true;
        }
        Player->Node.SetLoop(bLoop);
    }
    FString AssetPath, Type;
    UEdGraphPin* Pose = nullptr;
    CFAnim::ReadAssetPlayer(State, AssetPath, Type, Pose);
    UEdGraphPin* Sink = State->GetPoseSinkPinInsideState();
    if (!Pose || !Sink)
    {
        R.Outcome = TEXT("STATE_POSE_PIN_MISSING");
        return R;
    }
    if (!Sink->LinkedTo.Contains(Pose))
    {
        if (Sink->LinkedTo.Num() > 0)
        {
            R.Outcome = TEXT("STATE_RESULT_ALREADY_HAS_OTHER_POSE");
            return R;
        }
        if (!State->BoundGraph->GetSchema()->TryCreateConnection(Pose, Sink))
        {
            R.Outcome = TEXT("STATE_POSE_CONNECTION_FAILED");
            return R;
        }
        R.bMutationApplied = true;
    }
    if (bMakeEntry)
    {
        UAnimStateEntryNode* Entry = Graph->EntryNode;
        if (!Entry || Entry->Pins.Num() == 0 ||
            !Graph->GetSchema()->TryCreateConnection(Entry->Pins[0], State->GetInputPin()))
        {
            R.Outcome = TEXT("ENTRY_CONNECTION_FAILED");
            return R;
        }
        R.bMutationApplied = true;
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    R.ResultNodeGuid = State->NodeGuid.ToString();
    R.bInitializationCompleted = true;
    FString CheckAsset, CheckType;
    UEdGraphPin* CheckPose = nullptr;
    const int32 Count = CFAnim::ReadAssetPlayer(State, CheckAsset, CheckType, CheckPose);
    R.bReadbackVerified = Count == 1 && CheckAsset == Asset->GetPathName() &&
        CheckPose && State->GetPoseSinkPinInsideState() &&
        State->GetPoseSinkPinInsideState()->LinkedTo.Contains(CheckPose);
    CFAnim::Finished(BP, BlueprintPath, R, bSave);
    return R;
}

FCFBlueprintReceipt UContextForgeAnimationTools::UpsertStateTransition(
    const FString& BlueprintPath, const FString& MachineName,
    const FString& ExpectedFingerprint, const FString& FromState,
    const FString& ToState, const FString& BooleanVariable,
    bool bNegate, float BlendSeconds, bool bSave)
{
    UAnimBlueprint* BP = CFAnim::LoadAnimBP(BlueprintPath);
    FCFBlueprintReceipt R = CFAnim::BaseReceipt(BP, BlueprintPath);
    if (!BP) return R;
    UAnimGraphNode_StateMachine* SM = CFAnim::FindMachine(BP, MachineName);
    if (!SM || !SM->EditorStateMachineGraph ||
        !CFAnim::FingerprintMatches(SM, ExpectedFingerprint))
    {
        R.Outcome = TEXT("MACHINE_NOT_FOUND_OR_STALE");
        return R;
    }
    if (!CFAnim::BoolVariableExists(BP, BooleanVariable) ||
        !CFAnim::NameOK(FromState) || !CFAnim::NameOK(ToState) ||
        FromState == ToState || !FMath::IsFinite(BlendSeconds) ||
        BlendSeconds < 0.f || BlendSeconds > 2.f)
    {
        R.Outcome = TEXT("INVALID_RULE_OR_BLEND");
        R.Detail = TEXT("Require an existing AnimBP bool, two distinct states, and 0..2-second blend.");
        return R;
    }
    UAnimationStateMachineGraph* Graph = SM->EditorStateMachineGraph;
    UAnimStateNode* From = CFAnim::FindState(Graph, FromState);
    UAnimStateNode* To = CFAnim::FindState(Graph, ToState);
    if (!From || !To)
    {
        R.Outcome = TEXT("STATE_NOT_FOUND");
        return R;
    }
    UAnimStateTransitionNode* Transition = CFAnim::FindTransition(Graph, From, To);
    bool OldNegation = false, OldConnected = false;
    const FString OldVar = CFAnim::RuleVariable(Transition, OldNegation, OldConnected);
    if (Transition && (OldConnected || !OldVar.IsEmpty()) &&
        (!OldConnected || OldVar != BooleanVariable || OldNegation != bNegate))
    {
        R.Outcome = TEXT("EXISTING_RULE_DIFFERS");
        R.Detail = TEXT("Refuse to rewrite a custom or donor transition rule.");
        return R;
    }

    const FScopedTransaction Transaction(NSLOCTEXT("ContextForge", "UpsertAnimTransition", "Upsert animation transition"));
    Graph->Modify();
    R.bMutationAttempted = true;
    if (!Transition)
    {
        if (!Graph->GetSchema()->TryCreateConnection(
                From->GetOutputPin(), To->GetInputPin()))
        {
            R.Outcome = TEXT("TRANSITION_CREATION_FAILED");
            return R;
        }
        Transition = CFAnim::FindTransition(Graph, From, To);
        R.bMutationApplied = true;
    }
    if (!Transition)
    {
        R.Outcome = TEXT("TRANSITION_READBACK_FAILED");
        return R;
    }
    Transition->Modify();
    Transition->CrossfadeDuration = BlendSeconds;
    Transition->BlendMode = EAlphaBlendOption::QuadraticInOut;
    Transition->bAutomaticRuleBasedOnSequencePlayerInState = false;
    Transition->Bidirectional = false;
    Transition->bDisabled = false;
    R.bMutationApplied = true;

    UAnimationTransitionGraph* RuleGraph =
        Cast<UAnimationTransitionGraph>(Transition->BoundGraph);
    if (!RuleGraph || !RuleGraph->GetResultNode())
    {
        R.Outcome = TEXT("TRANSITION_RULE_GRAPH_MISSING");
        return R;
    }
    UEdGraphPin* RuleInput = CFAnim::FindBoolInput(RuleGraph->GetResultNode());
    if (!RuleInput)
    {
        R.Outcome = TEXT("RULE_RESULT_PIN_MISSING");
        return R;
    }
    if (!OldConnected)
    {
        if (RuleInput->LinkedTo.Num() != 0)
        {
            R.Outcome = TEXT("RULE_RESULT_ALREADY_WIRED");
            return R;
        }
        RuleGraph->Modify();
        FGraphNodeCreator<UK2Node_VariableGet> GetterCreator(*RuleGraph);
        UK2Node_VariableGet* Getter = GetterCreator.CreateNode();
        Getter->VariableReference.SetSelfMember(FName(*BooleanVariable));
        Getter->NodePosX = -360;
        Getter->NodePosY = 0;
        GetterCreator.Finalize();
        UEdGraphPin* VarOut = CFAnim::FindBoolOutput(Getter);
        if (!VarOut)
        {
            R.Outcome = TEXT("VARIABLE_BOOL_OUTPUT_MISSING");
            return R;
        }
        UEdGraphPin* ResultOut = VarOut;
        if (bNegate)
        {
            FGraphNodeCreator<UK2Node_CallFunction> NotCreator(*RuleGraph);
            UK2Node_CallFunction* NotNode = NotCreator.CreateNode();
            NotNode->SetFromFunction(
                UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Not_PreBool")));
            NotNode->NodePosX = -160;
            NotNode->NodePosY = 0;
            NotCreator.Finalize();
            UEdGraphPin* NotIn = CFAnim::FindBoolInput(NotNode);
            ResultOut = CFAnim::FindBoolOutput(NotNode);
            if (!NotIn || !ResultOut ||
                !RuleGraph->GetSchema()->TryCreateConnection(VarOut, NotIn))
            {
                R.Outcome = TEXT("NEGATION_RULE_CONNECTION_FAILED");
                return R;
            }
        }
        if (!RuleGraph->GetSchema()->TryCreateConnection(ResultOut, RuleInput))
        {
            R.Outcome = TEXT("RULE_CONNECTION_FAILED");
            return R;
        }
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    R.ResultNodeGuid = Transition->NodeGuid.ToString();
    R.bInitializationCompleted = true;
    bool CheckNeg = false, CheckConnected = false;
    const FString CheckVar = CFAnim::RuleVariable(Transition, CheckNeg, CheckConnected);
    R.bReadbackVerified = CFAnim::FindTransition(Graph, From, To) == Transition &&
        CheckConnected && CheckVar == BooleanVariable && CheckNeg == bNegate &&
        FMath::IsNearlyEqual(Transition->CrossfadeDuration, BlendSeconds);
    CFAnim::Finished(BP, BlueprintPath, R, bSave);
    return R;
}

FCFAnimValidationReceipt UContextForgeAnimationTools::ValidateStateMachine(
    const FString& BlueprintPath, const FString& MachineName, bool bCompile)
{
    FCFAnimValidationReceipt R;
    R.BlueprintPath = BlueprintPath;
    R.MachineName = MachineName;
    const FCFAnimMachineSnapshot Snapshot = InspectStateMachine(BlueprintPath, MachineName);
    R.Fingerprint = Snapshot.Fingerprint;
    if (!Snapshot.bSuccess)
    {
        R.Issues.Add(Snapshot.Error);
        return R;
    }
    if (!Snapshot.bHasEntry) R.Issues.Add(TEXT("Missing or ambiguous entry state."));
    if (!Snapshot.bConnectedToFinalPose)
    {
        R.Issues.Add(TEXT("State machine pose output is not connected to the AnimGraph."));
    }
    if (Snapshot.States.IsEmpty()) R.Issues.Add(TEXT("Machine has no states."));
    for (const FCFAnimStateInfo& State : Snapshot.States)
    {
        if (State.PlayerCount != 1 || State.AssetPath.IsEmpty() || !State.bPoseConnected)
        {
            R.Issues.Add(State.Name + TEXT(": requires one assigned, wired pose player."));
        }
        if (State.bHasInheritedCallbacks)
        {
            R.Issues.Add(State.Name + TEXT(": contains donor state callbacks."));
        }
    }
    for (const FCFAnimTransitionInfo& T : Snapshot.Transitions)
    {
        if (T.FromState.IsEmpty() || T.ToState.IsEmpty() || !T.bRuleConnected ||
            T.RuleVariable.IsEmpty())
        {
            R.Issues.Add(T.FromState + TEXT(" -> ") + T.ToState +
                         TEXT(": transition must have a connected explicit boolean rule."));
        }
    }
    R.bStructureValid = R.Issues.IsEmpty();
    if (bCompile)
    {
        const FCFBlueprintReceipt C = UContextForgeBlueprintTools::CompileWithDiagnostics(
            BlueprintPath, true);
        R.bCompileAttempted = C.bCompileAttempted;
        R.bCompileSucceeded = C.bCompileSucceeded;
        R.ErrorCount = C.ErrorCount;
        R.WarningCount = C.WarningCount;
        R.Diagnostics = C.Diagnostics;
        if (!R.bCompileSucceeded) R.Issues.Add(TEXT("AnimBlueprint compiler failed."));
    }
    return R;
}
