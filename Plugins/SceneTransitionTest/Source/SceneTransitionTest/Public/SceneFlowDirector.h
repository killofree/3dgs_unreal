#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/DataAsset.h"
#include "Components/SplineComponent.h"
#include "Engine/LatentActionManager.h"
#include "SceneFlowDirector.generated.h"

class USoundBase;
class UAudioComponent;

UENUM(BlueprintType)
enum class ESceneFlowAction : uint8
{
    Wait UMETA(DisplayName="대기 (Wait)"),
    WaitEvent UMETA(DisplayName="이벤트 대기 (WaitEvent)"),
    MovePath UMETA(DisplayName="경로 이동 (MovePath)"),
    PlaceAtMarker UMETA(DisplayName="즉시 배치 (시간 없이 위치 변경)"),
    SetVisible UMETA(DisplayName="대상 표시 / 숨김 (SetVisible)"),
    SetBool UMETA(DisplayName="기능 켜기 / 끄기 (SetBool)"),
    SetFloat UMETA(DisplayName="수치 조절 (SetFloat)"),
    Rotate UMETA(DisplayName="회전 (Rotate)"),
    FadeScreen UMETA(DisplayName="사용자 지정 페이드 (FadeScreen)"),
    PlaySound UMETA(DisplayName="소리 재생 (PlaySound)"),
    StopSound UMETA(DisplayName="소리 정지 (StopSound)"),
    Spawn UMETA(DisplayName="액터 생성 (Spawn)"),
    DestroySpawned UMETA(DisplayName="생성한 액터 삭제 (DestroySpawned)"),
    BlueprintEvent UMETA(DisplayName="블루프린트 이벤트 (BlueprintEvent)"),
    FadeOut UMETA(DisplayName="암전 (FadeOut)"),
    FadeIn UMETA(DisplayName="밝아짐 (FadeIn)"),
    ChangeScene UMETA(DisplayName="씬 전환 (ChangeScene)"),
    MoveToPosition UMETA(DisplayName="직선 이동 (시간 지정)")
};

USTRUCT(BlueprintType)
struct SCENETRANSITIONTEST_API FSceneFlowSlot
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="슬롯 이름")) FName Name;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="슬롯 사용", ToolTip="체크하면 실행합니다. 체크 해제한 슬롯은 건너뜁니다. 대상의 표시 상태와는 별개입니다.")) bool bEnabled = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="동작", EditCondition="bEnabled")) ESceneFlowAction Action = ESceneFlowAction::Wait;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="실행 전 대기 (초)", EditCondition="bEnabled", ClampMin="0")) float Delay = 0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="소요 시간 (초)", ToolTip="ChangeScene에서는 암전 시간입니다. PlaySound는 실제 소리 종료를 기다리므로 이 값을 사용하지 않습니다.", EditCondition="bEnabled && (Action == ESceneFlowAction::Wait || Action == ESceneFlowAction::MovePath || Action == ESceneFlowAction::MoveToPosition || Action == ESceneFlowAction::SetFloat || Action == ESceneFlowAction::Rotate || Action == ESceneFlowAction::FadeScreen || Action == ESceneFlowAction::FadeOut || Action == ESceneFlowAction::FadeIn || Action == ESceneFlowAction::StopSound || Action == ESceneFlowAction::ChangeScene)", EditConditionHides, ClampMin="0")) float Duration = 1;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="완료 후 다음 슬롯", ToolTip="체크 해제하면 다음 슬롯과 병행합니다. 씬 전환은 다른 작업 완료 후 실행합니다.", EditCondition="bEnabled && Action != ESceneFlowAction::ChangeScene")) bool bWaitForCompletion = true;
    // Player is reserved. Other names resolve through Bindings, then unique Actor Tags.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="대상 이름", ToolTip="Player 또는 Bindings 키 / Actor Tag. 직접 액터를 지정하면 그 액터를 우선합니다.", EditCondition="bEnabled && (Action == ESceneFlowAction::MovePath || Action == ESceneFlowAction::MoveToPosition || Action == ESceneFlowAction::PlaceAtMarker || Action == ESceneFlowAction::SetVisible || Action == ESceneFlowAction::SetBool || Action == ESceneFlowAction::SetFloat || Action == ESceneFlowAction::Rotate || Action == ESceneFlowAction::Spawn || Action == ESceneFlowAction::DestroySpawned || (Action == ESceneFlowAction::PlaySound && bSpatialSound))", EditConditionHides)) FName Target;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="대상 액터 직접 지정", EditCondition="bEnabled && (Action == ESceneFlowAction::MovePath || Action == ESceneFlowAction::MoveToPosition || Action == ESceneFlowAction::PlaceAtMarker || Action == ESceneFlowAction::SetVisible || Action == ESceneFlowAction::SetBool || Action == ESceneFlowAction::SetFloat || Action == ESceneFlowAction::Rotate || (Action == ESceneFlowAction::PlaySound && bSpatialSound))", EditConditionHides)) TObjectPtr<AActor> TargetActor;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="기준 위치 / 외부 경로 이름", EditCondition="bEnabled && ((Action == ESceneFlowAction::MovePath && !bUseDirectorPath) || Action == ESceneFlowAction::PlaceAtMarker || Action == ESceneFlowAction::Spawn)", EditConditionHides)) FName MarkerOrPath;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="디렉터 내부 경로 사용", EditCondition="bEnabled && Action == ESceneFlowAction::MovePath", EditConditionHides)) bool bUseDirectorPath = true;
    UPROPERTY() FGuid PathId;
    UPROPERTY() FSplineCurves PathShape;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="편집용 위치 오브젝트 표시", EditCondition="bEnabled && (Action == ESceneFlowAction::PlaceAtMarker || Action == ESceneFlowAction::MoveToPosition || Action == ESceneFlowAction::Spawn)", EditConditionHides)) bool bShowPositionHandle = true;
