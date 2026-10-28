// Copyright Epic Games, Inc. All Rights Reserved.
// P09: explicit full-ground diagnostic / P07 puddle Gaussian layer.
// Deliberately DOES NOT edit original GS, snow, shadow, original wet mesh, or
// camera depth. Large source splat clipping and water optics are later phases.
#include "GaussianSplatActor.h"
#include "GaussianSplatComponent.h"
#include "GaussianSplatAsset.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"

namespace LCCPuddleStage2
{
    struct FGroundCell
    {
        float Height = 0.f;
    };

    // World-space XY rasterization, independent of any FSceneView or camera.
    static bool HeightAt(const FVector& A, const FVector& B, const FVector& C,
        double X, double Y, double& OutZ)
    {
        const double Den = (B.Y - C.Y) * (A.X - C.X) +
            (C.X - B.X) * (A.Y - C.Y);
        if (FMath::Abs(Den) < 1.e-9) return false;
        const double U = ((B.Y - C.Y) * (X - C.X) + (C.X - B.X) * (Y - C.Y)) / Den;
        const double V = ((C.Y - A.Y) * (X - C.X) + (A.X - C.X) * (Y - C.Y)) / Den;
        const double W = 1.0 - U - V;
        if (U < -1.e-5 || V < -1.e-5 || W < -1.e-5) return false;
        OutZ = U*A.Z + V*B.Z + W*C.Z;
        return FMath::IsFinite(OutZ);
    }

    static float Smooth(float A, float B, float X)
    {
        const float V = FMath::Clamp((X - A) / FMath::Max(B - A, 1.e-5f), 0.f, 1.f);
        return V*V*(3.f - 2.f*V);
    }
}

bool AGaussianSplatActor::HasCurrentPuddleDampen(const UGaussianSplatComponent* Source) const
{
    if (!Source || Source != GaussianSplatComponent || !Source->GetSplatAsset()) return false;
    const int32 Originals = Source->GetSplatAsset()->GetOriginalSplatCount();
    return PuddleDampenSourceAsset == Source->GetSplatAsset() &&
        PuddleDampenSourceTransform.Equals(Source->GetComponentTransform(), 0.001) &&
        PuddleSourceDampenPackedBits.Num() == (Originals + 7) / 8 && Originals > 0;
}

void AGaussianSplatActor::DeletePuddleGaussians()
{
    if (!PuddleGaussianComponent) return;
#if WITH_EDITOR
    Modify();
    PuddleGaussianComponent->Modify();
#endif
    PuddleGaussianComponent->SetVisibility(false, false);
    PuddleGaussianComponent->SetHiddenInGame(true);
    PuddleGaussianComponent->SetSplatAsset(nullptr);
    BakedPuddleGaussianAsset = nullptr;
    PuddleSourceDampenPackedBits.Reset();
    PuddleDampenSourceAsset = nullptr;
    if (GaussianSplatComponent) GaussianSplatComponent->RefreshPuddleDampenRenderState();
    bShowPuddleGaussianTest = false;
    PuddleGaussianBuildStatus = TEXT("비 물 Gaussian 제거됨. 원본 GS 유지");
    SyncWeatherEffects();
#if WITH_EDITOR
    MarkPackageDirty();
#endif
}

