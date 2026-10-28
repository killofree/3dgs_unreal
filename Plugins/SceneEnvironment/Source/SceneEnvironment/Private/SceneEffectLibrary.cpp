#include "SceneEffectLibrary.h"
#include "Components/ActorComponent.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraParameterStore.h"
#include "UObject/UnrealType.h"

namespace
{
bool IsReadable(const FProperty* Property)
{
    return Property && Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible);
}
bool IsWritable(const FProperty* Property)
{
    return IsReadable(Property) && !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_BlueprintReadOnly);
}
bool Resolve(UObject* Target, const FString& Path, UObject*& Owner, FProperty*& Property, FString& Error, bool bWrite)
{
    Error.Reset(); Owner = Target; Property = nullptr;
    TArray<FString> Parts; Path.ParseIntoArray(Parts, TEXT("."), false);
    if (!IsValid(Owner) || Parts.IsEmpty()) { Error = TEXT("Target or property path is missing."); return false; }
    for (int32 Index = 0; Index < Parts.Num(); ++Index)
    {
        Property = FindFProperty<FProperty>(Owner->GetClass(), FName(*Parts[Index]));
        if (!IsReadable(Property)) { Error = FString::Printf(TEXT("Property not found/exposed: %s on %s"), *Parts[Index], *Owner->GetClass()->GetName()); return false; }
        if (Index + 1 < Parts.Num())
        {
            auto* Object = CastField<FObjectPropertyBase>(Property);
            Owner = Object ? Object->GetObjectPropertyValue_InContainer(Owner) : nullptr;
            if (!IsValid(Owner)) { Error = TEXT("An intermediate object is missing; use List Scene Effect Parameters to inspect the target."); return false; }
        }
    }
    if (bWrite && !IsWritable(Property)) { Error = TEXT("Property is read-only."); return false; }
    return true;
}
UEnum* EnumFor(FProperty* Property)
{
    if (auto* Enum = CastField<FEnumProperty>(Property)) return Enum->GetEnum();
    if (auto* Byte = CastField<FByteProperty>(Property)) return Byte->Enum;
    return nullptr;
}
FNumericProperty* EnumStorage(FProperty* Property)
{
    if (auto* Enum = CastField<FEnumProperty>(Property)) return Enum->GetUnderlyingProperty();
    return CastField<FByteProperty>(Property);
}
void AppendParameters(UObject* Object, const FString& Prefix, TArray<FSceneEffectParameter>& Result, bool bComponents)
{
    for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
    {
        FProperty* Property = *It;
        if (!IsReadable(Property)) continue;
        const FString Path = Prefix + Property->GetName();
        if (bComponents) if (auto* Ref = CastField<FObjectPropertyBase>(Property))
            if (auto* Component = Cast<UActorComponent>(Ref->GetObjectPropertyValue_InContainer(Object)))
                AppendParameters(Component, Path + TEXT("."), Result, false);
        FSceneEffectParameter Entry; Entry.Path = Path;
#if WITH_EDITOR
        Entry.Label = Property->GetDisplayNameText().ToString();
#else
        Entry.Label = Property->GetName();
#endif
        Entry.bWritable = IsWritable(Property);
        if (UEnum* Enum = EnumFor(Property))
        {
            Entry.Type = TEXT("Enum");
            Entry.Value = Enum->GetNameStringByValue(EnumStorage(Property)->GetSignedIntPropertyValue(Property->ContainerPtrToValuePtr<void>(Object)));
        }
        else if (auto* Number = CastField<FNumericProperty>(Property); Number && Number->IsFloatingPoint())
        { Entry.Type = TEXT("Float"); Entry.Value = FString::SanitizeFloat(Number->GetFloatingPointPropertyValue(Property->ContainerPtrToValuePtr<void>(Object))); }
        else if (auto* Bool = CastField<FBoolProperty>(Property))
        { Entry.Type = TEXT("Bool"); Entry.Value = Bool->GetPropertyValue_InContainer(Object) ? TEXT("true") : TEXT("false"); }
        else continue;
        Result.Add(MoveTemp(Entry));
    }
}
FName UserName(FName Name)
{
    const FString Text = Name.ToString();
    return Text.StartsWith(TEXT("User.")) ? Name : FName(*(TEXT("User.") + Text));
}
bool ValidateNiagaraFloat(UNiagaraComponent* Component, FName& Name, FString& Error)
{
    Error.Reset();
    if (!IsValid(Component) || !Component->GetAsset() || Name.IsNone()) { Error = TEXT("Niagara component, system or parameter is missing."); return false; }
    Name = UserName(Name);
    TArray<FNiagaraVariable> Variables; Component->GetAsset()->GetExposedParameters().GetParameters(Variables);
    const auto* Variable = Variables.FindByPredicate([Name](const FNiagaraVariable& Item) { return Item.GetName() == Name; });
    if (!Variable) { Error = FString::Printf(TEXT("System does not expose %s; inspect List Niagara User Parameters."), *Name.ToString()); return false; }
    if (Variable->GetType() != FNiagaraTypeDefinition::GetFloatDef()) { Error = TEXT("Niagara parameter exists but is not a float."); return false; }
    return true;
}
}

