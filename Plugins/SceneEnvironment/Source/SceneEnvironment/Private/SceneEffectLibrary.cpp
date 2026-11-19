#include "SceneEffectLibrary.h"
#include "SceneSeasonController.h"
#include "Components/ActorComponent.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystemInstanceController.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraEmitterInstance.h"
#if WITH_EDITOR
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraParameterMapHistory.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#endif
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
    Number->SetFloatingPointPropertyValue(Property->ContainerPtrToValuePtr<void>(Owner), Value);
    // SceneFlow reflection writes must apply uniforms immediately, including zero-duration slots.
    if (auto* Controls=Cast<ASceneSeasonController>(Owner)) Controls->ApplyControls();
    return true;
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

bool USceneEffectLibrary::AuthorWorldSpaceSnow(UNiagaraSystem* System)
{
#if WITH_EDITOR
    if (!System) return false;
    System->Modify();
    const FNiagaraVariable RateVariable(FNiagaraTypeDefinition::GetFloatDef(),TEXT("User.SeasonSnowSpawnRate"));
    System->GetExposedParameters().SetParameterValue<float>(700.f,RateVariable,true);
    for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
    {
        FVersionedNiagaraEmitterData* Data=Handle.GetInstance().GetEmitterData();
        if(!Data)continue;
        Data->bLocalSpace=false;
        UNiagaraScriptSource* Source=Cast<UNiagaraScriptSource>(Data->GraphSource);
        UNiagaraGraph* Graph=Source?Source->NodeGraph:nullptr;
        if(!Graph)return false;
        Graph->Modify();
        TArray<UNiagaraNodeFunctionCall*> Calls;Graph->GetNodesOfClass(Calls);
        for(UNiagaraNodeFunctionCall* Call:Calls)
        {
            if(!Call->FunctionScript || Call->FunctionScript->GetUsage()!=ENiagaraScriptUsage::Module)continue;
            const FString Name=Call->FunctionScript->GetName();
            if(Name.Contains(TEXT("SpawnRate")))
            {
                const auto Aliased=FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(FNiagaraParameterHandle(FName(TEXT("Module.SpawnRate"))),Call);
                UEdGraphPin& Pin=FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(*Call,Aliased,FNiagaraTypeDefinition::GetFloatDef(),FGuid(),FGuid());
                TSet<FNiagaraVariableBase> Known;Known.Add(RateVariable);
                FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(Pin,RateVariable,Known);
            }
            const bool Solve=Name.Contains(TEXT("SolveForcesAndVelocity"));
            const bool Motion=Name.Contains(TEXT("Force")) || Name.Contains(TEXT("Velocity")) ||
                Name.Contains(TEXT("Gravity")) || Name.Contains(TEXT("Wind")) ||
                Name.Contains(TEXT("CurlNoise")) || Name.Contains(TEXT("Camera")) || Name.Contains(TEXT("Drag")) || Name.Contains(TEXT("Orbit"));
            if(Motion || Solve || Name.Contains(TEXT("SpawnBurst")))
            {
                FNiagaraStackGraphUtilities::SetModuleIsEnabled(*Call,false);
                UE_LOG(LogTemp,Display,TEXT("[SequenceSnowMotion] Disabled %s"),*Name);
            }
        }
        TArray<UNiagaraNodeOutput*> Outputs;Graph->GetNodesOfClass(Outputs);
        for(UNiagaraNodeOutput* Output:Outputs)
        {
            const bool Spawn=Output->GetUsage()==ENiagaraScriptUsage::ParticleSpawnScript ||
                Output->GetUsage()==ENiagaraScriptUsage::ParticleSpawnScriptInterpolated;
            const bool Update=Output->GetUsage()==ENiagaraScriptUsage::ParticleUpdateScript;
            if(!Spawn && !Update)continue;
            TArray<FNiagaraVariable> Variables;
            Variables.Emplace(FNiagaraTypeDefinition::GetVec3Def(),TEXT("Particles.Velocity"));
            TArray<FString> Values={TEXT("(X=0,Y=0,Z=-120)")};
            if(Update)
            {
                Variables.Emplace(FNiagaraTypeDefinition::GetVec3Def(),TEXT("Particles.PhysicsForce"));
                Values.Add(TEXT("(X=0,Y=0,Z=0)"));
            }
            UNiagaraNodeAssignment* Assignment=FNiagaraStackGraphUtilities::AddParameterModuleToStack(
                Variables,*Output,Spawn?INDEX_NONE:0,Values);
            if(!Assignment)return false;
            Assignment->NodeComment=TEXT("Season snow: world down velocity, no camera-directed force");
            if(Update)
            {
                UNiagaraScript* Solver=LoadObject<UNiagaraScript>(nullptr,TEXT("/Niagara/Modules/Solvers/SolveForcesAndVelocity.SolveForcesAndVelocity"));
                if(!Solver || !FNiagaraStackGraphUtilities::AddScriptModuleToStack(Solver,*Output))return false;
            }
        }
    }
    System->RequestCompile(true);
    System->WaitForCompilationComplete(true, false);
    if(!System->IsValid())return false;
    System->MarkPackageDirty();
    return true;
#else
    return false;
#endif
}