void AGaussianSplatActor::GeneratePuddleGaussians()
{
    using namespace LCCPuddleStage2;
    if (!GaussianSplatComponent || !PuddleGaussianComponent) return;
    // Rain production pipeline: retained property names are needed for old level data,
    // but previous partial-basin / opaque-blue test paths are no longer reachable.
    bPuddleGaussianDebugFullGround = true;
    bPuddleGaussianIsolateOldWet = true;
    bPuddleIncludeUpperSurfaces = true;
    bPuddleSourceFittedRoof = true;
    bPuddleSourceFoliage = true;
    bPuddleDebugOpaqueBake = false;
    bPuddleDebugHideSource = false;
    bPuddleRoofOpaqueDebugView = false;
    PuddleRoofBlueDebugMode = 0;
    PuddleSourceCoverageDebug = 0;
    UStaticMesh* Proxy = GaussianSplatComponent->GetProxyShadowMesh();
    const FStaticMeshRenderData* Data = Proxy ? Proxy->GetRenderData() : nullptr;
    if (!GaussianSplatComponent->GetSplatAsset() || !Data || Data->LODResources.IsEmpty())
    {
        DeletePuddleGaussians();

        PuddleGaussianBuildStatus = TEXT("[비 Gaussian] 원본 GS 또는 프록시 메쉬가 없습니다");
        return;
    }

    const FStaticMeshLODResources& LOD = Data->LODResources[0];
    const uint32 VertexCount = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
    const uint32 IndexCount = LOD.IndexBuffer.GetNumIndices();
    if (VertexCount < 3 || IndexCount < 3 || IndexCount / 3 > 200000 ||
        !LOD.VertexBuffers.PositionVertexBuffer.GetVertexData())
    {
        DeletePuddleGaussians();

        PuddleGaussianBuildStatus = TEXT("[P07] 실패: 프록시 CPU 버퍼가 없거나 삼각형이 20만 개 초과");
        return;
    }

    const FTransform ProxyToWorld = GaussianSplatComponent->GetProxyShadowMeshWorldTransform();
    const FTransform GSWorld = GaussianSplatComponent->GetComponentTransform();
    const double Step = FMath::Clamp(double(PuddleGaussianSpacingCm), 15.0, 100.0);
    // P16: finer roof raster; never change the P09 floor grid.
    const double RoofStep = FMath::Clamp(double(PuddleRoofMicroSpacingCm), 12.0, 25.0);
    // Finer roof raster needs more splats (old 40cm roof roughly 8700 -> 16cm roughly 54k).
    // Baked only when full-ground + roofs are requested; budget remains bounded.
    const bool bFullRoofPass = bPuddleGaussianDebugFullGround && bPuddleIncludeUpperSurfaces;
    const int32 UserMaxCount = FMath::Clamp(PuddleGaussianMaxCount, 1000, 100000);
    const int32 MaxCount = bFullRoofPass ? FMath::Max(UserMaxCount, 115000) : UserMaxCount;
    const int32 MaxCells = FMath::Min(MaxCount * 5, 300000);
    const double UpThreshold = FMath::Cos(FMath::DegreesToRadians(24.0));

    struct FTri
    {
        FVector A, B, C;
    };
    TArray<FTri> GroundTriangles;
    GroundTriangles.Reserve(IndexCount/3);
    struct FRoofTri
    {
        FVector A, B, C, N;
    };
    TArray<FRoofTri> RoofTriangles;
    const bool bGenerateRoofs = bPuddleGaussianDebugFullGround && bPuddleIncludeUpperSurfaces;
    const double RoofMinUp = FMath::Cos(FMath::DegreesToRadians(
        FMath::Clamp(double(PuddleRoofMaxSlopeDegrees), 25.0, 75.0)));
    double MinFloorZ = TNumericLimits<double>::Max();
    int32 RejectedSlope = 0;
    for (uint32 I = 0; I + 2 < IndexCount; I += 3)
    {
        const uint32 IA = LOD.IndexBuffer.GetIndex(I);
        const uint32 IB = LOD.IndexBuffer.GetIndex(I + 1);
        const uint32 IC = LOD.IndexBuffer.GetIndex(I + 2);
        if (IA >= VertexCount || IB >= VertexCount || IC >= VertexCount) continue;
        const FVector A = ProxyToWorld.TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(IA)));
        const FVector B = ProxyToWorld.TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(IB)));
        const FVector C = ProxyToWorld.TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(IC)));
        FVector N = FVector::CrossProduct(B-A, C-A).GetSafeNormal();
        if (N.IsNearlyZero()) continue;
        // Match proxy authored orientation, including inverted winding.
        const FVector AuthoredN = ProxyToWorld.TransformVectorNoScale(
            FVector(LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(IA)));
        if (FVector::DotProduct(N, AuthoredN) < 0.0) N *= -1.0;
        // The original floor pass stays restricted to gently upward-facing faces.
        // Roof candidates are collected INDEPENDENTLY here; they may share XY with the courtyard.
        // Do not flip downward authored normals upward: eaves/undersides are not rain receivers.
        if (bGenerateRoofs && N.Z >= RoofMinUp)
        {
            RoofTriangles.Add({A, B, C, N});
        }
        if (N.Z < UpThreshold) { ++RejectedSlope; continue; }
        if (FMath::Min3(A.Z,B.Z,C.Z) < MinFloorZ) MinFloorZ = FMath::Min3(A.Z,B.Z,C.Z);
        GroundTriangles.Add({A,B,C});
    }
    if (GroundTriangles.IsEmpty() || !FMath::IsFinite(MinFloorZ))
    {
        DeletePuddleGaussians();

        PuddleGaussianBuildStatus = TEXT("[P07] 실패: 완만한 바닥 삼각형 없음");
        return;
    }

    TMap<FIntPoint, FGroundCell> Cells;
    Cells.Reserve(FMath::Min(MaxCells, 30000));
    const double CeilingZ = MinFloorZ + FMath::Clamp(double(PuddleGroundMaxHeightCm), 30.0, 600.0);
    // The floor is ALWAYS a floor-only raster, whether or not a roof is requested.
    // The roof uses its own maximum-height raster below.
    int64 Candidates = 0;
    int32 SkippedHuge = 0;
    for (const FTri& Tri : GroundTriangles)
    {
        const double SmallZ = FMath::Min3(Tri.A.Z,Tri.B.Z,Tri.C.Z);
        if (SmallZ > CeilingZ) continue; // retain the successful P09 ground coverage unchanged
        const double MinX = FMath::Min3(Tri.A.X,Tri.B.X,Tri.C.X);
        const double MaxX = FMath::Max3(Tri.A.X,Tri.B.X,Tri.C.X);
        const double MinY = FMath::Min3(Tri.A.Y,Tri.B.Y,Tri.C.Y);
        const double MaxY = FMath::Max3(Tri.A.Y,Tri.B.Y,Tri.C.Y);
        const int32 X0 = FMath::FloorToInt(MinX / Step), X1 = FMath::FloorToInt(MaxX / Step);
        const int32 Y0 = FMath::FloorToInt(MinY / Step), Y1 = FMath::FloorToInt(MaxY / Step);
        const int64 Covered = int64(X1-X0+1) * int64(Y1-Y0+1);
        if (Covered > 150000 || Covered <= 0) { ++SkippedHuge; continue; }
        for (int32 Y = Y0; Y <= Y1; ++Y)
        for (int32 X = X0; X <= X1; ++X)
        {
            const double WX = (double(X) + 0.5) * Step;
            const double WY = (double(Y) + 0.5) * Step;
            double WZ = 0;
            if (!HeightAt(Tri.A,Tri.B,Tri.C,WX,WY,WZ) || WZ > CeilingZ) continue;
            ++Candidates;
            const FIntPoint Key(X,Y);
            FGroundCell* Existing = Cells.Find(Key);
            if (Existing)
            {
                Existing->Height = FMath::Min(Existing->Height, float(WZ));
            }
            else if (Cells.Num() < MaxCells)
            {
                Cells.Add(Key, FGroundCell{float(WZ)});
            }
        }
    }
    // P12: upper rain-exposed faces use a distinct height/normal layer.
    // The highest visible roof sheet is retained at each XY; courtyard floor remains untouched.
    struct FRoofCell
    {
        float Z = 0.f;
        FVector Normal = FVector::UpVector;
    };
    TMap<FIntPoint, FRoofCell> RoofCells;
    // P13: the full proxy triangle footprints for each roof raster cell.
    // These are bake-time CPU data only, never sampled from the camera or kept at runtime.
    TMap<FIntPoint, TArray<int32>> RoofCellTriangles;
    int32 RoofRejectedHeight = 0;
    if (bGenerateRoofs)
    {
        RoofCells.Reserve(FMath::Min(MaxCells, 14000));
        RoofCellTriangles.Reserve(FMath::Min(MaxCells, 14000));
        // This separates elevated roofs from the lowest courtyard floor. Adjustable
        // via the preexisting ground height band; a dedicated mesh category is safer
        // for scenes with elevated terrain.
        const double RoofThresholdZ = CeilingZ;
        for (int32 RoofTriIndex = 0; RoofTriIndex < RoofTriangles.Num(); ++RoofTriIndex)
        {
            const FRoofTri& Tri = RoofTriangles[RoofTriIndex];
            if (FMath::Max3(Tri.A.Z, Tri.B.Z, Tri.C.Z) <= RoofThresholdZ)
            {
                ++RoofRejectedHeight;
                continue;
            }
            const int32 X0 = FMath::FloorToInt(FMath::Min3(Tri.A.X,Tri.B.X,Tri.C.X) / RoofStep);
            const int32 X1 = FMath::FloorToInt(FMath::Max3(Tri.A.X,Tri.B.X,Tri.C.X) / RoofStep);
            const int32 Y0 = FMath::FloorToInt(FMath::Min3(Tri.A.Y,Tri.B.Y,Tri.C.Y) / RoofStep);
            const int32 Y1 = FMath::FloorToInt(FMath::Max3(Tri.A.Y,Tri.B.Y,Tri.C.Y) / RoofStep);
            const int64 Covered = int64(X1-X0+1) * int64(Y1-Y0+1);
            if (Covered <= 0 || Covered > 150000) continue;
            for (int32 Y = Y0; Y <= Y1; ++Y)
            for (int32 X = X0; X <= X1; ++X)
            {
                const FIntPoint Key(X,Y);
                // Keep all relevant triangles, including those crossing only a
                // *part* of the cell. A center-only triangle test cannot describe
                // the outline of a slanted roof/eave at sub-cell resolution.
                if (RoofCellTriangles.Num() < MaxCells * 2 || RoofCellTriangles.Contains(Key))
                {
                    RoofCellTriangles.FindOrAdd(Key).Add(RoofTriIndex);
                }
                double Z;
                const double WX = (double(X) + 0.5) * RoofStep;
                const double WY = (double(Y) + 0.5) * RoofStep;
                if (!HeightAt(Tri.A,Tri.B,Tri.C,WX,WY,Z) || Z <= RoofThresholdZ) continue;
                FRoofCell* Existing = RoofCells.Find(Key);
                if (Existing)
                {
                    if (Z > Existing->Z) { Existing->Z = float(Z); Existing->Normal = Tri.N; }
                }
                else if (RoofCells.Num() < MaxCells)
                {
                    RoofCells.Add(Key, FRoofCell{float(Z), Tri.N});
                }
            }
        }
    }
    if (Cells.IsEmpty())
    {
        DeletePuddleGaussians();

        PuddleGaussianBuildStatus = TEXT("[P07] 실패: 래스터화된 지면 없음 (바닥 높이 제한/프록시 확인)");
        return;
    }

    // P07: water is not a coating of the whole proxy heightfield. Find low local
    // basins on a static world XY raster, flood-fill limited connected patches,
    // then assign ONE horizontal water Z per patch. No FSceneView / camera depth.
    const double Pattern = FMath::Clamp(double(PuddleGaussianPatternCm), 100.0, 1200.0);
    const double BaseRadius = FMath::Clamp(Pattern * 0.65, 90.0, 700.0);
    const double WaterDepth = FMath::Clamp(double(PuddleGaussianWaterDepthCm), 2.0, 35.0);
    const double Offset = FMath::Clamp(double(PuddleGaussianLiftCm), 0.5, 10.0);
    // P17: roof and ground now use separate offsets. Roof lift is vertical
    // to preserve baked 4x4 XY coverage alignment at eaves and curved-tile silhouettes.
    const double RoofLiftZ = FMath::Clamp(double(PuddleRoofLiftCm), 0.0, 20.0);
    const int32 MaxPatches = FMath::Clamp(PuddleGaussianPatchCount, 1, 32);
    const int32 MinPatchCells = 6;
    constexpr float SH_C0 = 0.28209479177387814f;
    const FLinearColor DiagnosticCyan(0.035f, 0.27f, 0.44f, 1.f);
    TArray<FIntPoint> Keys;
    Cells.GenerateKeyArray(Keys);
    Keys.Sort([](const FIntPoint& A, const FIntPoint& B)
    {
        return A.Y == B.Y ? A.X < B.X : A.Y < B.Y;
    });

    struct FSeed
    {
        FIntPoint Key;
        double Z = 0.0;
        double Score = 0.0;
    };
    TArray<FSeed> SeedCandidates;
    SeedCandidates.Reserve(Keys.Num() / 8 + 1);
    // The neighboring ring is purely world-space geometry. Reject isolated
    // raster islands and preferentially select low, enclosed areas.
    for (const FIntPoint& Key : Keys)
    {
        const FGroundCell* Center = Cells.Find(Key);
        if (!Center) continue;
        double RimSum = 0.0;
        int32 RimCount = 0;
        int32 HigherCount = 0;
        const double CenterZ = double(Center->Height);
        for (int32 DY = -3; DY <= 3; ++DY)
        for (int32 DX = -3; DX <= 3; ++DX)
        {
            if (FMath::Max(FMath::Abs(DX), FMath::Abs(DY)) != 3) continue;
            const FGroundCell* Neighbor = Cells.Find(FIntPoint(Key.X + DX, Key.Y + DY));
            if (!Neighbor) continue;
            RimSum += Neighbor->Height;
            ++RimCount;
            if (double(Neighbor->Height) > CenterZ + 0.8) ++HigherCount;
        }
        if (RimCount < 8) continue;
        const double Relief = RimSum / double(RimCount) - CenterZ;
        if (Relief < 1.0 || HigherCount < 5) continue;
        const double WX = (double(Key.X) + 0.5) * Step;
        const double WY = (double(Key.Y) + 0.5) * Step;
        const double Noise = double(FMath::PerlinNoise2D(FVector2D(WX/Pattern + 3.7, WY/Pattern - 5.9)));
        FSeed S;
        S.Key = Key;
        S.Z = CenterZ;
        S.Score = FMath::Min(Relief, 65.0) + 2.0 * Noise - 0.006 * FMath::Max(0.0, CenterZ - MinFloorZ);
        SeedCandidates.Add(S);
    }
    bool bUsedFlatFallback = false;
    // A flat surveyed courtyard can have no real 1cm basin. In that case choose
    // a LIMITED number of low-world-height seeds, never cover all cells by default.
    if (SeedCandidates.IsEmpty())
    {
        bUsedFlatFallback = true;
        for (const FIntPoint& Key : Keys)
        {
            const FGroundCell* Ground = Cells.Find(Key);
            if (!Ground) continue;
            const double WX = (double(Key.X) + 0.5) * Step;
            const double WY = (double(Key.Y) + 0.5) * Step;
            FSeed S;
            S.Key = Key;
            S.Z = Ground->Height;
            S.Score = 6.0 * double(FMath::PerlinNoise2D(FVector2D(WX/Pattern + 13.1, WY/Pattern - 5.2)))
                - 0.025 * FMath::Max(0.0, S.Z - MinFloorZ);
            SeedCandidates.Add(S);
        }
    }
    SeedCandidates.Sort([](const FSeed& A, const FSeed& B)
    {
        if (A.Score != B.Score) return A.Score > B.Score;
        return A.Key.Y == B.Key.Y ? A.Key.X < B.Key.X : A.Key.Y < B.Key.Y;
    });

    struct FWaterCell
    {
        float Z = 0.f;
        float Alpha = 0.f;
    };
    struct FSelectedSeed
    {
        FIntPoint Key;
        double Radius = 0.0;
    };
    TArray<FSelectedSeed> SelectedSeeds;
    TMap<FIntPoint, FWaterCell> WaterCells;
    WaterCells.Reserve(FMath::Min(MaxCount, Cells.Num()));
    int32 DroppedSmallPatches = 0;
    if (!bPuddleGaussianDebugFullGround)
    {
        for (const FSeed& S : SeedCandidates)
        {
            if (SelectedSeeds.Num() >= MaxPatches || WaterCells.Num() >= MaxCount) break;
            const double SX = (double(S.Key.X) + 0.5) * Step;
            const double SY = (double(S.Key.Y) + 0.5) * Step;
            const double Shape = double(FMath::PerlinNoise2D(FVector2D(SX / (Pattern * 0.83) + 2.1,
                SY / (Pattern * 0.83) + 7.3)));
            const double Radius = BaseRadius * FMath::Clamp(0.84 + Shape * 0.32, 0.65, 1.15);
            bool bTooClose = false;
            for (const FSelectedSeed& Prev : SelectedSeeds)
            {
                const double PX = (double(Prev.Key.X) + 0.5) * Step;
                const double PY = (double(Prev.Key.Y) + 0.5) * Step;
                if (FVector2D::Distance(FVector2D(SX,SY), FVector2D(PX,PY)) < Radius + Prev.Radius + Step)
                {
                    bTooClose = true;
                    break;
                }
            }
            if (bTooClose) continue;
            const double WaterZ = S.Z + WaterDepth;
            TArray<FIntPoint> Queue;
            TSet<FIntPoint> Seen;
            TMap<FIntPoint, FWaterCell> Patch;
            Queue.Add(S.Key);
            Seen.Add(S.Key);
            int32 Read = 0;
            constexpr int32 NeighborDX[4] = { 1, -1, 0, 0 };
            constexpr int32 NeighborDY[4] = { 0, 0, 1, -1 };
            // Water cannot propagate through a dry/high or missing cell. A single
            // flat height per connected pond eliminates the old wavy mesh appearance.
            while (Read < Queue.Num() && Patch.Num() < MaxCount && Queue.Num() < 100000)
            {
                const FIntPoint Key = Queue[Read++];
                const FGroundCell* Ground = Cells.Find(Key);
                if (!Ground || WaterCells.Contains(Key)) continue;
                const double WX = (double(Key.X) + 0.5) * Step;
                const double WY = (double(Key.Y) + 0.5) * Step;
                const double DX = WX - SX, DY = WY - SY;
                const double Dist = FMath::Sqrt(DX*DX + DY*DY);
                // Stable irregular outline, with world noise ONLY perturbing the
                // radius rather than arbitrarily admitting dry high cells.
                const float EdgeNoise = FMath::PerlinNoise2D(FVector2D(WX / (Pattern*0.40) + 4.2,
                    WY / (Pattern*0.40) - 13.3));
                const double LocalRadius = Radius * FMath::Clamp(1.0 + double(EdgeNoise) * 0.12, 0.88, 1.12);
                if (Dist > LocalRadius || double(Ground->Height) >= WaterZ - 0.5) continue;
                const float EdgeFade = 1.0f - Smooth(0.72f, 1.0f, float(Dist / LocalRadius));
                const float DepthFade = Smooth(0.5f, float(FMath::Max(3.0, WaterDepth*0.75)),
                    float(WaterZ - double(Ground->Height)));
                const float Alpha = FMath::Clamp(EdgeFade * DepthFade, 0.0f, 1.0f);
                Patch.Add(Key, FWaterCell{float(WaterZ + Offset), Alpha});
                for (int32 Dir = 0; Dir < 4; ++Dir)
                {
                    const FIntPoint Next(Key.X + NeighborDX[Dir], Key.Y + NeighborDY[Dir]);
                    if (!Seen.Contains(Next))
                    {
                        Seen.Add(Next);
                        Queue.Add(Next);
                    }
                }
            }
            if (Patch.Num() < MinPatchCells)
            {
                ++DroppedSmallPatches;
                continue;
            }
            for (const auto& Pair : Patch)
            {
                if (WaterCells.Num() >= MaxCount) break;
                if (Pair.Value.Alpha >= 0.06f) WaterCells.Add(Pair.Key, Pair.Value);
            }
            SelectedSeeds.Add(FSelectedSeed{S.Key, Radius});
        }
    }
    else
    {
        // P09 FULL-GROUND DIAGNOSTIC. Every eligible world-XY floor cell gets
        // a broad, flat Gaussian centered on the proxy floor at that XY.
        // Neither camera depth nor puddle seed/low-basin tests affect placement.
        // This is a ground-coverage check, NOT a realistic standing-water surface.
        // 1.0 alpha and overlapping Gaussian footprints make gaps visible as
        // missing geometry rather than as opacity noise.
        for (const FIntPoint& Key : Keys)
        {
            if (WaterCells.Num() >= MaxCount) break;
            const FGroundCell* Ground = Cells.Find(Key);
            if (Ground) WaterCells.Add(Key, FWaterCell{Ground->Height + float(Offset), 1.0f});
        }
    }

    TArray<FIntPoint> WetKeys;
    WaterCells.GenerateKeyArray(WetKeys);
    WetKeys.Sort([](const FIntPoint& A, const FIntPoint& B)
    {
        return A.Y == B.Y ? A.X < B.X : A.Y < B.Y;
    });
    // P08: the same world-space WaterCells decide both source inspection and
    // conservative broad-GS membership. No view-dependent depth is sampled.
    UGaussianSplatAsset* const SourceAsset = GaussianSplatComponent->GetSplatAsset();
    const int32 OriginalCount = SourceAsset->GetOriginalSplatCount();
    TArray<uint32> NewDampenBits;
    // P19: one shared buffer holds 4-bit flags per ORIGINAL GS, including
    // ground (bit0), roof (bit1), foliage (bit2). It is required in full-ground
    // mode too, because the source must be coloured independently of extra GS.
    // The original ground damping remains DISABLED in full-ground mode.
    if (OriginalCount > 0) NewDampenBits.SetNumZeroed((OriginalCount + 7) / 8);
    // A small pre-expanded sparse cell set avoids 25 hash probes on every
    // source splat in very large 3DGS assets (often >10M points).
    TSet<FIntPoint> NearWaterCells;
    NearWaterCells.Reserve(WaterCells.Num() * 25);
    for (const auto& Pair : WaterCells)
    {
        if (Pair.Value.Alpha < 0.18f) continue;
        for (int32 DY = -2; DY <= 2; ++DY)
        for (int32 DX = -2; DX <= 2; ++DX)
            NearWaterCells.Add(FIntPoint(Pair.Key.X + DX, Pair.Key.Y + DY));
    }
    int64 NearbySourceCount = 0;
    int64 SourceHeightSumCount = 0;
    double SourceHeightSum = 0.0;
    double SourceHeightMax = -1.0e20;
    int32 BroadCandidateCount = 0;
    bool bSourceCovarianceReadable = true;
    if (!bPuddleGaussianDebugFullGround)
    {
        constexpr int32 ChunkSize = 65536;
        TArray<FVector3f> SourcePositions;
        TArray<FQuat4f> SourceRotations;
        TArray<FVector3f> SourceScales;
        const double MaxWorldAxis = GSWorld.GetScale3D().GetAbsMax();
        for (int32 Base = 0; Base < OriginalCount; Base += ChunkSize)
        {
            const int32 Count = FMath::Min(ChunkSize, OriginalCount - Base);
            if (!SourceAsset->GetDecompressedPositionRange(Base, Count, SourcePositions))
            {
                bSourceCovarianceReadable = false;
                NewDampenBits.Reset();
                break; // diagnose unavailable source, do not remove any original GS
            }
            const bool bHasCov = SourceAsset->GetSnowSourceCovarianceRange(Base, Count,
                SourceRotations, SourceScales);
            if (!bHasCov) bSourceCovarianceReadable = false;
            for (int32 J = 0; J < SourcePositions.Num(); ++J)
            {
                if (SourcePositions[J].ContainsNaN()) continue;
                const FVector WorldP = GSWorld.TransformPosition(FVector(SourcePositions[J]));
                if (WorldP.ContainsNaN()) continue;
                const FIntPoint WorldCell(FMath::FloorToInt(WorldP.X / Step), FMath::FloorToInt(WorldP.Y / Step));
                if (!NearWaterCells.Contains(WorldCell)) continue;
                // Closest wetted footprint, also considering a broad center
                // just outside the water cell. Clamp to the actual baked patch.
                const FWaterCell* Match = nullptr;
                double MatchDistSq = TNumericLimits<double>::Max();
                for (int32 DY = -2; DY <= 2; ++DY)
                for (int32 DX = -2; DX <= 2; ++DX)
                {
                    const FIntPoint N(WorldCell.X + DX, WorldCell.Y + DY);
                    const FWaterCell* Candidate = WaterCells.Find(N);
                    if (!Candidate || Candidate->Alpha < 0.18f) continue;
                    const double CX = (double(N.X) + 0.5) * Step;
                    const double CY = (double(N.Y) + 0.5) * Step;
                    const double DistSq = FMath::Square(WorldP.X - CX) + FMath::Square(WorldP.Y - CY);
                    if (DistSq < MatchDistSq) { MatchDistSq = DistSq; Match = Candidate; }
                }
                if (!Match) continue;
                const double DeltaZ = WorldP.Z - double(Match->Z);
                if (FMath::Abs(DeltaZ) <= 200.0)
                {
                    ++NearbySourceCount;
                    if (FMath::Abs(DeltaZ) < 100.0)
                    {
                        ++SourceHeightSumCount;
                        SourceHeightSum += DeltaZ;
                        SourceHeightMax = FMath::Max(SourceHeightMax, DeltaZ);
                    }
                }
                if (!bHasCov || J >= SourceScales.Num() || J >= SourceRotations.Num()) continue;
                const FVector3f S = SourceScales[J];
                const FQuat4f R = SourceRotations[J];
                if (S.ContainsNaN() || R.ContainsNaN()) continue;
                const double SX = FMath::Abs(double(S.X));
                const double SY = FMath::Abs(double(S.Y));
                const double SZ = FMath::Abs(double(S.Z));
                const double SMin = FMath::Min3(SX, SY, SZ);
                const double SMax = FMath::Max3(SX, SY, SZ);
                const double MajorWorld = SMax * MaxWorldAxis;
                if (MajorWorld < 28.0 || MajorWorld > 260.0 || SMin / FMath::Max(SMax, 0.001) > 0.48) continue;
                const FVector Axis = (SX <= SY && SX <= SZ) ? FVector::ForwardVector :
                    (SY <= SZ ? FVector::RightVector : FVector::UpVector);
                const FQuat Q(double(R.X), double(R.Y), double(R.Z), double(R.W));
                if (Q.ContainsNaN() || Q.SizeSquared() < 0.001) continue;
                const FVector ThinAxis = GSWorld.TransformVectorNoScale(Q.GetNormalized().RotateVector(Axis)).GetSafeNormal();
                if (FMath::Abs(ThinAxis.Z) < 0.83) continue; // actual broad horizontal floor only
                if (DeltaZ < -35.0 || DeltaZ > FMath::Clamp(SMin * MaxWorldAxis * 2.0, 25.0, 65.0)) continue;
                if (MatchDistSq > FMath::Square(MajorWorld * 1.35 + Step * 0.5)) continue;
                const int32 OriginalIndex = Base + J;
                NewDampenBits[OriginalIndex >> 3] |= (uint32(1) << ((OriginalIndex & 7) * 4));
                ++BroadCandidateCount;
            }
        }
    }
    // Fail closed on unreadable covariance. Water itself remains functional.
    if (!bSourceCovarianceReadable) { NewDampenBits.Reset(); BroadCandidateCount = 0; }
    const double MeanDeltaCm = SourceHeightSumCount > 0 ? SourceHeightSum / double(SourceHeightSumCount) : 0.0;
    const double MaxDeltaCm = SourceHeightSumCount > 0 ? SourceHeightMax : 0.0;

    // P18: snow-like ORIGINAL GS source evidence. Separate 3D voxels preserve
    // roof height/curved tile ridges and keep vegetation above a roof distinct.
    // This is baked once; none of these reads or spatial fits run per frame.
    struct FSourceWetCell
    {
        FVector PositionSum = FVector::ZeroVector;
        FVector NormalSum = FVector::ZeroVector;
        float Weight = 0.f;
        int32 Count = 0;
    };
    constexpr double SourceCellCm = 12.0;
    constexpr double LeafCellCm = 18.0;
    const auto Key3D = [](const FVector& P, double Cm) -> FIntVector
    {
        return FIntVector(FMath::FloorToInt(P.X/Cm), FMath::FloorToInt(P.Y/Cm), FMath::FloorToInt(P.Z/Cm));
    };
    TMap<FIntVector, FSourceWetCell> RoofSourceGrid;
    TMap<FIntVector, FSourceWetCell> LeafSourceGrid;
    int32 SourceSurfaceVotes = 0, PlantColorVotes = 0;
    int32 OriginalRoofWetCandidates = 0, OriginalFoliageWetCandidates = 0;
    bool bP18CovarianceReady = false, bP18ColorsReady = false;
    if (bGenerateRoofs && (bPuddleSourceFittedRoof || bPuddleSourceFoliage))
    {
        RoofSourceGrid.Reserve(150000);
        LeafSourceGrid.Reserve(20000);
        constexpr int32 Chunk = 65536;
        TArray<FVector3f> SourcePositions;
        TArray<FQuat4f> SourceRotations;
        TArray<FVector3f> SourceScales;
        TArray<FLinearColor> SourceColors;
        const double MaxGSScale = GSWorld.GetScale3D().GetAbsMax();
        for (int32 Base = 0; Base < OriginalCount; Base += Chunk)
        {
            const int32 Count = FMath::Min(Chunk, OriginalCount - Base);
            if (!SourceAsset->GetDecompressedPositionRange(Base, Count, SourcePositions))
            {
                bP18CovarianceReady = false;
                RoofSourceGrid.Reset(); LeafSourceGrid.Reset(); break;
            }
            const bool bHasCov = SourceAsset->GetSnowSourceCovarianceRange(Base, Count, SourceRotations, SourceScales);
            bP18CovarianceReady = bHasCov;
            if (!bHasCov) { RoofSourceGrid.Reset(); LeafSourceGrid.Reset(); break; }
            const bool bHasColor = bPuddleSourceFoliage &&
                SourceAsset->GetPuddleSourceColorRange(Base, Count, SourceColors);
            bP18ColorsReady = bHasColor;
            for (int32 J = 0; J < SourcePositions.Num(); ++J)
            {
                if (SourcePositions[J].ContainsNaN() || SourceScales[J].ContainsNaN() ||
                    SourceRotations[J].ContainsNaN()) continue;
                const FVector P = GSWorld.TransformPosition(FVector(SourcePositions[J]));
                if (P.ContainsNaN() || P.Z < CeilingZ - 90.0) continue;
                const FVector3f Sc = SourceScales[J];
                const double SX = FMath::Abs(double(Sc.X)), SY = FMath::Abs(double(Sc.Y)), SZ = FMath::Abs(double(Sc.Z));
                const double MinScale = FMath::Min3(SX,SY,SZ), MaxScale = FMath::Max3(SX,SY,SZ);
                if (MinScale < 0.001 || MaxScale * MaxGSScale > 200.0 || MaxScale <= 0.0) continue;
                const FQuat R(double(SourceRotations[J].X), double(SourceRotations[J].Y),
                    double(SourceRotations[J].Z), double(SourceRotations[J].W));
                const FVector LocalAxis = SX <= SY && SX <= SZ ? FVector::ForwardVector :
                    (SY <= SZ ? FVector::RightVector : FVector::UpVector);
                FVector N = GSWorld.GetRotation().RotateVector(R.GetNormalized().RotateVector(LocalAxis));
                const FVector ScaleW = GSWorld.GetScale3D();
                N = FVector(N.X / FMath::Max(FMath::Abs(ScaleW.X), 1.0e-4),
                            N.Y / FMath::Max(FMath::Abs(ScaleW.Y), 1.0e-4),
                            N.Z / FMath::Max(FMath::Abs(ScaleW.Z), 1.0e-4)).GetSafeNormal();
                if (N.Z < 0.0) N *= -1.0;
                const double Planarity = 1.0 - MinScale / MaxScale;
                // A roof point must have BOTH fitted upward covariance AND
                // a matching elevated proxy roof tile. Never make a roof from
                // a distant backdrop, floating LOD representative, or tree.
                const FIntPoint RoofKey(FMath::FloorToInt(P.X/RoofStep), FMath::FloorToInt(P.Y/RoofStep));
                const FRoofCell* Roof = RoofCells.Find(RoofKey);
                const bool bNearRoof = Roof && FMath::Abs(P.Z - double(Roof->Z)) < 65.0;
                // P19: source-first roof wetness is authored at ORIGINAL splat
                // granularity. Unlike P18's sparse fit grid, it covers the actual
                // curved tile splats even when no extra water GS could be generated.
                // A separate same-XY elevated roof layer is required; ground isn't painted.
                const bool bRoofSource = bPuddleSourceFittedRoof && bNearRoof &&
                    FMath::Abs(P.Z - double(Roof->Z)) <= 45.0 &&
                    N.Z >= FMath::Max(0.18, RoofMinUp * 0.68) &&
                    MaxScale * MaxGSScale <= 150.0;
                if (bRoofSource && NewDampenBits.IsValidIndex((Base + J) >> 3))
                {
                    const int32 OriginalIndex = Base + J;
                    NewDampenBits[OriginalIndex >> 3] |= (uint32(2) << ((OriginalIndex & 7) * 4));
                    ++OriginalRoofWetCandidates;
                }
                if (bPuddleSourceFittedRoof && bNearRoof && N.Z >= RoofMinUp && Planarity >= 0.33)
                {
                    if (RoofSourceGrid.Num() < 360000 || RoofSourceGrid.Contains(Key3D(P, SourceCellCm)))
                    {
                        FSourceWetCell& C = RoofSourceGrid.FindOrAdd(Key3D(P, SourceCellCm));
                        const float W = FMath::Clamp(float(Planarity), 0.33f, 0.95f);
                        C.PositionSum += P * double(W); C.NormalSum += N * double(W);
                        C.Weight += W; ++C.Count; ++SourceSurfaceVotes;
                    }
                }
                // Green/plant-like colour is a conservative foliage heuristic,
                // not a semantic tree segmentation. Clusters near a verified
                // roof are excluded so green moss does not become tree droplets.
                if (bPuddleSourceFoliage && bHasColor && !bNearRoof && P.Z > CeilingZ + 100.0 &&
                    J < SourceColors.Num())
                {
                    const FLinearColor& C = SourceColors[J];
                    const bool bGreen = C.G > 0.04f && C.G > C.R * 1.16f &&
                        C.G > C.B * 1.10f && C.G < 1.6f && C.A > 0.15f;
                    const FIntVector LeafKey = Key3D(P, LeafCellCm);
                    // Preserve the parent ORIGINAL green splat as the main
                    // wetted foliage base; added tiny droplets are secondary.
                    if (bGreen && MaxScale * MaxGSScale < 100.0 &&
                        NewDampenBits.IsValidIndex((Base + J) >> 3))
                    {
                        const int32 OriginalIndex = Base + J;
                        NewDampenBits[OriginalIndex >> 3] |= (uint32(4) << ((OriginalIndex & 7) * 4));
                        ++OriginalFoliageWetCandidates;
                    }
                    if (bGreen && MaxScale * MaxGSScale < 100.0 &&
                        (LeafSourceGrid.Num() < 120000 || LeafSourceGrid.Contains(LeafKey)))
                    {
                        FSourceWetCell& L = LeafSourceGrid.FindOrAdd(LeafKey);
                        L.PositionSum += P; L.NormalSum += N; L.Weight += 1.0f;
                        ++L.Count; ++PlantColorVotes;
                    }
                }
            }
        }
        for (auto& E : RoofSourceGrid)
        {
            FSourceWetCell& C = E.Value;
            if (C.Weight > 0.0f) { C.PositionSum /= double(C.Weight); C.NormalSum.Normalize(); }
        }
        for (auto& E : LeafSourceGrid)
        {
            FSourceWetCell& C = E.Value;
            if (C.Weight > 0.0f) { C.PositionSum /= double(C.Weight); C.NormalSum.Normalize(); }
        }
    }
    // Weighted local source-surface projection, analogous to snow's
    // multi-neighbour source surface fit. No old arbitrary +10/+14cm lift.
    const auto FitRoofSource = [&RoofSourceGrid, &Key3D, RoofMinUp, SourceCellCm](const FVector& ProxyP,
        const FVector& ProxyN, FVector& FittedPoint, FVector& FittedNormal) -> bool
    {
        const FIntVector Center = Key3D(ProxyP, SourceCellCm);
        double HeightSum = 0.0, WeightSum = 0.0;
        FVector NormalSum = FVector::ZeroVector;
        int32 Support = 0;
        for (int32 DZ = -5; DZ <= 5; ++DZ)
        for (int32 DY = -2; DY <= 2; ++DY)
        for (int32 DX = -2; DX <= 2; ++DX)
        {
            const FSourceWetCell* C = RoofSourceGrid.Find(Center + FIntVector(DX,DY,DZ));
            if (!C || C->Weight < 0.2f) continue;
            const FVector& P = C->PositionSum;
            const FVector N = C->NormalSum.GetSafeNormal();
            if (N.Z < RoofMinUp || FVector::DotProduct(N,ProxyN) < 0.50) continue;
            const double Dist2 = FMath::Square(P.X - ProxyP.X) + FMath::Square(P.Y - ProxyP.Y);
            if (Dist2 > FMath::Square(30.0) || FMath::Abs(P.Z - ProxyP.Z) > 60.0) continue;
            const double PlaneZ = P.Z - (N.X * (ProxyP.X - P.X) + N.Y * (ProxyP.Y - P.Y)) / N.Z;
            if (!FMath::IsFinite(PlaneZ) || FMath::Abs(PlaneZ-ProxyP.Z) > 60.0) continue;
            const double W = FMath::Exp(-Dist2 / (2.0 * 18.0 * 18.0)) *
                FMath::Min(4.0, double(C->Count)) * FMath::Clamp(FVector::DotProduct(N,ProxyN),0.0,1.0);
            HeightSum += PlaneZ*W; WeightSum += W; NormalSum += N*W; ++Support;
        }
        if (Support < 2 || WeightSum < 0.3) return false;
        FittedPoint = ProxyP; FittedPoint.Z = HeightSum/WeightSum + 1.2; // thin surface lift only
        FittedNormal = NormalSum.GetSafeNormal();
        return FittedNormal.Z >= RoofMinUp && FMath::IsFinite(FittedPoint.Z);
    };

    TArray<FGaussianSplatData> Splats;
    Splats.Reserve(FMath::Min(MaxCount, WetKeys.Num()));
    const FVector LocalUp = GSWorld.InverseTransformVectorNoScale(FVector::UpVector).GetSafeNormal();
    const FQuat Rot = FQuat::FindBetweenNormals(FVector::UpVector, LocalUp);
    const double XAxis = GSWorld.TransformVector(Rot.RotateVector(FVector::ForwardVector)).Size();
    const double YAxis = GSWorld.TransformVector(Rot.RotateVector(FVector::RightVector)).Size();
    const double ZAxis = GSWorld.TransformVector(Rot.RotateVector(FVector::UpVector)).Size();
    if (XAxis < 1.e-5 || YAxis < 1.e-5 || ZAxis < 1.e-5)
    {
        DeletePuddleGaussians();

        PuddleGaussianBuildStatus = TEXT("[비 Gaussian] GS 월드 변환 크기가 0입니다");
        return;
    }
    for (const FIntPoint& Key : WetKeys)
    {
        if (Splats.Num() >= MaxCount) break;
        const FWaterCell* Water = WaterCells.Find(Key);
        if (!Water) continue;
        const double X = (double(Key.X) + 0.5) * Step;
        const double Y = (double(Key.Y) + 0.5) * Step;
        const FVector LocalP = GSWorld.InverseTransformPosition(FVector(X, Y, Water->Z));
        if (LocalP.ContainsNaN()) continue;
        FGaussianSplatData G;
        G.Position = FVector3f(LocalP);
        G.Rotation = FQuat4f(float(Rot.X), float(Rot.Y), float(Rot.Z), float(Rot.W));
        // P09: overlap neighboring coverage samples. Original basin splat
        // scale remains unchanged when the full-ground switch is OFF.
        const double HorizontalSigma = Step * (bPuddleGaussianDebugFullGround ? 0.98 : 0.72);
        G.Scale = FVector3f(float(HorizontalSigma / XAxis),
                            float(HorizontalSigma / YAxis), float(0.9 / ZAxis));
        // A translucent water test layer, not an opaque blue paint operation.
        G.Opacity = (bPuddleGaussianDebugFullGround || bPuddleDebugOpaqueBake)
            ? FMath::Clamp(0.985f * FMath::Max(0.65f, Water->Alpha), 0.64f, 0.985f)
            : FMath::Clamp(0.48f * Water->Alpha, 0.015f, 0.48f);
        G.SH_DC = FVector3f((DiagnosticCyan.R - 0.5f) / SH_C0,
                            (DiagnosticCyan.G - 0.5f) / SH_C0,
                            (DiagnosticCyan.B - 0.5f) / SH_C0);
        Splats.Add(G);
    }
    const int32 FloorSplatCount = Splats.Num();
    int32 RoofSplatCount = 0;
    int32 P18RejectedUnsupportedRoof = 0;
    int32 P18LeafSplatCount = 0;
    int32 RoofPartialCells = 0;
    int32 RoofRejectedEmptyCoverage = 0;
    const float RoofAlphaScale = FMath::Clamp(PuddleRoofAlphaMultiplier, 0.10f, 1.0f);
    // Separate roof water splats follow the ORIGINAL roof tangent plane, not a world-horizontal sheet.
    // This fixes the P11 overlap bug while leaving every previously accepted floor splat untouched.
    if (bGenerateRoofs)
    {
        TArray<FIntPoint> RoofKeys;
        RoofCells.GenerateKeyArray(RoofKeys);
        RoofKeys.Sort([](const FIntPoint& A, const FIntPoint& B)
        {
            return A.Y == B.Y ? A.X < B.X : A.Y < B.Y;
        });
        const int32 RoofOutputLimit = bPuddleSourceFoliage ? FMath::Max(0, MaxCount - 14000) : MaxCount;
        for (const FIntPoint& Key : RoofKeys)
        {
            if (Splats.Num() >= RoofOutputLimit) break;
            const FRoofCell* Roof = RoofCells.Find(Key);
            if (!Roof) continue;
            const FVector N = Roof->Normal.GetSafeNormal();
            if (N.Z < RoofMinUp) continue;
            // P13: 4x4 world-XY coverage encoded in the water-only splat DC.
            // Each 10cm subcell at 40cm spacing is tested against *real* proxy
            // roof triangles, not a screen mask, view depth, or the Gaussian center.
            // A roof cell can contain more than one authored triangle.
            uint32 CoverageBits = 0u;
            const TArray<int32>* SourceTriangles = RoofCellTriangles.Find(Key);
            if (!SourceTriangles) continue;
            for (int32 SubY = 0; SubY < 4; ++SubY)
            for (int32 SubX = 0; SubX < 4; ++SubX)
            {
                const double SampleX = (double(Key.X) + (double(SubX) + 0.5) * 0.25) * RoofStep;
                const double SampleY = (double(Key.Y) + (double(SubY) + 0.5) * 0.25) * RoofStep;
                bool bCovered = false;
                for (int32 TriIndex : *SourceTriangles)
                {
                    if (!RoofTriangles.IsValidIndex(TriIndex)) continue;
                    const FRoofTri& Tri = RoofTriangles[TriIndex];
                    if (FVector::DotProduct(N, Tri.N) < 0.55) continue;
                    double SampleZ = 0.0;
                    if (!HeightAt(Tri.A, Tri.B, Tri.C, SampleX, SampleY, SampleZ)) continue;
                    if (SampleZ <= CeilingZ || FMath::Abs(SampleZ - double(Roof->Z)) > RoofStep * 1.5) continue;
                    bCovered = true;
                    break;
                }
                if (bCovered) CoverageBits |= (1u << (SubY * 4 + SubX));
            }
            if (CoverageBits == 0u)
            {
                ++RoofRejectedEmptyCoverage;
                continue;
            }
            if (CoverageBits != 0xFFFFu) ++RoofPartialCells;
            const double X = (double(Key.X) + 0.5) * RoofStep;
            const double Y = (double(Key.Y) + 0.5) * RoofStep;
            const FVector WorldP(X, Y, double(Roof->Z));
            FVector SupportedP = WorldP + FVector(0,0,RoofLiftZ);
            FVector SupportedN = N;
            if (bPuddleSourceFittedRoof && !FitRoofSource(WorldP, N, SupportedP, SupportedN))
            {
                ++P18RejectedUnsupportedRoof;
                continue; // source-first: do not invent a proxy-only roof water sheet
            }
            const FVector LocalP = GSWorld.InverseTransformPosition(SupportedP);
            const FVector LocalN = GSWorld.InverseTransformVectorNoScale(SupportedN).GetSafeNormal();
            if (LocalP.ContainsNaN() || LocalN.IsNearlyZero()) continue;
            const FQuat RoofRot = FQuat::FindBetweenNormals(FVector::UpVector, LocalN);
            const double AxisX = GSWorld.TransformVector(RoofRot.RotateVector(FVector::ForwardVector)).Size();
            const double AxisY = GSWorld.TransformVector(RoofRot.RotateVector(FVector::RightVector)).Size();
            const double AxisZ = GSWorld.TransformVector(RoofRot.RotateVector(FVector::UpVector)).Size();
            if (FMath::Min3(AxisX,AxisY,AxisZ) < 1.e-5) continue;
            FGaussianSplatData G;
            G.Position = FVector3f(LocalP);
            G.Rotation = FQuat4f(float(RoofRot.X), float(RoofRot.Y), float(RoofRot.Z), float(RoofRot.W));
            // XY raster footprint expands on inclined roof planes.
            // P16: micro-GS footprint, NOT the old tilted 40-100cm blanket splats.
            // Smaller spacing + proportional covariance keeps roof density high without giant sky halos.
            const double Sigma = RoofStep * 0.48 / FMath::Max(double(SupportedN.Z), 0.55);
            G.Scale = FVector3f(float(Sigma / AxisX), float(Sigma / AxisY), float(0.55 / AxisZ));
            // P15: explicit opaque bake affects ROOF as well as floor. Runtime
            // blue debug bypasses this source opacity for an unequivocal check.
            G.Opacity = bPuddleDebugOpaqueBake ? 0.995f : 0.985f * RoofAlphaScale;
            // The water pixel shader replaces colour completely. Reserve the
            // roof-only RGB base as a lossless-ish 16-bit subcell bitmap in
            // FP16 texture channels (two 8-bit values), plus a roof marker.
            // Floor colours/alpha, original GS and snow asset stay unchanged.
            const float MaskLo = float(CoverageBits & 255u) / 255.0f;
            const float MaskHi = float((CoverageBits >> 8) & 255u) / 255.0f;
            G.SH_DC = FVector3f((0.96f - 0.5f) / SH_C0,
                                (MaskLo - 0.5f) / SH_C0,
                                (MaskHi - 0.5f) / SH_C0);
            Splats.Add(G);
            ++RoofSplatCount;
        }
    }
    // P18: tiny three-dimensional leaf droplets; no large horizontal roof
    // sheet and no water-ring effect on foliage. Deterministic bake positions.
    if (bGenerateRoofs && bPuddleSourceFoliage && !LeafSourceGrid.IsEmpty())
    {
        TArray<FIntVector> LeafKeys;
        LeafSourceGrid.GenerateKeyArray(LeafKeys);
        LeafKeys.Sort([](const FIntVector& A, const FIntVector& B)
        {
            return A.Z == B.Z ? (A.Y == B.Y ? A.X < B.X : A.Y < B.Y) : A.Z < B.Z;
        });
        const int32 LeafBudget = FMath::Min(14000, FMath::Max(0, MaxCount - Splats.Num()));
        // Uniform deterministic reduction instead of taking an arbitrary first
        // world corner when the source has millions of leaf voxels.
        const int32 Skip = FMath::Max(1, FMath::DivideAndRoundUp(LeafKeys.Num(), FMath::Max(LeafBudget,1)));
        for (int32 I = 0; I < LeafKeys.Num() && P18LeafSplatCount < LeafBudget; I += Skip)
        {
            const FSourceWetCell* Leaf = LeafSourceGrid.Find(LeafKeys[I]);
            if (!Leaf || Leaf->Count < 2 || Leaf->NormalSum.IsNearlyZero()) continue;
            const FVector N = Leaf->NormalSum.GetSafeNormal();
            const FVector P = Leaf->PositionSum + N*1.5;
            const FVector LocalP = GSWorld.InverseTransformPosition(P);
            const FVector LocalN = GSWorld.InverseTransformVectorNoScale(N).GetSafeNormal();
            if (LocalP.ContainsNaN() || LocalN.IsNearlyZero()) continue;
            const FQuat Q = FQuat::FindBetweenNormals(FVector::UpVector, LocalN);
            const double RX = GSWorld.TransformVector(Q.RotateVector(FVector::ForwardVector)).Size();
            const double RY = GSWorld.TransformVector(Q.RotateVector(FVector::RightVector)).Size();
            const double RZ = GSWorld.TransformVector(Q.RotateVector(FVector::UpVector)).Size();
            if (FMath::Min3(RX,RY,RZ) < 1.0e-5) continue;
            FGaussianSplatData G;
            G.Position = FVector3f(LocalP);
            G.Rotation = FQuat4f(float(Q.X),float(Q.Y),float(Q.Z),float(Q.W));
            G.Scale = FVector3f(float(4.5/RX),float(3.0/RY),float(0.40/RZ));
            G.Opacity = 0.76f;
            // 0.65 marker is foliage-only (roof = 0.96, floor = 0.035).
            G.SH_DC = FVector3f((0.65f-0.5f)/SH_C0, (0.0f-0.5f)/SH_C0, (0.0f-0.5f)/SH_C0);
            Splats.Add(G);
            ++P18LeafSplatCount;
        }
    }
    if (Splats.IsEmpty())
    {
        DeletePuddleGaussians();

        PuddleGaussianBuildStatus = TEXT("[P07] 실패: 연결된 낮은 지역 없음. 물 깊이/웅덩이 크기 또는 전체 지면 진단을 조절하세요");
        return;
    }