TArray<FSceneEffectParameter> USceneEffectLibrary::ListParameters(UObject* Target, bool bIncludeComponents)
{
    TArray<FSceneEffectParameter> Result;
    if (IsValid(Target)) AppendParameters(Target, TEXT(""), Result, bIncludeComponents);
    Result.Sort([](const auto& A, const auto& B) { return A.Path < B.Path; });
    return Result;
}
bool USceneEffectLibrary::ReadFloat(UObject* Target, const FString& Path, float& Value, FString& Error)
{
    Value = 0; UObject* Owner; FProperty* Property;
    if (!Resolve(Target, Path, Owner, Property, Error, false)) return false;
    auto* Number = CastField<FNumericProperty>(Property);
    if (!Number || !Number->IsFloatingPoint()) { Error = TEXT("Property is not a float/double."); return false; }
    Value = Number->GetFloatingPointPropertyValue(Property->ContainerPtrToValuePtr<void>(Owner)); return true;
}
bool USceneEffectLibrary::WriteFloat(UObject* Target, const FString& Path, float Value, FString& Error)
{
    UObject* Owner; FProperty* Property;
    if (!FMath::IsFinite(Value)) { Error = TEXT("Value must be finite."); return false; }
    if (!Resolve(Target, Path, Owner, Property, Error, true)) return false;
    auto* Number = CastField<FNumericProperty>(Property);
    if (!Number || !Number->IsFloatingPoint()) { Error = TEXT("Property is not a float/double."); return false; }
    Number->SetFloatingPointPropertyValue(Property->ContainerPtrToValuePtr<void>(Owner), Value); return true;
}
bool USceneEffectLibrary::ReadBool(UObject* Target, const FString& Path, bool& Value, FString& Error)
{
    Value = false; UObject* Owner; FProperty* Property;
    if (!Resolve(Target, Path, Owner, Property, Error, false)) return false;
    auto* Bool = CastField<FBoolProperty>(Property);
    if (!Bool) { Error = TEXT("Property is not a bool."); return false; }
    Value = Bool->GetPropertyValue_InContainer(Owner); return true;
}
bool USceneEffectLibrary::WriteBool(UObject* Target, const FString& Path, bool Value, FString& Error)
{
    UObject* Owner; FProperty* Property;
    if (!Resolve(Target, Path, Owner, Property, Error, true)) return false;
    auto* Bool = CastField<FBoolProperty>(Property);
    if (!Bool) { Error = TEXT("Property is not a bool."); return false; }
    Bool->SetPropertyValue_InContainer(Owner, Value); return true;
}
bool USceneEffectLibrary::ReadEnum(UObject* Target, const FString& Path, FString& Value, FString& Error)
{
    Value.Reset(); UObject* Owner; FProperty* Property;
    if (!Resolve(Target, Path, Owner, Property, Error, false)) return false;
    UEnum* Enum = EnumFor(Property);
    if (!Enum) { Error = TEXT("Property is not an enum."); return false; }
    Value = Enum->GetNameStringByValue(EnumStorage(Property)->GetSignedIntPropertyValue(Property->ContainerPtrToValuePtr<void>(Owner))); return true;
}
bool USceneEffectLibrary::WriteEnum(UObject* Target, const FString& Path, const FString& Value, FString& Error)
{
    UObject* Owner; FProperty* Property;
    if (!Resolve(Target, Path, Owner, Property, Error, true)) return false;
    UEnum* Enum = EnumFor(Property);
    if (!Enum) { Error = TEXT("Property is not an enum."); return false; }
    const int64 Number = Enum->GetValueByNameString(Value);
    if (Number == INDEX_NONE || Enum->GetNameStringByValue(Number).EndsWith(TEXT("MAX"))) { Error = TEXT("Unknown enum entry; pass the native entry name, e.g. Fog or Rain."); return false; }
    EnumStorage(Property)->SetIntPropertyValue(Property->ContainerPtrToValuePtr<void>(Owner), uint64(Number)); return true;
}
TArray<FSceneEffectParameter> USceneEffectLibrary::ListNiagaraParameters(UNiagaraComponent* Component)
{
    TArray<FSceneEffectParameter> Result;
    if (!IsValid(Component) || !Component->GetAsset()) return Result;
    TArray<FNiagaraVariable> Variables; Component->GetAsset()->GetExposedParameters().GetParameters(Variables);
    for (const auto& Variable : Variables)
    {
        FSceneEffectParameter Entry; Entry.Path = Variable.GetName().ToString(); Entry.Label = Entry.Path; Entry.Type = Variable.GetType().GetName();
        Entry.bWritable = Variable.GetType() == FNiagaraTypeDefinition::GetFloatDef();
        if (Entry.bWritable) { float Value; FString Error; if (ReadNiagaraFloat(Component, Variable.GetName(), Value, Error)) Entry.Value = FString::SanitizeFloat(Value); }
        Result.Add(MoveTemp(Entry));
    }
    return Result;
}
bool USceneEffectLibrary::ReadNiagaraFloat(UNiagaraComponent* Component, FName Parameter, float& Value, FString& Error)
{
    Value = 0;
    if (!ValidateNiagaraFloat(Component, Parameter, Error)) return false;
    bool bValid = false; Value = Component->GetVariableFloat(Parameter, bValid);
    if (!bValid) Value = Component->GetAsset()->GetExposedParameters().GetParameterValue<float>(FNiagaraVariableBase(FNiagaraTypeDefinition::GetFloatDef(), Parameter));
    return true;
}
bool USceneEffectLibrary::WriteNiagaraFloat(UNiagaraComponent* Component, FName Parameter, float Value, FString& Error)
{
    if (!FMath::IsFinite(Value)) { Error = TEXT("Value must be finite."); return false; }
    if (!ValidateNiagaraFloat(Component, Parameter, Error)) return false;
    Component->SetVariableFloat(Parameter, Value); return true;
}
