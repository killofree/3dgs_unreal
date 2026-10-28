#pragma once
#include "CoreMinimal.h"
class UWorld;
namespace GSAutomaticWorldScope
{
    // Authored weather actors are deliberately excluded from this predicate.
    LCCCINEMATICGS_API bool HasGS(const UWorld* World);
}
