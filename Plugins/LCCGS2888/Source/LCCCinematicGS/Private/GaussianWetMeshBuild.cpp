#include "GaussianSplatActor.h"
#include "GaussianSplatComponent.h"
#include "GaussianWetMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "StaticMeshResources.h"
#include "RenderingThread.h"
#include "RHICommandList.h"

namespace
{
struct FWetTriangle
{
    FVector A, B, C, N;
    FBox Bounds;
};
// XY bins accelerate vertical rain tests. Very large triangles are kept in a
// separate bounded list rather than inserting millions of grid entries.
constexpr double Cell = 200.0;
FIntPoint Bin(const FVector& P) { return FIntPoint(FMath::FloorToInt(P.X / Cell), FMath::FloorToInt(P.Y / Cell)); }
bool Above(const FVector& P, const FWetTriangle& T)
{
    if (P.X < T.Bounds.Min.X || P.X > T.Bounds.Max.X || P.Y < T.Bounds.Min.Y || P.Y > T.Bounds.Max.Y || T.Bounds.Max.Z <= P.Z + 1.0) return false;
    const FVector U = T.B - T.A, V = T.C - T.A, Q = P - T.A;
    const double D = U.X * V.Y - U.Y * V.X;
    if (FMath::Abs(D) < 1.e-8) return false;
    const double S = (Q.X * V.Y - Q.Y * V.X) / D;
    const double R = (U.X * Q.Y - U.Y * Q.X) / D;
    return S >= 0 && R >= 0 && S + R <= 1 && T.A.Z + S * U.Z + R * V.Z > P.Z + 1.0;
}
}

void AGaussianSplatActor::GenerateWetSurfaceMesh()
{
    UGaussianSplatComponent* C = GaussianSplatComponent;
    if (!C) return;
    UStaticMesh* Mesh = C->ProxyShadowMesh;
    const FStaticMeshRenderData* Data = Mesh ? Mesh->GetRenderData() : nullptr;
    if (!Data || Data->LODResources.IsEmpty())
    {
        C->WetMeshBuildStatus = TEXT("실패: 프록시 메시가 없습니다");
        return;
    }
    const FStaticMeshLODResources& LOD = Data->LODResources[0];
    const uint32 VertexCount = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
    const uint32 IndexCount = LOD.IndexBuffer.GetNumIndices();
    if (!VertexCount || !IndexCount || !LOD.VertexBuffers.PositionVertexBuffer.GetVertexData() || IndexCount / 3 > 200000)
    {
        C->WetMeshBuildStatus = TEXT("실패: 프록시 CPU 데이터 없음 또는 20만 삼각형 초과");
        return;
    }
    const FTransform ProxyToWorld = C->GetProxyShadowMeshWorldTransform();
    TArray<FWetTriangle> Triangles;
    Triangles.Reserve(IndexCount / 3);
    for (uint32 I = 0; I + 2 < IndexCount; I += 3)
    {
        uint32 IA = LOD.IndexBuffer.GetIndex(I), IB = LOD.IndexBuffer.GetIndex(I + 1), IC = LOD.IndexBuffer.GetIndex(I + 2);
        if (IA >= VertexCount || IB >= VertexCount || IC >= VertexCount) continue;
        FWetTriangle T;
        T.A = ProxyToWorld.TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(IA)));
        T.B = ProxyToWorld.TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(IB)));
        T.C = ProxyToWorld.TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(IC)));
        T.N = FVector::CrossProduct(T.B - T.A, T.C - T.A).GetSafeNormal();
        if (T.N.IsNearlyZero()) continue;
        // Respect authored normals instead of abs(N.z), which would admit ceilings.
        const FVector Authored = ProxyToWorld.TransformVectorNoScale(FVector(LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(IA)));
        if (FVector::DotProduct(T.N, Authored) < 0) T.N *= -1;
        T.Bounds = FBox(ForceInit); T.Bounds += T.A; T.Bounds += T.B; T.Bounds += T.C;
        Triangles.Add(T);
    }
    TMap<FIntPoint, TArray<int32>> Grid;
    TArray<int32> Large;
    if (C->bWetMeshCheckRainExposure)
    {
        for (int32 I = 0; I < Triangles.Num(); ++I)
        {
            const FIntPoint Lo = Bin(Triangles[I].Bounds.Min), Hi = Bin(Triangles[I].Bounds.Max);
            if (int64(Hi.X - Lo.X + 1) * int64(Hi.Y - Lo.Y + 1) > 256) { Large.Add(I); continue; }
            for (int32 X = Lo.X; X <= Hi.X; ++X)
                for (int32 Y = Lo.Y; Y <= Hi.Y; ++Y) Grid.FindOrAdd(FIntPoint(X, Y)).Add(I);
        }
    }
    auto IsExposed = [&](const FVector& P, int32 Self)
    {
        if (!C->bWetMeshCheckRainExposure) return true;
        if (const TArray<int32>* Candidates = Grid.Find(Bin(P)))
            for (int32 I : *Candidates) if (I != Self && Above(P, Triangles[I])) return false;
        for (int32 I : Large) if (I != Self && Above(P, Triangles[I])) return false;
        return true;
    };
    TArray<FVector> Built;
    const double MinUp = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(C->WetMeshMaxSlopeDegrees, 0.f, 80.f)));
    const FTransform WorldToActor = GetActorTransform().Inverse();
    int32 Sheltered = 0;
    for (int32 I = 0; I < Triangles.Num(); ++I)
    {
        const auto& T = Triangles[I];
        if (T.N.Z < MinUp) continue;
        const FVector Center = (T.A + T.B + T.C) / 3.0;
        // Conservative four-point test; partially sheltered triangles are omitted.
        if (!IsExposed(Center, I) || !IsExposed(FMath::Lerp(Center,T.A,0.8), I) ||
            !IsExposed(FMath::Lerp(Center,T.B,0.8), I) || !IsExposed(FMath::Lerp(Center,T.C,0.8), I)) { ++Sheltered; continue; }
        const FVector Offset = T.N * FMath::Clamp(C->WetMeshSurfaceOffsetCm, 0.f, 10.f);
        Built.Add(WorldToActor.TransformPosition(T.A + Offset));
        Built.Add(WorldToActor.TransformPosition(T.B + Offset));
        Built.Add(WorldToActor.TransformPosition(T.C + Offset));
    }
    Modify(); C->Modify();
    WetSurfaceMeshVertices = MoveTemp(Built);
    ++WetMeshRevision;
    C->WetReceiverTestMode = EGSWetReceiverTestMode::WetMesh;
    C->WetMeshBuildStatus = FString::Printf(TEXT("실제 젖음 메시: %d 삼각형 / 비 차폐 제외 %d / 카메라 독립 베이크"), WetSurfaceMeshVertices.Num()/3, Sheltered);
    MarkPackageDirty();
    SyncWetSurfaceMesh();
    UE_LOG(LogTemp, Display, TEXT("[GSWetMesh] %s"), *C->WetMeshBuildStatus);
}

