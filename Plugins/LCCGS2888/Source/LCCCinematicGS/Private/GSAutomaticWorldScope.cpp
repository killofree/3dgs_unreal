#include "GSAutomaticWorldScope.h"
#include "GaussianSplatComponent.h"
#include "GaussianSplatAsset.h"
#include "Engine/World.h"
#include "UObject/UObjectIterator.h"
bool GSAutomaticWorldScope::HasGS(const UWorld* World)
{
    if (!IsValid(World)) return false;
    for (TObjectIterator<UGaussianSplatComponent> It; It; ++It)
    {
        const auto* C = *It;
        const auto* Asset = C->GetSplatAsset();
        if (!C->IsTemplate() && C->GetWorld() == World && C->IsRegistered() &&
            IsValid(Asset) && Asset->GetSplatCount() > 0) return true;
    }
    return false;
}