bool USceneEffectLibrary::AuthorWaterfallParticles(UNiagaraSystem* System, UMaterialInterface* Material, bool bMist)
{
#if WITH_EDITOR
    if (!System || !Material) return false;
    System->Modify();
    auto& Parameters=System->GetExposedParameters();
    const FNiagaraVariable Rate(FNiagaraTypeDefinition::GetFloatDef(),TEXT("User.FallsSpawnRate"));
    const FNiagaraVariable Velocity(FNiagaraTypeDefinition::GetVec3Def(),TEXT("User.FallsVelocity"));
    const FNiagaraVariable Force(FNiagaraTypeDefinition::GetVec3Def(),TEXT("User.FallsForce"));
    const FNiagaraVariable Size(FNiagaraTypeDefinition::GetVec2Def(),TEXT("User.FallsSize"));
    const FNiagaraVariable Life(FNiagaraTypeDefinition::GetFloatDef(),TEXT("User.FallsLifetime"));
    const FNiagaraVariable Color(FNiagaraTypeDefinition::GetColorDef(),TEXT("User.FallsColor"));
    const FNiagaraVariable Extent(FNiagaraTypeDefinition::GetVec3Def(),TEXT("User.FallsSpawnExtent"));
    const FNiagaraVariable Radius(FNiagaraTypeDefinition::GetFloatDef(),TEXT("User.FallsSpawnRadius"));
    const FNiagaraVariable Offset(FNiagaraTypeDefinition::GetVec3Def(),TEXT("User.FallsSpawnOffset"));
    Parameters.SetParameterValue<float>(bMist?45.f:220.f,Rate,true);
    Parameters.SetParameterValue<FVector3f>(bMist?FVector3f(35,-55,65):FVector3f(0,-100,-650),Velocity,true);
    Parameters.SetParameterValue<FVector3f>(bMist?FVector3f(0,0,-4):FVector3f(0,0,-450),Force,true);
    Parameters.SetParameterValue<FVector2f>(bMist?FVector2f(130,160):FVector2f(3,18),Size,true);
    Parameters.SetParameterValue<float>(bMist?3.8f:2.3f,Life,true);
    Parameters.SetParameterValue<FLinearColor>(bMist?FLinearColor(.68f,.73f,.76f,.20f):FLinearColor(.8f,.85f,.88f,.65f),Color,true);
    Parameters.SetParameterValue<FVector3f>(bMist?FVector3f(220,130,100):FVector3f(110,65,120),Extent,true);
    Parameters.SetParameterValue<float>(bMist?35.f:45.f,Radius,true);
    Parameters.SetParameterValue<FVector3f>(FVector3f::ZeroVector,Offset,true);
    auto Link=[&](UNiagaraNodeFunctionCall* Call,const FNiagaraVariable& Input,const FNiagaraVariable& User)
    {
        const auto Alias=FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(FNiagaraParameterHandle(Input.GetName()),Call);
        UEdGraphPin& Pin=FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(*Call,Alias,Input.GetType(),FGuid(),FGuid());
        Pin.BreakAllPinLinks();
        TSet<FNiagaraVariableBase> Known; Known.Add(User);
        FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(Pin,User,Known);
    };
    int32 Renderers=0,SpawnModules=0;
    for (const FNiagaraEmitterHandle& Handle:System->GetEmitterHandles())
    {
        auto* Data=Handle.GetInstance().GetEmitterData(); if (!Data) continue;
        Data->bLocalSpace=false; Data->SimTarget=ENiagaraSimTarget::CPUSim;
        for (UNiagaraRendererProperties* Renderer:Data->GetRenderers())
            if (auto* Sprite=Cast<UNiagaraSpriteRendererProperties>(Renderer))
            { Sprite->Modify(); Sprite->Material=Material; Sprite->MICMaterial=nullptr; Sprite->MaterialUserParamBinding=FNiagaraUserParameterBinding(); Sprite->SubImageSize=FVector2D(1,1); Sprite->Alignment=bMist?ENiagaraSpriteAlignment::Unaligned:ENiagaraSpriteAlignment::VelocityAligned; Sprite->FacingMode=ENiagaraSpriteFacingMode::FaceCameraPosition; ++Renderers; }
        auto* Source=Cast<UNiagaraScriptSource>(Data->GraphSource);
        UNiagaraGraph* Graph=Source?Source->NodeGraph.Get():nullptr; if (!Graph) return false;
        Graph->Modify();
        TArray<UNiagaraNodeFunctionCall*> Calls;Graph->GetNodesOfClass(Calls);
        for (auto* Call:Calls)
        {
            if (!Call->FunctionScript || Call->FunctionScript->GetUsage()!=ENiagaraScriptUsage::Module) continue;
            const FString Name=Call->FunctionScript->GetName();
            if (Name.Contains(TEXT("SpawnRate")))
            { Link(Call,FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(),TEXT("Module.SpawnRate")),Rate); ++SpawnModules; }
            if (Name.Contains(TEXT("Shape")) || Name.Contains(TEXT("Location")))
            {
                TArray<FNiagaraVariable> Inputs;
                FNiagaraStackGraphUtilities::GetStackFunctionInputsLegacy(*Call,Inputs,FCompileConstantResolver(Handle.GetInstance(),ENiagaraScriptUsage::ParticleSpawnScript),FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);
                for (const auto& Input:Inputs)
                {
                    const FString Key=Input.GetName().ToString();
                    UE_LOG(LogTemp,Display,TEXT("[FallsShape] %s : %s"),*Name,*Key);
                    if (Input.GetType()==FNiagaraTypeDefinition::GetVec3Def() && (Key.Contains(TEXT("Box Size")) || Key.Contains(TEXT("BoxSize")) || Key.Contains(TEXT("Box Dimensions")))) Link(Call,Input,Extent);
                    if (Input.GetType()==FNiagaraTypeDefinition::GetFloatDef() && (Key.EndsWith(TEXT("Sphere Radius")) || Key.EndsWith(TEXT("Cylinder Radius")) || Key.EndsWith(TEXT("Ring Radius")))) Link(Call,Input,Radius);
                    if (Input.GetType()==FNiagaraTypeDefinition::GetVec3Def() && (Key.EndsWith(TEXT(".Offset")) || Key.EndsWith(TEXT("Box Midpoint")))) Link(Call,Input,Offset);
                }
            }
            if (Name.Contains(TEXT("Force")) || Name.Contains(TEXT("Velocity")) || Name.Contains(TEXT("Gravity")) || Name.Contains(TEXT("Wind")) || Name.Contains(TEXT("CurlNoise")) || Name.Contains(TEXT("Camera")) || Name.Contains(TEXT("Drag")) || Name.Contains(TEXT("Orbit")) || Name.Contains(TEXT("Collision")) || Name.Contains(TEXT("SpawnBurst")) || Name.Contains(TEXT("ScaleColor")) || Name.Contains(TEXT("ScaleSpriteSize")))
                FNiagaraStackGraphUtilities::SetModuleIsEnabled(*Call,false);
        }
        TArray<UNiagaraNodeOutput*> Outputs;Graph->GetNodesOfClass(Outputs);
        for (auto* Output:Outputs)
        {
            const bool Spawn=Output->GetUsage()==ENiagaraScriptUsage::ParticleSpawnScript || Output->GetUsage()==ENiagaraScriptUsage::ParticleSpawnScriptInterpolated;
            const bool Update=Output->GetUsage()==ENiagaraScriptUsage::ParticleUpdateScript;
            if (!Spawn && !Update) continue;
            TArray<FNiagaraVariable> Variables;TArray<FString> Values;TArray<FNiagaraVariable> Users;
            if (Spawn)
            {
                Variables={FNiagaraVariable(Velocity.GetType(),TEXT("Particles.Velocity")),FNiagaraVariable(Size.GetType(),TEXT("Particles.SpriteSize")),FNiagaraVariable(Life.GetType(),TEXT("Particles.Lifetime")),FNiagaraVariable(Color.GetType(),TEXT("Particles.Color"))};
                Values={TEXT("(X=0,Y=0,Z=0)"),TEXT("(X=1,Y=1)"),TEXT("2"),TEXT("(R=1,G=1,B=1,A=1)")};Users={Velocity,Size,Life,Color};
            }
            else { Variables={FNiagaraVariable(Force.GetType(),TEXT("Particles.PhysicsForce"))};Values={TEXT("(X=0,Y=0,Z=0)")};Users={Force}; }
            auto* Assignment=FNiagaraStackGraphUtilities::AddParameterModuleToStack(Variables,*Output,Spawn?INDEX_NONE:0,Values);
            if (!Assignment) return false;
            for (int32 I=0;I<Variables.Num();++I) Link(Assignment,FNiagaraVariable(Variables[I].GetType(),FName(*(TEXT("Module.")+Variables[I].GetName().ToString()))),Users[I]);
            Assignment->NodeComment=TEXT("Waterfall: independently controlled spray / mist particles");
            if (Update)
            {
                auto* Solver=LoadObject<UNiagaraScript>(nullptr,TEXT("/Niagara/Modules/Solvers/SolveForcesAndVelocity.SolveForcesAndVelocity"));
                if (!Solver || !FNiagaraStackGraphUtilities::AddScriptModuleToStack(Solver,*Output)) return false;
            }
        }
    }
    System->SetFixedBounds(FBox(FVector(-2000,-2000,-6000),FVector(2000,2000,2000)));
    System->RequestCompile(true);System->WaitForCompilationComplete(true,false);
    System->MarkPackageDirty();
    return Renderers>0 && SpawnModules>0 && System->IsValid();
#else
    return false;
#endif
}

int32 USceneEffectLibrary::GetLiveWaterfallParticleCount(UNiagaraComponent* Component)
{
    if (!IsInGameThread() || !Component) return 0;
    auto Controller=Component->GetSystemInstanceController();
    if (!Controller.IsValid()) return 0;
    Controller->WaitForConcurrentTickAndFinalize();
    auto* Instance=Controller->GetSystemInstance_Unsafe();
    if (!Instance) return 0;
    int32 Count=0;
    for (auto& Emitter:Instance->GetEmitters()) Count+=Emitter->GetNumParticles();
    return Count;
}