void AGaussianSplatActor::DeleteWetSurfaceMesh()
{
    Modify(); WetSurfaceMeshVertices.Reset(); ++WetMeshRevision;
    if (GaussianSplatComponent)
    {
        GaussianSplatComponent->Modify();
        GaussianSplatComponent->WetReceiverTestMode = EGSWetReceiverTestMode::SplatSize;
        GaussianSplatComponent->WetMeshBuildStatus = TEXT("젖음 메시 삭제됨");
    }
    MarkPackageDirty(); SyncWetSurfaceMesh();
}

void AGaussianSplatActor::SyncWetSurfaceMesh()
{
    if (!GetWorld() || !GaussianSplatComponent) return;
    const FTransform Transform = GetActorTransform();
    const int32 Mode = static_cast<int32>(GaussianSplatComponent->WetReceiverTestMode);
    const FSceneInterface* Scene = GetWorld()->Scene;
    if (WetMeshPublishedRevision == WetMeshRevision && WetMeshPublishedMode == Mode &&
        WetMeshPublishedScene == Scene && WetMeshPublishedTransform.Equals(Transform)) return;
    TArray<FVector4f> WorldVertices;
    if (Mode >= 3)
    {
        WorldVertices.Reserve(WetSurfaceMeshVertices.Num());
        for (const FVector& P : WetSurfaceMeshVertices) WorldVertices.Emplace(FVector3f(Transform.TransformPosition(P)), 1.f);
    }
    ENQUEUE_RENDER_COMMAND(PublishWetMesh)([Scene, Vertices = MoveTemp(WorldVertices)](FRHICommandListImmediate&) mutable
    { PublishGaussianWetMesh(Scene, MoveTemp(Vertices)); });
    WetMeshPublishedRevision = WetMeshRevision; WetMeshPublishedMode = Mode;
    WetMeshPublishedScene = Scene; WetMeshPublishedTransform = Transform;
}