#if WITH_EDITORONLY_DATA
    UPROPERTY(Transient, VisibleAnywhere, meta=(DisplayName="이 슬롯의 위치 오브젝트", EditCondition="bEnabled && bShowPositionHandle && (Action == ESceneFlowAction::PlaceAtMarker || Action == ESceneFlowAction::MoveToPosition || Action == ESceneFlowAction::Spawn)", EditConditionHides)) TObjectPtr<class ASceneFlowStartHandle> PositionHandle;
#endif
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="배치 위치 (디렉터 기준)", ToolTip="외부 기준 위치 이름이 None일 때 사용합니다. 뷰포트에서 위치 위젯을 움직여 지정할 수 있습니다.", MakeEditWidget, EditCondition="bEnabled && (Action == ESceneFlowAction::PlaceAtMarker || Action == ESceneFlowAction::Spawn)", EditConditionHides)) FTransform Placement = FTransform::Identity;
    // World coordinates: no separate marker actor is required.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="목적지 (월드 좌표)", EditCondition="bEnabled && Action == ESceneFlowAction::MoveToPosition", EditConditionHides)) FVector Destination = FVector::ZeroVector;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="전환할 레벨", EditCondition="bEnabled && Action == ESceneFlowAction::ChangeScene", EditConditionHides, AllowedClasses="/Script/Engine.World")) FSoftObjectPath DestinationMap;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="전환 후 밝아지는 시간 (초)", EditCondition="bEnabled && Action == ESceneFlowAction::ChangeScene", EditConditionHides, ClampMin="0")) float FadeInSeconds = 1.5f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="속성 / 이벤트 이름", EditCondition="bEnabled && (Action == ESceneFlowAction::SetBool || Action == ESceneFlowAction::SetFloat || Action == ESceneFlowAction::WaitEvent || Action == ESceneFlowAction::BlueprintEvent)", EditConditionHides)) FName PropertyOrEvent;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="목표 수치 / 종료 불투명도", EditCondition="bEnabled && (Action == ESceneFlowAction::SetFloat || Action == ESceneFlowAction::FadeScreen)", EditConditionHides)) float Value = 0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ScriptName="ToggleValue", DisplayName="대상 켜기 (해제 = 끄기)", ToolTip="SetVisible: 체크하면 표시, 해제하면 숨김. SetBool: 체크하면 true, 해제하면 false.", EditCondition="bEnabled && (Action == ESceneFlowAction::SetVisible || Action == ESceneFlowAction::SetBool)", EditConditionHides)) bool bValue = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="하위 메쉬도 함께 적용", EditCondition="bEnabled && Action == ESceneFlowAction::SetVisible", EditConditionHides)) bool bIncludeComponents = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="부드러운 출발 / 도착", EditCondition="bEnabled && (Action == ESceneFlowAction::MovePath || Action == ESceneFlowAction::MoveToPosition)", EditConditionHides)) bool bEaseMovement = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="목표 회전", EditCondition="bEnabled && Action == ESceneFlowAction::Rotate", EditConditionHides)) FRotator Rotation = FRotator::ZeroRotator;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="페이드 색상", EditCondition="bEnabled && (Action == ESceneFlowAction::FadeScreen || Action == ESceneFlowAction::FadeOut || Action == ESceneFlowAction::FadeIn || Action == ESceneFlowAction::ChangeScene)", EditConditionHides)) FLinearColor FadeColor = FLinearColor::Black;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="재생할 소리", EditCondition="bEnabled && Action == ESceneFlowAction::PlaySound", EditConditionHides)) TObjectPtr<USoundBase> Sound;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="오디오 채널 이름", EditCondition="bEnabled && (Action == ESceneFlowAction::PlaySound || Action == ESceneFlowAction::StopSound)", EditConditionHides)) FName AudioChannel = TEXT("Ambience");
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="음량 / 시작 불투명도", EditCondition="bEnabled && (Action == ESceneFlowAction::PlaySound || Action == ESceneFlowAction::FadeScreen)", EditConditionHides, ClampMin="0")) float Volume = 1;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="대상 위치에서 공간 음향 재생", EditCondition="bEnabled && Action == ESceneFlowAction::PlaySound", EditConditionHides)) bool bSpatialSound = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="생성할 액터 클래스", EditCondition="bEnabled && Action == ESceneFlowAction::Spawn", EditConditionHides)) TSubclassOf<AActor> SpawnClass;
};

