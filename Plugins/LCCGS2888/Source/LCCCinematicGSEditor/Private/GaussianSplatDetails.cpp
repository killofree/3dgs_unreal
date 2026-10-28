#include "GaussianSplatDetails.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "IDetailGroup.h"
#include "UObject/UnrealType.h"
#include "GaussianSplatActor.h"
#include "GaussianSplatComponent.h"
#include "DetailWidgetRow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "ScopedTransaction.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatAssetTypeActions.h"
#include "Editor.h"
#include "GSPreviewRecorderSettings.h"

TSharedRef<IDetailCustomization> FGaussianSplatDetails::MakeInstance()
{
	return MakeShared<FGaussianSplatDetails>();
}

static void CommitProductionSnowFloat(
    TWeakObjectPtr<AGaussianSplatActor> WeakActor,
    FName PropertyName, float RequestedValue, float MinValue, float MaxValue)
{
    AGaussianSplatActor* Actor = WeakActor.Get();
    UGaussianSplatComponent* Source = Actor ? Actor->GaussianSplatComponent.Get() : nullptr;
    FFloatProperty* Property = FindFProperty<FFloatProperty>(
        UGaussianSplatComponent::StaticClass(), PropertyName);
    if (!Source || !Property || !FMath::IsFinite(RequestedValue)) return;

    float* CurrentValue = Property->ContainerPtrToValuePtr<float>(Source);
    if (!CurrentValue) return;
    const float NewValue = FMath::Clamp(RequestedValue, MinValue, MaxValue);
    if (FMath::IsNearlyEqual(*CurrentValue, NewValue, KINDA_SMALL_NUMBER)) return;

    // Use the real source component, and its existing non-reconstructing editor
    // property notification path. This preserves undo, shader synchronization,
    // the original GS SceneProxy and all serialized values.
    const FScopedTransaction Transaction(FText::FromString(TEXT("LCC Snow: Change Setting")));
    Source->Modify();
    Source->PreEditChange(Property);
    *CurrentValue = NewValue;
    FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
    Source->PostEditChangeProperty(Event);
}

void FGaussianSplatDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	static const TCHAR* OrderedCategories[] =
	{
		TEXT("01 기본 설정"), TEXT("02 조명 및 그림자"), TEXT("03 색상 및 후처리"),
		TEXT("04 환경 및 날씨"), TEXT("05 프록시 및 Stable"),
		TEXT("06 렌더링 및 성능"), TEXT("07 진단 및 고급")
	};
	struct FGroupSpec { const TCHAR* Root; const TCHAR* Label; };
	static const FGroupSpec Groups[] =
	{
		{ TEXT("02 조명 및 그림자"), TEXT("01 그림자 합성") },
		{ TEXT("02 조명 및 그림자"), TEXT("02 캐스트 및 폐쇄 그림자") },
		{ TEXT("02 조명 및 그림자"), TEXT("03 씬 라이트") },
		{ TEXT("02 조명 및 그림자"), TEXT("04 리라이팅 및 촬영광 제거") },
		{ TEXT("02 조명 및 그림자"), TEXT("05 식물 투과광") },
		{ TEXT("03 색상 및 후처리"), TEXT("01 노출 및 필름") },
		{ TEXT("03 색상 및 후처리"), TEXT("02 전체 색감") },
		{ TEXT("03 색상 및 후처리"), TEXT("03 빛받는 부분") },
		{ TEXT("03 색상 및 후처리"), TEXT("04 그늘 부분") },
		{ TEXT("03 색상 및 후처리"), TEXT("05 디테일 및 선택 색상") },
		{ TEXT("03 색상 및 후처리"), TEXT("06 블룸 및 글레어") },
		{ TEXT("04 환경 및 날씨"), TEXT("01 계절 프리셋") },
		{ TEXT("04 환경 및 날씨"), TEXT("02 날씨 프리셋") },
		{ TEXT("04 환경 및 날씨"), TEXT("02A 비 효과") },
		{ TEXT("04 환경 및 날씨"), TEXT("02B 눈 효과") },
		{ TEXT("04 환경 및 날씨"), TEXT("02 계절 색상") },
		{ TEXT("04 환경 및 날씨"), TEXT("03 대기 및 안개") },
		{ TEXT("04 환경 및 날씨"), TEXT("04 표면 레이어") },
		{ TEXT("04 환경 및 날씨"), TEXT("05 체적광") },
		{ TEXT("05 프록시 및 Stable"), TEXT("01 프록시 설정") },
		{ TEXT("05 프록시 및 Stable"), TEXT("02 Stable Binding") },
		{ TEXT("05 프록시 및 Stable"), TEXT("03 Stable Field") },
		{ TEXT("05 프록시 및 Stable"), TEXT("04 표면 노멀") },
		{ TEXT("05 프록시 및 Stable"), TEXT("05 상태") },
		{ TEXT("05 프록시 및 Stable"), TEXT("06 디버그") }
	};

	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);

	// P30 UI-only. A Gaussian actor has three UGaussianSplatComponent instances,
	// but ONLY the main source component should own scene-wide lighting/colour
	// and time-of-day detail controls. When an auxiliary component receives its
	// own layout invocation, hide its properties individually, never categories.
	// HideCategory() was the cause of the previous accidental 01~07 UI removal.
	bool bAuxiliaryOnly = Objects.Num() > 0;
	for (const TWeakObjectPtr<UObject>& WeakObject : Objects)
	{
		const UGaussianSplatComponent* Component = Cast<UGaussianSplatComponent>(WeakObject.Get());
		const AGaussianSplatActor* Owner = Component ? Cast<AGaussianSplatActor>(Component->GetOwner()) : nullptr;
		if (!Component || !Owner ||
			(Component != Owner->PuddleGaussianComponent.Get() && Component != Owner->SnowGaussianComponent.Get()))
		{
			bAuxiliaryOnly = false;
			break;
		}
	}
	if (bAuxiliaryOnly)
	{
		// Only component fields; no actor-category or source-component handles.
		// Native components remain alive and selectable in the Components tree.
		for (TFieldIterator<FProperty> It(UGaussianSplatComponent::StaticClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property) continue;
			const FString Category = Property->GetMetaData(TEXT("Category"));
			bool bPluginCategory = false;
			for (const TCHAR* RootName : OrderedCategories)
			{
				if (Category == RootName || Category.StartsWith(FString(RootName) + TEXT("|")))
				{
					bPluginCategory = true;
					break;
				}
			}
			if (!bPluginCategory) continue;
			const TSharedRef<IPropertyHandle> Handle = DetailBuilder.GetProperty(Property->GetFName(), Property->GetOwnerStruct());
			if (Handle->IsValidHandle()) DetailBuilder.HideProperty(Handle);
		}
		return; // Do not generate GSGroup on the auxiliary renderers.
	}

	// Existing actor/main component 01~07 category order stays untouched.
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(OrderedCategories); ++Index)
	{
		DetailBuilder.EditCategory(OrderedCategories[Index]).SetSortOrder(Index);
	}

	// Actor-level property rows for helper renderers are not production controls.
	// Their dedicated settings/actions already exist under 04 weather groups.
	// Do not hide any category or the main GaussianSplatComponent.
	for (const TWeakObjectPtr<UObject>& WeakObject : Objects)
	{
		if (Cast<AGaussianSplatActor>(WeakObject.Get()))
		{
			for (const FName HelperName : {
				GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, PuddleGaussianComponent),
				GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, SnowGaussianComponent) })
			{
				const TSharedRef<IPropertyHandle> Handle = DetailBuilder.GetProperty(HelperName, AGaussianSplatActor::StaticClass());
				if (Handle->IsValidHandle()) DetailBuilder.HideProperty(Handle);
			}
			break;
		}
	}

	auto ResolveGaussianActor = [Objects]() -> AGaussianSplatActor*
	{
		for (const TWeakObjectPtr<UObject>& WeakObject : Objects)
		{
			if (AGaussianSplatActor* Actor = Cast<AGaussianSplatActor>(WeakObject.Get()))
			{
				return Actor;
			}
			if (UGaussianSplatComponent* Component = Cast<UGaussianSplatComponent>(WeakObject.Get()))
			{
				if (AGaussianSplatActor* Owner = Cast<AGaussianSplatActor>(Component->GetOwner()))
				{
					return Owner;
				}
			}
		}
		return nullptr;
	};

    // Actor and embedded component both use this customization. Emit the action
    // only for the direct actor layout, or it appears twice in the same Details view.
    const bool bDirectClusterActor = Objects.ContainsByPredicate([](const TWeakObjectPtr<UObject>& O)
        { return Cast<AGaussianSplatActor>(O.Get()) != nullptr; });
    if (bDirectClusterActor)
    {
    // Re-evaluate the selected source on every click: importing/replacing a GS
    // must never bake the asset from a previous details-panel selection.
    DetailBuilder.EditCategory(TEXT("01 기본 설정"))
        .AddCustomRow(FText::FromString(TEXT("클러스터 베이크 VR Cluster Bake")))
        .NameContent()
        [ SNew(STextBlock).Text(FText::FromString(TEXT("VR 클러스터"))) ]
        .ValueContent().MinDesiredWidth(240.0f)
        [ SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [ SNew(STextBlock).Text_Lambda([ResolveGaussianActor]()
                {
                    const auto* A=ResolveGaussianActor();
                    const auto* C=A ? A->GaussianSplatComponent.Get() : nullptr;
                    const auto* Asset=C ? C->GetSplatAsset() : nullptr;
                    return Asset ? FText::Format(FText::FromString(TEXT("{0} · 클러스터 {1}")),
                        FText::FromString(Asset->GetName()), FText::AsNumber(Asset->GetClusterCount()))
                        : FText::FromString(TEXT("3DGS 자산을 먼저 연결하세요"));
                }) ]
            + SVerticalBox::Slot().AutoHeight().Padding(0,4,0,0)
            [ SNew(SButton).Text(FText::FromString(TEXT("클러스터 베이크·적용")))
                .ToolTipText(FText::FromString(TEXT("현재 자산의 원본 데이터를 보존한 베이크 복사본을 저장하고 이 액터에 적용합니다. 축약 LOD를 만들지 않습니다. 레벨 변경은 실행 취소할 수 있으며 모두 저장으로 저장하세요.")))
                .IsEnabled_Lambda([ResolveGaussianActor]()
                {
                    const auto* A=ResolveGaussianActor();
                    return !GEditor->PlayWorld && A && A->GaussianSplatComponent && A->GaussianSplatComponent->GetSplatAsset();
                })
                .OnClicked_Lambda([ResolveGaussianActor]()
                {
                    auto* A=ResolveGaussianActor();
                    auto* C=A ? A->GaussianSplatComponent.Get() : nullptr;
                    if (!C || GEditor->PlayWorld) return FReply::Handled();
                    auto* Baked=FAssetTypeActions_GaussianSplatAsset::CreateVRClusterCopy(C->GetSplatAsset());
                    if (Baked)
                    {
                        const FScopedTransaction Transaction(FText::FromString(TEXT("Apply VR cluster bake")));
                        A->Modify(); C->Modify(); C->SetSplatAsset(Baked); A->MarkPackageDirty();
                    }
                    return FReply::Handled();
                }) ]
        ];

        // V8 global editor preference (not an actor setting). The details panel
        // supplies one action instead of requiring five console commands.
        DetailBuilder.EditCategory(TEXT("07 진단 및 고급"))
            .AddCustomRow(FText::FromString(TEXT("VR 프레임 측정")))
            .WholeRowContent()
            [ SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()
                [ SNew(SCheckBox)
                    .ToolTipText(FText::FromString(TEXT("켜면 다음 Gaussian VR Preview부터 매 프레임 CSV, GPU .utrace 및 자동 Insights 통계를 기록합니다. 선택은 프로젝트별 에디터 설정에 저장됩니다. Indirect Count 비동기 표본도 함께 활성화됩니다.")))
                    .IsEnabled_Lambda([]() { return !(GEditor && GEditor->PlayWorld); })
                    .IsChecked_Lambda([]() {
                        return GSPreviewRecorderUI::IsEnabled() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                    })
                    .OnCheckStateChanged_Lambda([](ECheckBoxState NewState) {
                        GSPreviewRecorderUI::SetEnabled(NewState == ECheckBoxState::Checked);
                    })
                    [ SNew(STextBlock).Text(FText::FromString(TEXT("VR 프레임 측정 활성화 (자동 CSV / GPU 트레이스)"))) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
                [ SNew(SButton)
                    .ToolTipText(FText::FromString(TEXT("VR 성능 테스트 모드를 켜거나 끕니다. 켜면 다음 VR Preview에서 FPS/Unit 화면 표시, 프레임 CSV, .utrace 및 GPU 타이머 CSV 기록을 자동으로 준비합니다. VR Preview 실행 자체는 에디터의 VR Preview 버튼으로 진행하세요.")))
                    .IsEnabled_Lambda([]() { return GEditor && !GEditor->PlayWorld; })
                    .Text_Lambda([]() {
                        return GSPreviewRecorderUI::IsEnabled()
                            ? FText::FromString(TEXT("테스트 모드 종료 (측정 끄기)"))
                            : FText::FromString(TEXT("VR 성능 테스트 모드 켜기"));
                    })
                    .OnClicked_Lambda([]() -> FReply {
                        GSPreviewRecorderUI::SetEnabled(!GSPreviewRecorderUI::IsEnabled());
                        return FReply::Handled();
                    })
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
                [ SNew(STextBlock)
                    .AutoWrapText(true)
                    .Text_Lambda([]() {
                        return GSPreviewRecorderUI::IsEnabled()
                            ? FText::FromString(TEXT("활성: 다음 GS VR Preview 시작/종료에 맞춰 기록·자동 내보내기. 세부 Draw 인스턴스 읽기는 추가 비용이 있습니다."))
                            : FText::FromString(TEXT("비활성: 측정하지 않습니다. 체크 또는 테스트 모드 버튼으로 모든 성능 기록과 FPS/Unit 표시를 준비합니다."));
                    })
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
                [ SNew(STextBlock)
                    .AutoWrapText(true)
                    .Text(FText::FromString(TEXT("저장: Saved/LCCGS/Profiling/  |  테스트 모드 ON 후 VR Preview 실행  |  PIE 도중 변경 불가")))
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 2.0f)
                [ SNew(STextBlock).Text(FText::FromString(TEXT("V15 모드 0 Raster 최적화 (A/B 독립 비교)"))) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
                [ SNew(SCheckBox)
                    .ToolTipText(FText::FromString(TEXT("V15-A: 일반 Gaussian에서 알파 1/255 미만인 픽셀은 원본 그림자/조명 셰이더 계산 전에 제외합니다. 물웅덩이/프리뷰 진단의 특수 알파 경로는 그대로 유지합니다. 기본 OFF.")))
                    .IsEnabled_Lambda([](){return GEditor && !GEditor->PlayWorld;})
                    .IsChecked_Lambda([](){return GSPreviewRecorderUI::IsEarlyAlphaEnabled()?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                    .OnCheckStateChanged_Lambda([](ECheckBoxState State){GSPreviewRecorderUI::SetEarlyAlphaEnabled(State==ECheckBoxState::Checked);})
                    [ SNew(STextBlock).Text(FText::FromString(TEXT("A. 알파 폐기 먼저 → 조명 중복 연산 감소"))) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
                [ SNew(SCheckBox)
                    .ToolTipText(FText::FromString(TEXT("V15-B: PCVR LeanOutputs를 이미 사용하는 렌더 경로에서 GS 전용 안개가 꺼져 있을 경우에만 사용하지 않는 Fog MRT를 제거해 3개를 2개로 줄입니다. 안개 ON이면 자동으로 원래 MRT 복원. 기본 OFF.")))
                    .IsEnabled_Lambda([](){return GEditor && !GEditor->PlayWorld;})
                    .IsChecked_Lambda([](){return GSPreviewRecorderUI::IsTrimFogMRTEnabled()?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                    .OnCheckStateChanged_Lambda([](ECheckBoxState State){GSPreviewRecorderUI::SetTrimFogMRTEnabled(State==ECheckBoxState::Checked);})
                    [ SNew(STextBlock).Text(FText::FromString(TEXT("B. 미사용 GS 안개 MRT 자동 제거 (LeanOutputs 한정)"))) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
                [ SNew(SCheckBox)
                    .ToolTipText(FText::FromString(TEXT("V16-C: 기존 EvaluateFinalSplatLighting 수식을 보이는 Gaussian별 Compute 패스에서 계산하고 픽셀 셰이더에서 결과를 읽습니다. 그림자 베이크/수식은 유지. PCVR LeanOutputs+사전 Compact+Final 조건을 만족할 때만 실행하고 아니면 원본 픽셀 경로로 돌아갑니다. 기본 OFF.")))
                    .IsEnabled_Lambda([](){return GEditor && !GEditor->PlayWorld;})
                    .IsChecked_Lambda([](){return GSPreviewRecorderUI::IsPerSplatLightCacheEnabled()?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                    .OnCheckStateChanged_Lambda([](ECheckBoxState State){GSPreviewRecorderUI::SetPerSplatLightCacheEnabled(State==ECheckBoxState::Checked);})
                    [ SNew(STextBlock).Text(FText::FromString(TEXT("C. Gaussian별 최종 조명 계산 캐시 (원본 그림자 유지)"))) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
                [ SNew(SCheckBox)
                    .ToolTipText(FText::FromString(TEXT("V16-D: C 경로가 활성화된 눈에서 GPU가 기록한 실제 Visible sort tile count를 활용해 FinalColor Compute를 간접 실행합니다. 기존 양안 공유 컬링/Compact를 재사용하지만 눈별 SH·투영·정렬 및 최종 색상은 분리합니다. 기본 OFF.")))
                    .IsEnabled_Lambda([](){return GEditor && !GEditor->PlayWorld;})
                    .IsChecked_Lambda([](){return GSPreviewRecorderUI::IsVisibleLightDispatchEnabled()?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                    .OnCheckStateChanged_Lambda([](ECheckBoxState State){GSPreviewRecorderUI::SetVisibleLightDispatchEnabled(State==ECheckBoxState::Checked);})
                    [ SNew(STextBlock).Text(FText::FromString(TEXT("D. Visible Gaussian 수 기반 GPU 조명 Dispatch"))) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
                [ SNew(SCheckBox)
                    .ToolTipText(FText::FromString(TEXT("V17-E: Gaussian 중심에서 계산하는 radial depth = length(TranslatedWorldPos)는 Quad 모든 픽셀에 동일합니다. 정점 셰이더에서 계산한 뒤 기존 TEXCOORD3의 미사용 w에 전달해 픽셀당 반복 sqrt를 생략합니다. 그림자, 물, 눈, 안개 계산식과 데이터는 그대로 유지. 기본 OFF.")))
                    .IsEnabled_Lambda([](){return GEditor && !GEditor->PlayWorld;})
                    .IsChecked_Lambda([](){return GSPreviewRecorderUI::IsRadialDepthReuseEnabled()?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                    .OnCheckStateChanged_Lambda([](ECheckBoxState State){GSPreviewRecorderUI::SetRadialDepthReuseEnabled(State==ECheckBoxState::Checked);})
                    [ SNew(STextBlock).Text(FText::FromString(TEXT("E. Gaussian 중심 깊이 계산 재사용 (V17)"))) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
                [ SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("A/B/C/D 모두 독립 비교 가능 · 모드 0 사용. C는 안전 조건에서만 작동하고 GPU에 별도 Compute 비용이 추가됩니다. D는 양안 최종 색상 자체를 공유하지 않습니다."))) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
                [ SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("A/B 기본 OFF. 그림자 베이크/조명/계절/날씨는 끄지 않습니다. B는 LeanOutputs가 불가능한 씬에선 효과가 없습니다. Raster 모드 0으로 비교하세요."))) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 2.0f)
                [ SNew(SCheckBox)
                    .ToolTipText(FText::FromString(TEXT("V13: GS 조명/그림자는 원래 픽셀 경로로 복원합니다. 다음 VR Preview에서만 GS 내부/공간 Bloom, UE PostProcess Bloom, PIE Directional LightShaft Bloom을 임시 비활성화합니다. 그림자/안개/합성은 유지, 종료 시 모든 값을 복원합니다.")))
                    .IsEnabled_Lambda([](){ return GEditor && !GEditor->PlayWorld; })
                    .IsChecked_Lambda([](){return GSPreviewRecorderUI::IsDisableAllBloomEnabled() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;})
                    .OnCheckStateChanged_Lambda([](ECheckBoxState State){GSPreviewRecorderUI::SetDisableAllBloomEnabled(State==ECheckBoxState::Checked);})
                    [ SNew(STextBlock).Text(FText::FromString(TEXT("V13 블룸 전체 OFF (VR Preview 한정 · 그림자 복원)"))) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
                [ SNew(STextBlock).AutoWrapText(true)
                    .Text(FText::FromString(TEXT("V12 정점 재조명 변경은 제거했습니다. 양안 공통 컬링/Compact는 유지하고 눈별 투영·정렬은 별도로 계산합니다."))) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 2.0f)
                [ SNew(SCheckBox)
                    .ToolTipText(FText::FromString(TEXT("다음 VR Preview에서 일반 Static/Skeletal/Instanced/Landscape/Procedural 메쉬를 임시로 숨기고 메쉬 그림자도 제외합니다. GS 본체와 GS 소유 프록시, SceneCapture 전용 콜라이더, VR Pawn, SkySphere는 유지합니다. 에디터 원본은 변경하지 않습니다.")))
                    .IsEnabled_Lambda([]() { return GEditor && !GEditor->PlayWorld; })
                    .IsChecked_Lambda([]() {
                        return GSPreviewRecorderUI::IsGSOnlySceneEnabled() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                    })
                    .OnCheckStateChanged_Lambda([](ECheckBoxState State) {
                        GSPreviewRecorderUI::SetGSOnlySceneEnabled(State == ECheckBoxState::Checked);
                    })
                    [ SNew(STextBlock).Text(FText::FromString(TEXT("3DGS만 표시 (일반 메쉬 임시 숨김)"))) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
                [ SNew(STextBlock).AutoWrapText(true)
                    .Text(FText::FromString(TEXT("체크 후 VR Preview 실행. GS 렌더/환경 및 필요 프록시는 유지합니다. PIE 종료 시 원상 복구. 메쉬 제거는 그림자/오클루전에도 영향을 주므로 단순 GPU 시간 뺄셈은 정확하지 않습니다."))) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 2.0f)
                [ SNew(STextBlock).Text(FText::FromString(TEXT("Raster 비용 비교 (테스트별 1개 선택)"))) ]
                + SVerticalBox::Slot().AutoHeight()
                [ SNew(STextBlock).AutoWrapText(true)
                    .Text(FText::FromString(TEXT("기본 0은 화면 변경 없음. 1~3은 선택한 VR Preview 동안만 진단 셰이더를 사용하여 화면이 일시적으로 달라집니다. Preview 종료 즉시 원래 렌더 모드로 복원됩니다."))) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
                [ SNew(SButton)
                    .IsEnabled_Lambda([](){ return GEditor && !GEditor->PlayWorld; })
                    .Text_Lambda([](){ return FText::FromString(GSPreviewRecorderUI::GetRasterDiagnosticVariant()==0 ? TEXT("[선택] 0 원본 화질 / 전체 셰이더") : TEXT("0 원본 화질 / 전체 셰이더")); })
                    .OnClicked_Lambda([](){ GSPreviewRecorderUI::SetRasterDiagnosticVariant(0); return FReply::Handled(); }) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
                [ SNew(SButton)
                    .IsEnabled_Lambda([](){ return GEditor && !GEditor->PlayWorld; })
                    .Text_Lambda([](){ return FText::FromString(GSPreviewRecorderUI::GetRasterDiagnosticVariant()==1 ? TEXT("[선택] 1 간소 PS / 동일 MRT") : TEXT("1 간소 PS / 동일 MRT")); })
                    .OnClicked_Lambda([](){ GSPreviewRecorderUI::SetRasterDiagnosticVariant(1); return FReply::Handled(); }) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
                [ SNew(SButton)
                    .IsEnabled_Lambda([](){ return GEditor && !GEditor->PlayWorld; })
                    .Text_Lambda([](){ return FText::FromString(GSPreviewRecorderUI::GetRasterDiagnosticVariant()==2 ? TEXT("[선택] 2 간소 PS / 2 MRT") : TEXT("2 간소 PS / 2 MRT")); })
                    .OnClicked_Lambda([](){ GSPreviewRecorderUI::SetRasterDiagnosticVariant(2); return FReply::Handled(); }) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
                [ SNew(SButton)
                    .IsEnabled_Lambda([](){ return GEditor && !GEditor->PlayWorld; })
                    .Text_Lambda([](){ return FText::FromString(GSPreviewRecorderUI::GetRasterDiagnosticVariant()==3 ? TEXT("[선택] 3 SH 기본 / 날씨 없음 / 1 MRT") : TEXT("3 SH 기본 / 날씨 없음 / 1 MRT")); })
                    .OnClicked_Lambda([](){ GSPreviewRecorderUI::SetRasterDiagnosticVariant(3); return FReply::Handled(); }) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
                [ SNew(STextBlock).AutoWrapText(true)
                    .Text(FText::FromString(TEXT("모드별 VR Preview를 각각 종료하고 생성된 _RasterStages.csv와 _RasterWorkload.csv를 비교하세요. 1~3 테스트는 진단 화질이며 0이 기존 장면입니다."))) ]
            ];

    }

	// Production rain controls only. Serialized legacy fields still exist to open old
	// project levels, but are hidden and cannot re-enable retired test render paths.
	const TSet<FName> ProductionRainProperties = {
		TEXT("bRainPrecipitationEnabled"), TEXT("RainOverlayStrength"),
		TEXT("RainOverlayExposureEV"), TEXT("RainOverlayGamma"),
		TEXT("RainOverlaySaturation"), TEXT("RainOverlayTint"),
		TEXT("RainOverlayTintStrength"),
		TEXT("ProxyRainRippleStrength"), TEXT("ProxyRainRippleSizeCm"),
		TEXT("ProxyRainRippleSpacingCm"), TEXT("ProxyRainRippleSpeed"),
		TEXT("PuddleWaterFilmOpacity"), TEXT("PuddleWaterReflectionStrength"),
		TEXT("PuddleSourceWetStrength"), TEXT("PuddleRoofFlowStrength"),
		TEXT("PuddleRoofFlowSpeed"), TEXT("bPuddleRoofUseDownflow"),
		TEXT("PuddleRoofMicroSpacingCm"), TEXT("PuddleGaussianSpacingCm"),
		TEXT("PuddleGaussianMaxCount"), TEXT("PuddleRoofMaxSlopeDegrees")
	};
	TSet<const FProperty*> AddedProperties;
	for (const FGroupSpec& Spec : Groups)
	{
		const bool bIsSnowGroup =
			FCString::Strcmp(Spec.Root, TEXT("04 환경 및 날씨")) == 0 &&
			FCString::Strcmp(Spec.Label, TEXT("02B 눈 효과")) == 0;

        // Snow-only island: do not touch the existing category ordering,
        // non-snow property grouping, or nested component details behavior.
        if (bIsSnowGroup)
        {
            AGaussianSplatActor* SnowActor = nullptr;
            bool bActorIsDirectlyCustomized = false;
            for (const TWeakObjectPtr<UObject>& WeakObject : Objects)
            {
                if (AGaussianSplatActor* A = Cast<AGaussianSplatActor>(WeakObject.Get()))
                {
                    SnowActor = A;
                    bActorIsDirectlyCustomized = true;
                    break;
                }
            }

            // Hide ONLY properties in the actual snow group. Selecting a component
            // must not duplicate the actor's UI, and never hides lighting/proxy/etc.
            for (const TWeakObjectPtr<UObject>& WeakObject : Objects)
            {
                UObject* Object = WeakObject.Get();
                if (!Object) continue;
                for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
                {
                    FProperty* P = *It;
                    if (P->GetMetaData(TEXT("Category")) != TEXT("04 환경 및 날씨") ||
                        P->GetMetaData(TEXT("GSGroup")) != TEXT("02B 눈 효과")) continue;
                    const TSharedRef<IPropertyHandle> H = DetailBuilder.GetProperty(P->GetFName(), P->GetOwnerStruct());
                    if (H->IsValidHandle()) DetailBuilder.HideProperty(H);
                }
            }
            if (!bActorIsDirectlyCustomized || !SnowActor) continue;

            const TWeakObjectPtr<AGaussianSplatActor> SelectedActor = SnowActor;
            IDetailCategoryBuilder& Root = DetailBuilder.EditCategory(TEXT("04 환경 및 날씨"));
            IDetailGroup& Snow = Root.AddGroup(
                FName(TEXT("02B 눈 효과")), FText::FromString(TEXT("02B 눈 효과")), false, false);

            auto AddHeading = [&Snow](const TCHAR* Caption)
            {
                Snow.AddWidgetRow().WholeRowContent()
                [ SNew(STextBlock).Text(FText::FromString(Caption)) ];
            };
            auto AddActorProperty = [&Snow, &DetailBuilder](FName Name)
            {
                const TSharedRef<IPropertyHandle> Handle =
                    DetailBuilder.GetProperty(Name, AGaussianSplatActor::StaticClass());
                if (Handle->IsValidHandle()) Snow.AddPropertyRow(Handle);
            };
            auto AddSourceFloat = [&Snow, SelectedActor](
                const TCHAR* Caption, FName Name, float Min, float Max, const TCHAR* Tooltip)
            {
                Snow.AddWidgetRow()
                .NameContent()
                [ SNew(STextBlock).Text(FText::FromString(Caption))
                    .ToolTipText(FText::FromString(Tooltip)) ]
                .ValueContent().MinDesiredWidth(165.0f)
                [
                    SNew(SNumericEntryBox<float>)
                    .AllowSpin(true)
                    .MinValue(Min).MaxValue(Max)
                    .MinSliderValue(Min).MaxSliderValue(Max)
                    .Value_Lambda([SelectedActor, Name]() -> TOptional<float>
                    {
                        const AGaussianSplatActor* A = SelectedActor.Get();
                        const UGaussianSplatComponent* Source = A ? A->GaussianSplatComponent.Get() : nullptr;
                        const FFloatProperty* P = FindFProperty<FFloatProperty>(
                            UGaussianSplatComponent::StaticClass(), Name);
                        return (Source && P)
                            ? TOptional<float>(*P->ContainerPtrToValuePtr<float>(Source))
                            : TOptional<float>();
                    })
                    .OnValueCommitted_Lambda([SelectedActor, Name, Min, Max](float V, ETextCommit::Type)
                    {
                        CommitProductionSnowFloat(SelectedActor, Name, V, Min, Max);
                    })
                    .ToolTipText(FText::FromString(Tooltip))
                ];
            };

            AddHeading(TEXT("01 실시간 적설 조절"));
            AddActorProperty(GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, SnowRuntimeHeightCm2878));
            AddSourceFloat(TEXT("원본 GS 적설 색상 강도"),
                GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowHybridBaseCoverageStrength),
                0.0f, 1.0f, TEXT("베이크 재생성 없이 원본 GS 눈 색상 강도를 조절합니다."));
            AddSourceFloat(TEXT("원본 눈 커버 강도"),
                GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowSourceCoverStrength2872),
                0.0f, 1.0f, TEXT("원본 GS 눈 아래 무늬의 대비를 조절합니다."));

            AddHeading(TEXT("02 눈 베이크 설정 (변경 후 재생성)"));
            AddActorProperty(GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, SnowBakeMaximumHeightCm2878));
            AddActorProperty(GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, SnowBakeIntervalCount2879));
            AddSourceFloat(TEXT("바닥 눈 채우기"),
                GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySnowGroundFillStrength),
                0.0f, 1.0f, TEXT("바닥의 적설 보완 범위. 변경 후 재생성해야 합니다."));
            AddSourceFloat(TEXT("구조물 눈 제거"),
                GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySnowStructureRemoval),
                0.0f, 1.0f, TEXT("경사진 구조물의 적설 억제. 변경 후 재생성해야 합니다."));
            AddSourceFloat(TEXT("구조물 경사 기준"),
                GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySnowStructureSlopeThreshold),
                0.0f, 1.0f, TEXT("적설 허용 경사. 변경 후 재생성해야 합니다."));

            AddHeading(TEXT("03 눈 베이크 관리"));
            Snow.AddWidgetRow().WholeRowContent()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 4.0f, 8.0f, 2.0f)
                [ SNew(SButton)
                    .Text(FText::FromString(TEXT("눈높이 다단계 베이크 재생성")))
                    .OnClicked_Lambda([SelectedActor]()
                    {
                        if (AGaussianSplatActor* A = SelectedActor.Get()) A->GenerateSnowGaussians();
                        return FReply::Handled();
                    }) ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 4.0f, 0.0f, 2.0f)
                [ SNew(SButton)
                    .Text(FText::FromString(TEXT("Snow Gaussian 삭제")))
                    .OnClicked_Lambda([SelectedActor]()
                    {
                        if (AGaussianSplatActor* A = SelectedActor.Get()) A->DeleteSnowGaussians();
                        return FReply::Handled();
                    }) ]
            ];
            AddActorProperty(GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, SnowHeightBakeStatus2878));
            continue;
        }

		// P31 UI-only: group the existing time-of-day properties without editing
		// UPROPERTY metadata or removing any existing 01~07 category. This runs
		// inside the established 03 Scene Light entry; it is deliberately
		// independent from the auxiliary component deduplication path above.
		const bool bIsSceneLightGroup =
			FCString::Strcmp(Spec.Root, TEXT("02 조명 및 그림자")) == 0 &&
			FCString::Strcmp(Spec.Label, TEXT("03 씬 라이트")) == 0;
		if (bIsSceneLightGroup)
		{
			TArray<TSharedRef<IPropertyHandle>> BasicSettings, SunMoonSettings,
				SkyCloudSettings, TimeColorSettings, AdvancedSettings;
			for (const TWeakObjectPtr<UObject>& WeakObject : Objects)
			{
				const UObject* Object = WeakObject.Get();
				if (!Object) continue;
				for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
				{
					FProperty* Property = *It;
					if (!Property || AddedProperties.Contains(Property) ||
						Property->GetMetaData(TEXT("Category")) != TEXT("02 조명 및 그림자")) continue;
					const FString GroupName = Property->GetMetaData(TEXT("GSGroup"));
					const FString PropertyName = Property->GetName();
					const FString EditCondition = Property->GetMetaData(TEXT("EditCondition"));
					const bool bExplicitDayNightGroup =
						GroupName == TEXT("03A 시간대 · 태양/달") ||
						GroupName == TEXT("03B 하늘 · 구름") ||
						GroupName == TEXT("03C 시간대 색상");
					const bool bLegacyDayNight = GroupName == TEXT("03 씬 라이트") &&
						(EditCondition.Contains(TEXT("bEnableAutomaticDayNightCycle")) ||
						 PropertyName.StartsWith(TEXT("UnifiedTOD")) ||
						 PropertyName.StartsWith(TEXT("RuntimeTimeOfDay")) ||
						 PropertyName.StartsWith(TEXT("RuntimeSolar")) ||
						 PropertyName.StartsWith(TEXT("RuntimeDirectionalLightPitch")) ||
						 PropertyName.StartsWith(TEXT("RuntimeP23Lighting")) ||
						 PropertyName == TEXT("bShowTimeOfDaySceneOverlay"));
					if (!bExplicitDayNightGroup && !bLegacyDayNight) continue;

					const TSharedRef<IPropertyHandle> Handle = DetailBuilder.GetProperty(
						Property->GetFName(), Property->GetOwnerStruct());
					if (!Handle->IsValidHandle()) continue;
					DetailBuilder.HideProperty(Handle); // property only; never HideCategory
					AddedProperties.Add(Property);
					if (GroupName == TEXT("03C 시간대 색상") ||
						PropertyName.Contains(TEXT("TintStrength")) ||
						PropertyName.Contains(TEXT("ColorTransition")) ||
						PropertyName == TEXT("TimeOfDayGlobalTintStrength"))
					{
						TimeColorSettings.Add(Handle);
					}
					else if (GroupName == TEXT("03B 하늘 · 구름") ||
						PropertyName.Contains(TEXT("Sky")) || PropertyName.Contains(TEXT("Atmosphere")) ||
						PropertyName.Contains(TEXT("Cloud")) || PropertyName.Contains(TEXT("NightAmbient")))
					{
						SkyCloudSettings.Add(Handle);
					}
					else if (PropertyName == TEXT("bEnableAutomaticDayNightCycle") ||
						PropertyName == TEXT("bUseUnifiedPhysicalTimeOfDay") ||
						PropertyName == TEXT("bShowTimeOfDaySceneOverlay") ||
						PropertyName.StartsWith(TEXT("RuntimeTimeOfDay")) ||
						PropertyName.StartsWith(TEXT("RuntimeSolar")) ||
						PropertyName.StartsWith(TEXT("RuntimeDirectionalLightPitch")))
					{
						BasicSettings.Add(Handle);
					}
					else if (GroupName == TEXT("03A 시간대 · 태양/달") ||
						PropertyName.Contains(TEXT("Moon")) || PropertyName.Contains(TEXT("SunLux")) ||
						PropertyName.Contains(TEXT("LightShaftBloom")) ||
						PropertyName.Contains(TEXT("P23PostProcessBloom")))
					{
						SunMoonSettings.Add(Handle);
					}
					else
					{
						AdvancedSettings.Add(Handle);
					}
				}
			}
			if (BasicSettings.Num() + SunMoonSettings.Num() + SkyCloudSettings.Num() +
				TimeColorSettings.Num() + AdvancedSettings.Num() > 0)
			{
				IDetailCategoryBuilder& Root = DetailBuilder.EditCategory(TEXT("02 조명 및 그림자"));
				IDetailGroup& DayNight = Root.AddGroup(FName(TEXT("03A 낮/밤 효과")),
					FText::FromString(TEXT("03A 낮/밤 효과")), false, false);
				auto AddSection = [&DayNight](const TCHAR* Key, const TCHAR* Label,
					const TArray<TSharedRef<IPropertyHandle>>& Handles)
				{
					if (Handles.IsEmpty()) return;
					IDetailGroup& Section = DayNight.AddGroup(FName(Key), FText::FromString(Label), false);
					for (const TSharedRef<IPropertyHandle>& Handle : Handles)
					{
						Section.AddPropertyRow(Handle);
					}
				};
				AddSection(TEXT("P31TimeOfDayBasic"), TEXT("01 자동 전환 · 시간 표시"), BasicSettings);
				AddSection(TEXT("P31TimeOfDaySunMoon"), TEXT("02 태양 · 달 · Lux · Bloom"), SunMoonSettings);
				AddSection(TEXT("P31TimeOfDaySkyCloud"), TEXT("03 하늘 · 구름 · 야간 밝기"), SkyCloudSettings);
				AddSection(TEXT("P31TimeOfDayColors"), TEXT("04 새벽 · 노을 · 밤 색상"), TimeColorSettings);
				AddSection(TEXT("P31TimeOfDayAdvanced"), TEXT("05 시간대 계산 · 노출 고급"), AdvancedSettings);
			}
		}

		// Original non-snow grouping, including basic scene-light properties, stays unchanged.
		TArray<TSharedRef<IPropertyHandle>> GroupProperties;
		// P45: Present one shared physical fog range first, while retaining all
		// serialized GS-only fractional shape controls in collapsed advanced UI.
		// No fog equations, weather preset values or runtime rendering are changed.
		const bool bIsFogGroup =
			FCString::Strcmp(Spec.Root, TEXT("04 환경 및 날씨")) == 0 &&
			FCString::Strcmp(Spec.Label, TEXT("03 대기 및 안개")) == 0;
		TArray<TSharedRef<IPropertyHandle>> FogCoreProperties, FogUnrealProperties,
			FogGSShapeProperties, FogAtmosphereProperties, FogSaveProperties;
		TSharedPtr<IPropertyHandle> SnowColorStrength;
		TSharedPtr<IPropertyHandle> SnowSourceCoverStrength;
		for (const TWeakObjectPtr<UObject>& WeakObject : Objects)
		{
			const UObject* Object = WeakObject.Get();
			if (!Object) continue;
			for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				if (AddedProperties.Contains(Property) ||
					Property->GetMetaData(TEXT("Category")) != Spec.Root ||
					Property->GetMetaData(TEXT("GSGroup")) != Spec.Label)
				{
					continue;
				}
				TSharedRef<IPropertyHandle> Handle = DetailBuilder.GetProperty(Property->GetFName(), Property->GetOwnerStruct());
				if (Handle->IsValidHandle())
				{
					DetailBuilder.HideProperty(Handle);
					if (FCString::Strcmp(Spec.Label, TEXT("02A 비 효과")) == 0 &&
						!ProductionRainProperties.Contains(Property->GetFName()))
					{
						AddedProperties.Add(Property);
						continue;
					}
					// These are separate live shading parameters, not enable switches.
					// Defer both so they appear next to each other regardless of
					// their declaration order in GaussianSplatComponent.h.
					if (bIsSnowGroup && Property->GetFName() == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowHybridBaseCoverageStrength))
					{
						SnowColorStrength = Handle;
					}
					else if (bIsSnowGroup && Property->GetFName() == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowSourceCoverStrength2872))
					{
						SnowSourceCoverStrength = Handle;
					}
					else if (bIsFogGroup)
					{
						const FName FogName = Property->GetFName();
						// The only normal-use distance range: absolute centimeters, shared by
						// the engine Height Fog and the post-composited GS fog bridge.
						if (FogName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableFogOverlayInOtherWeather) ||
							FogName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogWeatherStrength) ||
							FogName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NativeFogStartDistanceCm) ||
							FogName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogVisibilityDistance) ||
							FogName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogColor) ||
							FogName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, DenseFogScatteringColor))
						{
							FogCoreProperties.Add(Handle);
						}
						else if (FogName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NativeFogDensity) ||
							FogName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NativeFogMaxOpacity) ||
							FogName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableNativeVolumetricFog))
						{
							FogUnrealProperties.Add(Handle);
						}
						else if (FogName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogDistanceStart) ||
							FogName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogDistanceEnd) ||
							FogName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NearAirStart) ||
							FogName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NearAirEnd))
						{
							// P46: legacy ratio controls are kept serialized for old levels,
							// but not presented as additional fog sliders.
						}
						else if (FogName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bSaveCurrentWeatherFogProfileNow) ||
							FogName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bCurrentWeatherFogProfileSaved) ||
							FogName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CurrentWeatherFogProfileOptionCount) ||
							FogName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CurrentWeatherFogProfileSaveRevision))
						{
							FogSaveProperties.Add(Handle);
						}
						else
						{
							FogAtmosphereProperties.Add(Handle);
						}
					}
					else
					{
						GroupProperties.Add(Handle);
					}
					AddedProperties.Add(Property);
				}
			}
		}
		if (!GroupProperties.IsEmpty() || SnowColorStrength.IsValid() || SnowSourceCoverStrength.IsValid() ||
			(bIsFogGroup && (!FogCoreProperties.IsEmpty() || !FogUnrealProperties.IsEmpty() ||
				!FogGSShapeProperties.IsEmpty() || !FogAtmosphereProperties.IsEmpty() || !FogSaveProperties.IsEmpty())))
		{
			IDetailCategoryBuilder& Root = DetailBuilder.EditCategory(Spec.Root);
			IDetailGroup& Group = Root.AddGroup(
				FName(Spec.Label), FText::FromString(Spec.Label), false, false);
			if (bIsFogGroup)
			{
				// Keep the GS-to-world shared centimeter distances together.
				// Advanced fractional controls are NOT disguised as the same units.
				auto AddFogSection = [&Group](const TCHAR* Key, const TCHAR* Caption,
					const TArray<TSharedRef<IPropertyHandle>>& Handles)
				{
					if (Handles.IsEmpty()) return;
					IDetailGroup& Section = Group.AddGroup(FName(Key), FText::FromString(Caption), false);
					for (const TSharedRef<IPropertyHandle>& Handle : Handles)
					{
						Section.AddPropertyRow(Handle);
					}
				};
				AddFogSection(TEXT("P45FogShared"), TEXT("01 공통 안개 · 부드러운 걷힘 / 가시거리"), FogCoreProperties);
				AddFogSection(TEXT("P45FogNative"), TEXT("02 언리얼 포그 고급"), FogUnrealProperties);
				AddFogSection(TEXT("P45FogAtmosphere"), TEXT("03 대기 효과 세부"), FogAtmosphereProperties);
				AddFogSection(TEXT("P45FogSave"), TEXT("05 날씨별 안개 저장"), FogSaveProperties);
			}
			for (const TSharedRef<IPropertyHandle>& Handle : GroupProperties)
			{
				Group.AddPropertyRow(Handle);
			}

			// Latest Rain production pipeline. No historical test/opaque/debug controls.
			if (FCString::Strcmp(Spec.Label, TEXT("02A 비 효과")) == 0)
			{
				Group.AddWidgetRow().WholeRowContent()
				[ SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()
					[ SNew(SButton).Text(FText::FromString(TEXT("비 물 Gaussian 재생성")))
					.OnClicked_Lambda([ResolveGaussianActor]()
					{
						if (auto* A = ResolveGaussianActor()) A->GeneratePuddleGaussians();
						return FReply::Handled();
					}) ]
					+ SHorizontalBox::Slot().AutoWidth().Padding(6,0)
					[ SNew(SButton).Text(FText::FromString(TEXT("물 Gaussian 제거")))
					.OnClicked_Lambda([ResolveGaussianActor]()
					{
						if (auto* A = ResolveGaussianActor()) A->DeletePuddleGaussians();
						return FReply::Handled();
					}) ]
				];
				// The common editor selection is the nested GS component; expose live
				// actor-owned production settings there without an extra test section.
				bool bSelectedActorDirectly = false;
				for (const auto& WeakObject : Objects)
				{
					if (Cast<AGaussianSplatActor>(WeakObject.Get()))
					{
						bSelectedActorDirectly = true;
						break;
					}
				}
				if (!bSelectedActorDirectly)
				{
					auto AddLiveRainFloat = [&Group, ResolveGaussianActor](
						const TCHAR* Caption, FName PropertyName, float Lower, float Upper)
					{
						Group.AddWidgetRow().NameContent()
						[ SNew(STextBlock).Text(FText::FromString(Caption)) ]
						.ValueContent().MinDesiredWidth(170.f)
						[ SNew(SNumericEntryBox<float>)
							.AllowSpin(true).MinValue(Lower).MaxValue(Upper)
							.MinSliderValue(Lower).MaxSliderValue(Upper)
							.Value_Lambda([ResolveGaussianActor, PropertyName]() -> TOptional<float>
							{
								const auto* A = ResolveGaussianActor();
								const auto* P = FindFProperty<FFloatProperty>(AGaussianSplatActor::StaticClass(), PropertyName);
								return A && P ? TOptional<float>(*P->ContainerPtrToValuePtr<float>(A)) : TOptional<float>();
							})
							.OnValueCommitted_Lambda([ResolveGaussianActor, PropertyName, Lower, Upper](float V, ETextCommit::Type)
							{
								if (auto* A = ResolveGaussianActor())
								{
									if (auto* P = FindFProperty<FFloatProperty>(AGaussianSplatActor::StaticClass(), PropertyName))
									{
										const FScopedTransaction T(FText::FromString(TEXT("Rain: Update Water Setting")));
										A->Modify();
										*P->ContainerPtrToValuePtr<float>(A) = FMath::Clamp(V, Lower, Upper);
										A->SyncWeatherEffects();
									}
								}
							})
						];
					};
					AddLiveRainFloat(TEXT("물 수막 투명도"), GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, PuddleWaterFilmOpacity), 0.f, 0.9f);
					AddLiveRainFloat(TEXT("물 수막 반사 강도"), GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, PuddleWaterReflectionStrength), 0.f, 2.f);
					AddLiveRainFloat(TEXT("기와/식생 젖음 강도"), GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, PuddleSourceWetStrength), 0.f, 1.f);
					AddLiveRainFloat(TEXT("지붕 물방울 흐름 강도"), GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, PuddleRoofFlowStrength), 0.f, 2.f);
					AddLiveRainFloat(TEXT("지붕 물방울 흐름 속도"), GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, PuddleRoofFlowSpeed), 0.f, 3.f);
					AddLiveRainFloat(TEXT("지붕 물GS 간격 (재베이크)"), GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, PuddleRoofMicroSpacingCm), 12.f, 25.f);
				}
				Group.AddWidgetRow().NameContent()
				[ SNew(STextBlock).Text(FText::FromString(TEXT("물 Gaussian 상태"))) ]
				.ValueContent().MinDesiredWidth(280.f)
				[ SNew(STextBlock).AutoWrapText(true).Text_Lambda([ResolveGaussianActor]()
				{
					const auto* A = ResolveGaussianActor();
					return FText::FromString(A ? A->PuddleGaussianBuildStatus : TEXT("액터를 선택하세요"));
				}) ];
			}
			// Restore the manual Stable controls explicitly so proxy replacement/testing
			// never depends on the automatic bootstrap path. These buttons only forward
			// to the already-existing actor functions; no binding/field/render logic changes.
			const bool bIsStableBindingGroup =
				FCString::Strcmp(Spec.Root, TEXT("05 프록시 및 Stable")) == 0 &&
				FCString::Strcmp(Spec.Label, TEXT("02 Stable Binding")) == 0;
			const bool bIsStableFieldGroup =
				FCString::Strcmp(Spec.Root, TEXT("05 프록시 및 Stable")) == 0 &&
				FCString::Strcmp(Spec.Label, TEXT("03 Stable Field")) == 0;

			if (bIsStableBindingGroup)
			{
				Group.AddWidgetRow()
				.WholeRowContent()
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Rebuild Stable Binding")))
					.ToolTipText(FText::FromString(TEXT("현재 Proxy Mesh를 기준으로 모든 splat의 Triangle ID + barycentric Stable Binding을 수동 재구축합니다. Proxy Mesh를 교체하거나 정렬을 바꾼 뒤 실행합니다.")))
					.IsEnabled_Lambda([ResolveGaussianActor]() { return ResolveGaussianActor() != nullptr; })
					.OnClicked_Lambda([ResolveGaussianActor]()
					{
						if (AGaussianSplatActor* Actor = ResolveGaussianActor()) Actor->RebuildStableProxyBinding();
						return FReply::Handled();
					})
				];
			}

			if (bIsStableFieldGroup)
			{
				Group.AddWidgetRow()
				.WholeRowContent()
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Rebuild Stable Field (Stable 3x3)")))
					.ToolTipText(FText::FromString(TEXT("현재 LightDepth 기준으로 카메라 독립 Stable 3x3 그림자 Field를 다시 생성합니다. 카메라 이동 시 그림자 자글거림/반짝임을 막는 Stable 그림자 경로의 수동 재생성 버튼입니다.")))
					.IsEnabled_Lambda([ResolveGaussianActor]() { return ResolveGaussianActor() != nullptr; })
					.OnClicked_Lambda([ResolveGaussianActor]()
					{
						if (AGaussianSplatActor* Actor = ResolveGaussianActor()) Actor->RebuildStableProxyField();
						return FReply::Handled();
					})
				];

				Group.AddWidgetRow()
				.WholeRowContent()
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Bake Stable Cast Angle Cache")))
					.ToolTipText(FText::FromString(TEXT("Directional Light 각도 변화용 Stable Cast 캐시를 베이크합니다. 먼저 Stable Binding이 Ready인지 확인한 뒤 실행합니다.")))
					.IsEnabled_Lambda([ResolveGaussianActor]() { return ResolveGaussianActor() != nullptr; })
					.OnClicked_Lambda([ResolveGaussianActor]()
					{
						if (AGaussianSplatActor* Actor = ResolveGaussianActor()) Actor->BakeStableCastAngleCache();
						return FReply::Handled();
					})
				];
			}

			// Keep the two live source-GS appearance controls together below
			// the bake controls, and immediately above the snow action buttons.
			if (bIsSnowGroup && (SnowColorStrength.IsValid() || SnowSourceCoverStrength.IsValid()))
			{
				Group.AddWidgetRow()
				.WholeRowContent()
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("원본 GS 눈 색상 / 무늬 조정 (실시간)")))
				];
				if (SnowColorStrength.IsValid())
				{
					Group.AddPropertyRow(SnowColorStrength.ToSharedRef());
				}
				if (SnowSourceCoverStrength.IsValid())
				{
					Group.AddPropertyRow(SnowSourceCoverStrength.ToSharedRef());
				}
			}

			// Keep the baked Snow Gaussian actions in the same visible Snow UI group.
			// CallInEditor functions on nested components can be hidden by Unreal's inline actor details,
			// so render explicit buttons here and forward them to the owning GaussianSplatActor.
			if (bIsSnowGroup)
			{
				Group.AddWidgetRow()
				.WholeRowContent()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 4.0f, 8.0f, 2.0f)
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("눈높이 다단계 베이크 재생성")))
						.ToolTipText(FText::FromString(TEXT("0cm=원본만. 베이크 구간 수를 4~12로 선택해 각 단계의 실제 Snow Gaussian/원본 마스크를 사전 생성합니다. 구간을 늘리면 RAM·저장 용량·생성 시간이 증가합니다. 실시간 원본 GS 색상은 연속 조정, Snow 형상은 가장 가까운 저장 단계로 전환합니다. 원본 렌더러는 유지됩니다.")))
						.IsEnabled_Lambda([ResolveGaussianActor]() { return ResolveGaussianActor() != nullptr; })
						.OnClicked_Lambda([ResolveGaussianActor]()
						{
							if (AGaussianSplatActor* Actor = ResolveGaussianActor()) Actor->GenerateSnowGaussians();
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 4.0f, 0.0f, 2.0f)
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Snow Gaussian 삭제")))
						.IsEnabled_Lambda([ResolveGaussianActor]() { return ResolveGaussianActor() != nullptr; })
						.OnClicked_Lambda([ResolveGaussianActor]()
						{
							if (AGaussianSplatActor* Actor = ResolveGaussianActor()) Actor->DeleteSnowGaussians();
							return FReply::Handled();
						})
					]
				];


			}
		}
	}

	// P31: time-of-day grades belong under 03 Colour/Post, not in the
	// 02 lighting controls. Keep each actual property handle and saved value.
	// The three bands were previously ungrouped fallback rows.
	TArray<TSharedRef<IPropertyHandle>> SunsetGrade, DawnGrade, NightGrade;
	for (const TWeakObjectPtr<UObject>& WeakObject : Objects)
	{
		const UObject* Object = WeakObject.Get();
		if (!Object) continue;
		for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property || AddedProperties.Contains(Property) ||
				Property->GetMetaData(TEXT("Category")) != TEXT("03 색상 및 후처리")) continue;
			const FString GroupName = Property->GetMetaData(TEXT("GSGroup"));
			TArray<TSharedRef<IPropertyHandle>>* Target = nullptr;
			if (GroupName == TEXT("05 일몰 전용")) Target = &SunsetGrade;
			else if (GroupName == TEXT("06 새벽 전용")) Target = &DawnGrade;
			else if (GroupName == TEXT("07 밤 전용")) Target = &NightGrade;
			if (!Target) continue;
			const TSharedRef<IPropertyHandle> Handle = DetailBuilder.GetProperty(
				Property->GetFName(), Property->GetOwnerStruct());
			if (!Handle->IsValidHandle()) continue;
			DetailBuilder.HideProperty(Handle);
			AddedProperties.Add(Property);
			Target->Add(Handle);
		}
	}
	if (SunsetGrade.Num() + DawnGrade.Num() + NightGrade.Num() > 0)
	{
		IDetailCategoryBuilder& Root = DetailBuilder.EditCategory(TEXT("03 색상 및 후처리"));
		IDetailGroup& Grade = Root.AddGroup(FName(TEXT("07 낮/밤 색상 보정")),
			FText::FromString(TEXT("07 낮/밤 색상 보정")), false, false);
		auto AddGrade = [&Grade](const TCHAR* Key, const TCHAR* Label,
			const TArray<TSharedRef<IPropertyHandle>>& Handles)
		{
			if (Handles.IsEmpty()) return;
			IDetailGroup& Section = Grade.AddGroup(FName(Key), FText::FromString(Label), false);
			for (const TSharedRef<IPropertyHandle>& Handle : Handles)
			{
				Section.AddPropertyRow(Handle);
			}
		};
		AddGrade(TEXT("P31SunsetGrade"), TEXT("01 노을 보정"), SunsetGrade);
		AddGrade(TEXT("P31DawnGrade"), TEXT("02 새벽 보정"), DawnGrade);
		AddGrade(TEXT("P31NightGrade"), TEXT("03 밤 보정"), NightGrade);
	}

}
