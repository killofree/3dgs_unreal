#include "GaussianSplatAsset.h"
#include "GaussianClusterBuilder.h"
#include "Misc/AutomationTest.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
namespace GSClusterBakeTests
{
static TArray<FString> Records(UGaussianSplatAsset* Asset)
{
    TArray<uint8> P,O,C,H; Asset->GetPositionData(P); Asset->GetOtherData(O);
    Asset->GetColorTextureData(C); Asset->GetSHData(H);
    const int32 Stride=UGaussianSplatAsset::GetSHBytesPerSplat(Asset->SHFormat,Asset->SHBands);
    TArray<FString> Result;
    for (int32 I=0;I<Asset->GetOriginalSplatCount();++I)
    {
        int32 X,Y; GaussianSplattingUtils::SplatIndexToTextureCoord(I,Asset->ColorTextureWidth,X,Y);
        FString Key=BytesToHex(P.GetData()+int64(I)*12,12)+BytesToHex(O.GetData()+int64(I)*28,28)+
            BytesToHex(C.GetData()+(int64(Y)*Asset->ColorTextureWidth+X)*8,8);
        if (Stride) Key+=BytesToHex(H.GetData()+int64(I)*Stride,Stride);
        Result.Add(MoveTemp(Key));
    }
    Result.Sort();return Result;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGSClusterBakePreservesRecords,
    "LCCCinematicGS.Clusters.BakePreservesPackedRecords",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGSClusterBakePreservesRecords::RunTest(const FString&)
{
    for (int32 Bands : {0,1,3})
    {
        auto* Asset=NewObject<UGaussianSplatAsset>(); Asset->SHBands=Bands;
        TArray<FGaussianSplatData> Splats;Splats.SetNum(257);
        for (int32 I=0;I<Splats.Num();++I)
        {
            auto& G=Splats[I];G.Position=FVector3f(float((I*37)%257),float(I%11),float(I%7));
            G.Scale=FVector3f(.3f,.7f,1.1f);G.Opacity=float(1+I%9)/10.f;
            G.SH_DC=FVector3f(-.2f,.4f,.9f);
            for (int32 J=0;J<15;++J)G.SH[J]=FVector3f(.01f*(I%13),-.02f*J,.03f);
        }
        Asset->InitializeFromSplatData(Splats,EGaussianQualityLevel::Medium);
        Asset->SourceFilePath=TEXT("Z:/missing-source-test.ply");
        const auto Before=GSClusterBakeTests::Records(Asset);
        if (!TestTrue(TEXT("Bake needs no source PLY"),Asset->BakeVRClusters())) return false;
        TestEqual(TEXT("Original count retained"),Asset->GetSplatCount(),257);
        TestEqual(TEXT("No replacement splats"),Asset->GetClusterHierarchy().TotalLODSplatCount,0u);
        TestTrue(TEXT("Cluster hierarchy enabled"),Asset->IsNaniteEnabled()&&Asset->HasClusterHierarchy());
        TestTrue(TEXT("Position/covariance/color/opacity/SH packed records retained"),Before==GSClusterBakeTests::Records(Asset));
        for (const auto& Cluster:Asset->GetClusterHierarchy().Clusters)
        {
            if (!Cluster.IsLeaf())continue;
            for (uint32 I=Cluster.SplatStartIndex;I<Cluster.SplatStartIndex+Cluster.SplatCount;++I)
            {
                const auto Positions=Asset->GetDecompressedPositions();
                const FVector3f Pos(Positions[I]);
                TestTrue(TEXT("Bounds contain Gaussian support"),
                    Cluster.BoundsMin.X<=Pos.X-3.3f && Cluster.BoundsMax.X>=Pos.X+3.3f);
            }
        }
        TestTrue(TEXT("Rebake succeeds without appending originals"),Asset->BakeVRClusters());
        TestTrue(TEXT("Rebake retains packed records"),Before==GSClusterBakeTests::Records(Asset));
        Asset->PositionFormat=EGaussianPositionFormat::Norm16;
        AddExpectedError(TEXT("Unsupported or invalid imported asset"),EAutomationExpectedErrorFlags::Contains,1);
        TestFalse(TEXT("Unsupported formats fail before mutation"),Asset->BakeVRClusters());
        TestEqual(TEXT("Failure leaves count intact"),Asset->GetSplatCount(),257);
    }
    return true;
}
#endif
