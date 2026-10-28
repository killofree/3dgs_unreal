#include "SceneFlowBlueprintTools.h"
#include "SceneFlowDirector.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetRegistryModule.h"

namespace
{
bool AddAutoStartGate(UBlueprint* BP)
{
    if (!BP || BP->UbergraphPages.IsEmpty()) return false;
    UEdGraph* Graph = BP->UbergraphPages[0];
    UK2Node_Event* Begin = nullptr;
    for (UEdGraphNode* Node : Graph->Nodes) if (auto* Event = Cast<UK2Node_Event>(Node))
        if (Event->EventReference.GetMemberName() == TEXT("ReceiveBeginPlay")) Begin = Event;
    if (!Begin) return false;
    UEdGraphPin* Out = Begin->FindPinChecked(UEdGraphSchema_K2::PN_Then);
    if (Out->LinkedTo.Num() != 1) return false;
    if (Cast<UK2Node_IfThenElse>(Out->LinkedTo[0]->GetOwningNode())) return true;
    UEdGraphPin* Next = Out->LinkedTo[0];
    const auto* Schema = GetDefault<UEdGraphSchema_K2>();
    auto* Gate = NewObject<UK2Node_IfThenElse>(Graph);
    Graph->AddNode(Gate,false,false); Gate->CreateNewGuid(); Gate->PostPlacedNewNode(); Gate->AllocateDefaultPins();
    Gate->NodePosX = Begin->NodePosX + 250; Gate->NodePosY = Begin->NodePosY;
    Gate->NodeComment = TEXT("Auto Start 켠 경우에만 연출 시작 · 끄면 일반 미리보기"); Gate->bCommentBubbleVisible = true;
    auto* Setting = NewObject<UK2Node_VariableGet>(Graph);
    Graph->AddNode(Setting,false,false);
    Setting->VariableReference.SetSelfMember(GET_MEMBER_NAME_CHECKED(ASceneFlowDirector,bAutoStart));
    Setting->CreateNewGuid(); Setting->PostPlacedNewNode(); Setting->AllocateDefaultPins();
    Setting->NodePosX = Gate->NodePosX; Setting->NodePosY = Gate->NodePosY + 180;
    for (UEdGraphNode* Node : Graph->Nodes) if (Node != Begin && Node != Gate && Node != Setting) Node->NodePosX += 500;
    Out->BreakAllPinLinks();
    const bool Connected = Schema->TryCreateConnection(Out,Gate->GetExecPin()) &&
        Schema->TryCreateConnection(Setting->FindPinChecked(TEXT("bAutoStart")),Gate->GetConditionPin()) &&
        Schema->TryCreateConnection(Gate->GetThenPin(),Next);
    if (!Connected) return false;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    FKismetEditorUtilities::CompileBlueprint(BP);
    BP->MarkPackageDirty();
    return BP->Status != BS_Error;
}
}

UBlueprint* USceneFlowBlueprintTools::CreateSceneDirectorBlueprint(ASceneFlowDirector* Source, const FString& AssetPath)
{
    if (!IsValid(Source) || !FPackageName::IsValidLongPackageName(AssetPath)) return nullptr;
    // Upgrade only the BeginPlay entry connection; preserve all authored steps and defaults.
    if (FPackageName::DoesPackageExist(AssetPath))
    {
        UBlueprint* Existing = LoadObject<UBlueprint>(nullptr, *(AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath)));
        return AddAutoStartGate(Existing) ? Existing : nullptr;
    }
    if (FindPackage(nullptr, *AssetPath)) return nullptr;
    UPackage* Package = CreatePackage(*AssetPath);
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(ASceneFlowDirector::StaticClass(), Package,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    if (!BP || BP->UbergraphPages.IsEmpty()) return nullptr;
    UEdGraph* Graph = BP->UbergraphPages[0];
    TArray<UEdGraphNode*> OldNodes = Graph->Nodes;
    for (auto* Node : OldNodes) Graph->RemoveNode(Node);
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    auto* Begin = NewObject<UK2Node_Event>(Graph);
    Graph->AddNode(Begin, false, false);
    Begin->EventReference.SetExternalMember(FName(TEXT("ReceiveBeginPlay")), AActor::StaticClass());
    Begin->bOverrideFunction = true;
    Begin->CreateNewGuid(); Begin->PostPlacedNewNode(); Begin->AllocateDefaultPins();
    Begin->NodePosX = 0; Begin->NodePosY = 0;
    UEdGraphPin* Previous = Begin->FindPinChecked(UEdGraphSchema_K2::PN_Then);
    int32 X = 300;
    auto AddCall = [&](FName Function, const FString& Comment) -> UK2Node_CallFunction*
    {
        auto* Node = NewObject<UK2Node_CallFunction>(Graph);
        Graph->AddNode(Node, false, false);
        Node->SetFromFunction(ASceneFlowDirector::StaticClass()->FindFunctionByName(Function));
        Node->CreateNewGuid(); Node->PostPlacedNewNode(); Node->AllocateDefaultPins();
        Node->NodePosX = X; Node->NodePosY = 0; X += 380;
        Node->NodeComment = Comment; Node->bCommentBubbleVisible = true;
        check(Schema->TryCreateConnection(Previous, Node->GetExecPin()));
        Previous = Node->GetThenPin();
        return Node;
    };
    AddCall(GET_FUNCTION_NAME_CHECKED(ASceneFlowDirector, BeginSceneGraph), TEXT("검정 화면 유지 · 플레이어 시작 위치 배치 · 로딩 대기"));
    const auto& Slots = Source->FlowAsset ? Source->FlowAsset->Slots : Source->Slots;
    for (const auto& Slot : Slots)
    {
        auto* Node = AddCall(GET_FUNCTION_NAME_CHECKED(ASceneFlowDirector, RunSceneStep), Slot.Name.ToString());
        Schema->TrySetDefaultValue(*Node->FindPinChecked(TEXT("StepName")), Slot.Name.ToString());
        Schema->TrySetDefaultValue(*Node->FindPinChecked(TEXT("DurationOverride")), FString::SanitizeFloat(Slot.Duration));
    }
    AddCall(GET_FUNCTION_NAME_CHECKED(ASceneFlowDirector, FinishSceneGraph), TEXT("첫 씬 완료 · 다음 연출 연결 지점"));
    if (!AddAutoStartGate(BP)) return nullptr;
    auto* Defaults = CastChecked<ASceneFlowDirector>(BP->GeneratedClass->GetDefaultObject());
    Defaults->Slots = Slots;
    for (auto& Slot : Defaults->Slots) Slot.TargetActor = nullptr;
    Defaults->bAutoStart = false;
    Defaults->bUseStartPosition = Source->bUseStartPosition;
    Defaults->StartPosition = Source->StartPosition;
    Defaults->StartPathSlotIndex = Source->StartPathSlotIndex;
    Defaults->bLinkStartToPath = Source->bLinkStartToPath;
    Defaults->bStartWithBlack = Source->bStartWithBlack;
    FAssetRegistryModule::AssetCreated(BP);
    Package->MarkPackageDirty();
    return BP;
}
