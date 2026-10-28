#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "IPropertyTypeCustomization.h"
#include "PropertyHandle.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "SceneFlowDirector.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"

class FSceneFlowSlotCustomization : public IPropertyTypeCustomization
{
public:
    static TSharedRef<IPropertyTypeCustomization> MakeInstance() { return MakeShared<FSceneFlowSlotCustomization>(); }
    virtual void CustomizeHeader(TSharedRef<IPropertyHandle> Handle, FDetailWidgetRow& Row, IPropertyTypeCustomizationUtils&) override
    {
        Slot = Handle;
        Row.NameContent()[Handle->CreatePropertyNameWidget()]
        .ValueContent().MinDesiredWidth(230)
        [
            SNew(SButton)
            .IsEnabled_Lambda([this]() { return CanEdit(); })
            .ToolTipText(FText::FromString(TEXT("이 슬롯의 위치 오브젝트 또는 경로를 선택합니다. 디렉터 전체 위치는 변경하지 않습니다.")))
            .Text_Lambda([this]()
            {
                const auto* Data = GetData();
                const FString Name = Data && !Data->Name.IsNone() ? Data->Name.ToString() : TEXT("이름 없는 슬롯");
                const bool bHasPosition = Data && (Data->Action == ESceneFlowAction::MoveToPosition || Data->Action == ESceneFlowAction::PlaceAtMarker || Data->Action == ESceneFlowAction::Spawn);
                return FText::FromString(Name + (Data && Data->Action == ESceneFlowAction::MovePath ? TEXT("  · 경로 편집") : bHasPosition ? TEXT("  · 위치 편집") : TEXT("")));
            })
            .OnClicked_Lambda([this]()
            {
                if (ASceneFlowDirector* Director = FindDirector()) Director->SelectSlotEditorHandle(Slot->GetIndexInArray());
                return FReply::Handled();
            })
        ];
    }
    virtual void CustomizeChildren(TSharedRef<IPropertyHandle> Handle, IDetailChildrenBuilder& Children, IPropertyTypeCustomizationUtils&) override
    {
        uint32 Count = 0; Handle->GetNumChildren(Count);
        for (uint32 Index = 0; Index < Count; ++Index)
            if (auto Child = Handle->GetChildHandle(Index)) Children.AddProperty(Child.ToSharedRef());
    }
private:
    TSharedPtr<IPropertyHandle> Slot;
    const FSceneFlowSlot* GetData() const
    {
        void* Address = nullptr;
        return Slot.IsValid() && Slot->GetValueData(Address) == FPropertyAccess::Success ? static_cast<FSceneFlowSlot*>(Address) : nullptr;
    }
    ASceneFlowDirector* FindDirector() const
    {
        if (!Slot.IsValid() || !GEditor) return nullptr;
        TArray<UObject*> Objects; Slot->GetOuterObjects(Objects);
        if (Objects.Num() != 1) return nullptr;
        if (auto* Director = Cast<ASceneFlowDirector>(Objects[0])) return Director;
        auto* Asset = Cast<USceneFlowAsset>(Objects[0]);
        if (!Asset) return nullptr;
        // Editing a shared asset needs a concrete level director to supply its transform.
        for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
            if (auto* Director = Cast<ASceneFlowDirector>(*It)) if (Director->FlowAsset == Asset) return Director;
        ASceneFlowDirector* Found = nullptr;
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World) return nullptr;
        for (TActorIterator<ASceneFlowDirector> It(World); It; ++It) if (It->FlowAsset == Asset)
        { if (Found) return nullptr; Found = *It; }
        return Found;
    }
    bool CanEdit() const
    {
        const auto* Data = GetData();
        if (!Data || !Data->bEnabled || !FindDirector()) return false;
        return (Data->Action == ESceneFlowAction::MovePath) ||
            (Data->Action == ESceneFlowAction::MoveToPosition && Data->bShowPositionHandle) ||
            ((Data->Action == ESceneFlowAction::PlaceAtMarker || Data->Action == ESceneFlowAction::Spawn) && Data->MarkerOrPath.IsNone() && Data->bShowPositionHandle);
    }
};

class FSceneTransitionTestEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        auto& Properties = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
        Properties.RegisterCustomPropertyTypeLayout("SceneFlowSlot", FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FSceneFlowSlotCustomization::MakeInstance));
        Properties.NotifyCustomizationModuleChanged();
    }
    virtual void ShutdownModule() override
    {
        if (auto* Properties = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor"))
        {
            Properties->UnregisterCustomPropertyTypeLayout("SceneFlowSlot");
            Properties->NotifyCustomizationModuleChanged();
        }
    }
};
IMPLEMENT_MODULE(FSceneTransitionTestEditorModule, SceneTransitionTestEditor)