UCLASS(BlueprintType)
class SCENETRANSITIONTEST_API USceneFlowAsset : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(TitleProperty="Name", DisplayName="연출 슬롯")) TArray<FSceneFlowSlot> Slots;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSceneFlowSlotEvent, int32, Index, FName, Name);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSceneFlowError, FString, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSceneFlowFinished);

// Place one per level. Blueprint subclasses handle project-specific effects via OnSlotStarted.
UCLASS(Blueprintable)
class SCENETRANSITIONTEST_API ASceneFlowDirector : public AActor
{
    GENERATED_BODY()
public:
    ASceneFlowDirector();
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void OnConstruction(const FTransform& Transform) override;
    UFUNCTION(BlueprintCallable, CallInEditor, Category="Scene Flow", meta=(DisplayName="슬롯 위치·경로 표시 갱신")) void RebuildSlotPaths();
    UFUNCTION(BlueprintCallable, Category="Scene Flow|Editor") bool SelectSlotEditorHandle(int32 SlotIndex);
    UPROPERTY(EditAnywhere, Category="Scene Flow|Editor", meta=(DisplayName="선택한 슬롯의 위치·경로만 표시")) bool bShowOnlySelectedSlot = true;
    UFUNCTION(BlueprintCallable, Category="Scene Flow") void SaveSlotPositionFromHandle(FGuid SlotId, FTransform WorldTransform);
    UFUNCTION(BlueprintPure, Category="Scene Flow") AActor* GetSlotPositionHandle(int32 SlotIndex) const;
    UFUNCTION(BlueprintPure, Category="Scene Flow") USplineComponent* GetSlotPath(int32 SlotIndex) const;
    UFUNCTION(BlueprintCallable, CallInEditor, Category="Scene Flow", meta=(DisplayName="선택 경로 시작점으로 시작 위치 설정")) void SetStartFromPath();
    UFUNCTION(BlueprintCallable, CallInEditor, Category="Scene Flow|Start", meta=(DisplayName="시작 위치 오브젝트 편집")) void EditStartPosition();
    UFUNCTION(BlueprintCallable, Category="Scene Flow|Start") void SaveStartPositionFromHandle(FVector WorldLocation);
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Scene Flow|Start", meta=(DisplayName="시작 위치 사용")) bool bUseStartPosition = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Scene Flow|Start", meta=(DisplayName="시작 위치 (디렉터 기준)", MakeEditWidget, EditCondition="bUseStartPosition")) FVector StartPosition = FVector::ZeroVector;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Scene Flow|Start", meta=(DisplayName="시작점 기준 경로 슬롯", ClampMin="0")) int32 StartPathSlotIndex = 0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Scene Flow|Start", meta=(DisplayName="시작 위치와 경로 첫 점 연결", ToolTip="시작 위치 오브젝트를 옮기면 지정한 경로 슬롯의 첫 점도 같이 이동합니다.")) bool bLinkStartToPath = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Scene Flow") TObjectPtr<USceneFlowAsset> FlowAsset;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Scene Flow", meta=(TitleProperty="Name", DisplayName="연출 슬롯", EditCondition="FlowAsset == nullptr")) TArray<FSceneFlowSlot> Slots;
    UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category="Scene Flow") TMap<FName, TObjectPtr<AActor>> Bindings;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Scene Flow") bool bAutoStart = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Scene Flow", meta=(DisplayName="검정 화면에서 시작", ToolTip="로딩 후에도 검정 화면을 유지합니다. 슬롯의 밝아짐(FadeIn)으로 첫 화면을 표시하세요.")) bool bStartWithBlack = true;
    UPROPERTY(BlueprintReadOnly, Category="Scene Flow") bool bRunning = false;
    UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category="Scene Flow|Playback", meta=(DisplayName="현재 슬롯 번호")) int32 CurrentSlotIndex = -1;
    UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category="Scene Flow|Playback", meta=(DisplayName="현재 슬롯 이름")) FName CurrentSlotName;
    UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category="Scene Flow|Playback", meta=(DisplayName="진행 상태")) FString PlaybackStatus = TEXT("정지");
    UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category="Scene Flow|Playback", meta=(DisplayName="마지막 오류")) FString LastError;
    UPROPERTY(BlueprintAssignable, Category="Scene Flow") FSceneFlowSlotEvent OnSlotStarted;
    UPROPERTY(BlueprintAssignable, Category="Scene Flow") FSceneFlowSlotEvent OnSlotCompleted;
    UPROPERTY(BlueprintAssignable, Category="Scene Flow") FSceneFlowError OnError;
    UPROPERTY(BlueprintAssignable, Category="Scene Flow") FSceneFlowFinished OnFinished;
    // Graph mode uses the existing editable positions/effects but never advances slots itself.
    UFUNCTION(BlueprintCallable, Category="Scene Flow|Graph", meta=(DisplayName="Begin Scene Graph")) void BeginSceneGraph();
    UFUNCTION(BlueprintCallable, Category="Scene Flow|Graph", meta=(Latent, LatentInfo="LatentInfo", DisplayName="Run Scene Step", ToolTip="Run one named slot. Completed resumes the graph after this action finishes. Disabled slots are skipped. Errors stop the graph and broadcast OnError. DurationOverride -1 uses the slot setting."))
    void RunSceneStep(FName StepName, float DurationOverride, FLatentActionInfo LatentInfo);
    UFUNCTION(BlueprintCallable, Category="Scene Flow|Graph", meta=(DisplayName="Finish Scene Graph")) void FinishSceneGraph();
    uint64 GetGraphCompletedStep() const { return GraphCompletedStep; }
    uint64 GetGraphGeneration() const { return RunVersion; }
    UFUNCTION(BlueprintCallable, Category="Scene Flow") void StartFlow(int32 StartIndex = 0);
    UFUNCTION(BlueprintCallable, Category="Scene Flow") void StopFlow();
    UFUNCTION(BlueprintCallable, Category="Scene Flow") void SendEvent(FName Event);
    UFUNCTION(BlueprintCallable, CallInEditor, Category="Scene Flow", meta=(DisplayName="첫 씬 예제 추가 (빈 목록 전용)")) void CreateFirstCutTemplate();
    UFUNCTION(BlueprintPure, Category="Scene Flow") AActor* ResolveTarget(FName Name) const;