#if WITH_EDITOR
    Modify();
    PuddleGaussianComponent->Modify();
#endif
    UGaussianSplatAsset* NewAsset = NewObject<UGaussianSplatAsset>(this, NAME_None, RF_Transactional);
    NewAsset->SHBands = 0;
    NewAsset->InitializeFromSplatData(Splats, EGaussianQualityLevel::Medium);
    if (!NewAsset->IsValid())
    {
        DeletePuddleGaussians();

        PuddleGaussianBuildStatus = TEXT("[비 Gaussian] 생성 자산 초기화 오류");
        return;
    }

    PuddleGaussianComponent->SetVisibility(false, false);
    PuddleGaussianComponent->SetHiddenInGame(true);
    PuddleGaussianComponent->SetSplatAsset(nullptr);
    BakedPuddleGaussianAsset = NewAsset;
    PuddleSourceDampenPackedBits = MoveTemp(NewDampenBits);
    // Keep the pre-existing P08 partial-basin ground-damping workflow working:
    // that path does not run the P18 roof/leaf scan at all.
    PuddleDampenSourceAsset = (bSourceCovarianceReadable &&
        (!bGenerateRoofs || bP18CovarianceReady)) ? SourceAsset : nullptr;
    PuddleDampenSourceTransform = GSWorld;
    GaussianSplatComponent->RefreshPuddleDampenRenderState();
    PuddleGaussianComponent->SetRelativeTransform(FTransform::Identity);
    PuddleGaussianComponent->SetSplatAsset(NewAsset);
    bShowPuddleGaussianTest = true;
    if (bPuddleGaussianDebugFullGround)
    {
        const int32 UnfilledGroundCells = FMath::Max(0, Cells.Num() - FloorSplatCount);
        const int32 UnfilledRoofCells = FMath::Max(0, RoofCells.Num() - RoofSplatCount);
        PuddleGaussianBuildStatus = FString::Printf(
            TEXT("[비 Gaussian] 바닥 %d/%d셀 / 지붕 %d/%d셀 / 경계셀 %d / 커버리지 없음 %d / 누락 바닥 %d, 지붕 %d / 총 %d / 경사 제외 %d / 지붕 높이제외 %d / 바닥 간격 %.0fcm, 지붕 간격 %.0fcm / 지붕 Z높임 %.1fcm / 4x4 클립"),
            FloorSplatCount, Cells.Num(), RoofSplatCount, RoofCells.Num(), RoofPartialCells,
            RoofRejectedEmptyCoverage, UnfilledGroundCells, UnfilledRoofCells,
            Splats.Num(), RejectedSlope, RoofRejectedHeight, Step, RoofStep, RoofLiftZ);
        PuddleGaussianBuildStatus += FString::Printf(
            TEXT(" / 원본기와 증거 %d / 지붕 측정불가 %d / 식생색상 후보 %d / 식생 물GS %d / 원본공분산 %s / 색상읽기 %s"),
            SourceSurfaceVotes, P18RejectedUnsupportedRoof, PlantColorVotes, P18LeafSplatCount,
            bP18CovarianceReady ? TEXT("OK") : TEXT("없음"),
            bP18ColorsReady ? TEXT("OK") : TEXT("없음"));
        PuddleGaussianBuildStatus += FString::Printf(
            TEXT(" / 원본GS 지붕 %d개 / 나무 %d개 / 대응 마스크 %s"),
            OriginalRoofWetCandidates, OriginalFoliageWetCandidates,
            PuddleDampenSourceAsset ? TEXT("유효") : TEXT("비활성"));
        if (UnfilledGroundCells > 0 || UnfilledRoofCells > 0)
            PuddleGaussianBuildStatus += TEXT(" / 주의: 최대 물 스플렛 수(또는 변환/샘플 제약)로 표면 일부 누락. 생성 최대 스플렛 수를 늘려 재베이크하세요");
    }
    else
    {
        PuddleGaussianBuildStatus = FString::Printf(
            TEXT("[P08] 고정 수면: %d 물GS / 웅덩이 %d개 / 전체지면 %d셀 / 후보 %d / 제외 %d / 평탄대체 %d / 경사제외 %d / GS근접 %lld / GS수면높이차 평균 %.1fcm 최대 %.1fcm (%lld 표본) / 넓은 바닥GS 후보 %d / 공분산 %s / 고불투명 %s"),
            Splats.Num(), SelectedSeeds.Num(), Cells.Num(), SeedCandidates.Num(), DroppedSmallPatches,
            bUsedFlatFallback ? 1 : 0, RejectedSlope, NearbySourceCount, MeanDeltaCm,
            MaxDeltaCm, SourceHeightSumCount, BroadCandidateCount,
            bSourceCovarianceReadable ? TEXT("OK") : TEXT("없음(약화차단)"),
            bPuddleDebugOpaqueBake ? TEXT("ON") : TEXT("OFF"));
    }
    SyncWeatherEffects();
#if WITH_EDITOR
    MarkPackageDirty();
#endif
    UE_LOG(LogTemp, Display, TEXT("[GSPuddle07] %s"), *PuddleGaussianBuildStatus);
}
