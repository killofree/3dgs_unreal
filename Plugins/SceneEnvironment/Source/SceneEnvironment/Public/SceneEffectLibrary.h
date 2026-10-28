#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SceneEffectLibrary.generated.h"

class UNiagaraComponent;

USTRUCT(BlueprintType)
struct SCENEENVIRONMENT_API FSceneEffectParameter
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FString Path;
    UPROPERTY(BlueprintReadOnly) FString Label;
    UPROPERTY(BlueprintReadOnly) FString Type;
    UPROPERTY(BlueprintReadOnly) FString Value;
    UPROPERTY(BlueprintReadOnly) bool bWritable = false;
};

// Reflection adapter intentionally has no dependency on a specific 3DGS plugin version.
UCLASS()
class SCENEENVIRONMENT_API USceneEffectLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintPure, Category="Scene Effects|Parameters", meta=(DisplayName="List Scene Effect Parameters"))
    static TArray<FSceneEffectParameter> ListParameters(UObject* Target, bool bIncludeComponents = true);
    UFUNCTION(BlueprintCallable, Category="Scene Effects|Parameters", meta=(DisplayName="Read Scene Float"))
    static bool ReadFloat(UObject* Target, const FString& Path, float& Value, FString& Error);
    UFUNCTION(BlueprintCallable, Category="Scene Effects|Parameters", meta=(DisplayName="Write Scene Float"))
    static bool WriteFloat(UObject* Target, const FString& Path, float Value, FString& Error);
    UFUNCTION(BlueprintCallable, Category="Scene Effects|Parameters", meta=(DisplayName="Read Scene Bool"))
    static bool ReadBool(UObject* Target, const FString& Path, bool& Value, FString& Error);
    UFUNCTION(BlueprintCallable, Category="Scene Effects|Parameters", meta=(DisplayName="Write Scene Bool"))
    static bool WriteBool(UObject* Target, const FString& Path, bool Value, FString& Error);
    UFUNCTION(BlueprintCallable, Category="Scene Effects|Parameters", meta=(DisplayName="Read Scene Enum"))
    static bool ReadEnum(UObject* Target, const FString& Path, FString& Value, FString& Error);
    UFUNCTION(BlueprintCallable, Category="Scene Effects|Parameters", meta=(DisplayName="Write Scene Enum By Name"))
    static bool WriteEnum(UObject* Target, const FString& Path, const FString& Value, FString& Error);
    UFUNCTION(BlueprintPure, Category="Scene Effects|Niagara", meta=(DisplayName="List Niagara User Parameters"))
    static TArray<FSceneEffectParameter> ListNiagaraParameters(UNiagaraComponent* Component);
    UFUNCTION(BlueprintCallable, Category="Scene Effects|Niagara", meta=(DisplayName="Read Niagara User Float"))
    static bool ReadNiagaraFloat(UNiagaraComponent* Component, FName Parameter, float& Value, FString& Error);
    UFUNCTION(BlueprintCallable, Category="Scene Effects|Niagara", meta=(DisplayName="Write Niagara User Float"))
    static bool WriteNiagaraFloat(UNiagaraComponent* Component, FName Parameter, float Value, FString& Error);
};