protected:
    virtual void BeginPlay() override;
private:
    struct FJob
    {
        int32 Index = 0;
        float Time = 0;
        bool bStarted = false;
        uint64 EventVersion = 0;
        float InitialValue = 0;
        FRotator InitialRotation = FRotator::ZeroRotator;
        FVector InitialPosition = FVector::ZeroVector;
        TWeakObjectPtr<AActor> Actor;
        TWeakObjectPtr<AActor> Reference;
        TWeakObjectPtr<USplineComponent> Spline;
        TWeakObjectPtr<UAudioComponent> Audio;
    };
    UPROPERTY(Transient) TArray<FSceneFlowSlot> RunningSlots;
    UPROPERTY(Transient) TMap<FName, TObjectPtr<UAudioComponent>> AudioChannels;
    UPROPERTY(Transient) TMap<FName, TObjectPtr<AActor>> Spawned;
    UPROPERTY(VisibleInstanceOnly, Category="Scene Flow|Paths", meta=(DisplayName="슬롯 소속 경로")) TMap<FGuid, TObjectPtr<USplineComponent>> SlotPaths;
    bool bGraphMode = false;
    uint64 GraphStep = 0;
    uint64 GraphCompletedStep = 0;
    bool bUpdatingPaths = false;
#if WITH_EDITORONLY_DATA
    UPROPERTY() TObjectPtr<class ASceneFlowStartHandle> StartHandle;
    UPROPERTY() TMap<FGuid, TObjectPtr<class ASceneFlowStartHandle>> SlotPositionHandles;
    UPROPERTY(Transient) FGuid EditingSlotId;
    bool bEditingStartHandle = false;
#endif
    void ApplyEditorSlotVisibility();
    void RefreshPositionHandles();
    bool bUpdatingHandles = false;
    bool bStartPlaced = false;
    TMap<FName, uint64> Events;
    TArray<FJob> Jobs;
    int32 NextIndex = 0;
    bool bWaitingForStartup = false;
    uint64 RunVersion = 0;
    bool bHandingOff = false;
    bool UpdateJob(FJob& Job, float DeltaSeconds);
    void Fail(const FString& Message);
    void CleanupFlow(bool bCancelContinuation);
};
