// Editor preprocessing only: opacity/area weighted Gaussian moment matching.
// No dynamic sun, weather or shadow result is baked into these representatives.
static FGaussianSplatData MergePCVRMoments(const TArray<FGaussianSplatData>& S,int32 Start,int32 Count)
{
 FGaussianSplatData R;double Weight=0,Mass=0;FVector3d Mean=FVector3d::ZeroVector;
 auto Area=[](const FVector3f& V){return FMath::Max((double(V.X)*V.Y+double(V.Y)*V.Z+double(V.Z)*V.X)/3.0,1.e-20);};
 auto W=[&](const FGaussianSplatData& G){return FMath::Max(-FMath::Loge(FMath::Clamp(1.0-double(G.Opacity),1.e-6,1.0))*Area(G.Scale),1.e-20);};
 for(int32 I=0;I<Count;++I){const auto& G=S[Start+I];double T=W(G);Weight+=T;Mean+=FVector3d(G.Position)*T;}
 Mean/=Weight;R.Position=FVector3f(Mean);double A[3][3]={},V[3][3]={{1,0,0},{0,1,0},{0,0,1}};
 FVector3d DC=FVector3d::ZeroVector,SH[15];for(auto& H:SH)H=FVector3d::ZeroVector;
 for(int32 I=0;I<Count;++I){const auto& G=S[Start+I];double T=W(G),F=T/Weight;Mass+=T;FVector3d D=FVector3d(G.Position)-Mean;
  FVector3f Axes[3]={G.Rotation.RotateVector(FVector3f(1,0,0)),G.Rotation.RotateVector(FVector3f(0,1,0)),G.Rotation.RotateVector(FVector3f(0,0,1))};
  for(int X=0;X<3;++X)for(int Y=0;Y<3;++Y){double C=D[X]*D[Y];for(int K=0;K<3;++K)C+=double(Axes[K][X])*Axes[K][Y]*double(G.Scale[K])*G.Scale[K];A[X][Y]+=F*C;}
  DC+=FVector3d(G.SH_DC)*F;for(int H=0;H<15;++H)SH[H]+=FVector3d(G.SH[H])*F;
 }
 double Original[3][3];FMemory::Memcpy(Original,A,sizeof(A));
 for(int Iter=0;Iter<32;++Iter){int P=0,Q=1;for(int X=0;X<3;++X)for(int Y=X+1;Y<3;++Y)if(FMath::Abs(A[X][Y])>FMath::Abs(A[P][Q])){P=X;Q=Y;}
  if(FMath::Abs(A[P][Q])<1.e-12*FMath::Max(A[0][0]+A[1][1]+A[2][2],1.e-20))break;
  double T=.5*FMath::Atan2(2*A[P][Q],A[Q][Q]-A[P][P]),C=FMath::Cos(T),B=FMath::Sin(T),AP=A[P][P],AQ=A[Q][Q],PQ=A[P][Q];
  for(int K=0;K<3;++K)if(K!=P&&K!=Q){double KP=A[K][P],KQ=A[K][Q];A[K][P]=A[P][K]=C*KP-B*KQ;A[K][Q]=A[Q][K]=B*KP+C*KQ;}
  A[P][P]=C*C*AP-2*C*B*PQ+B*B*AQ;A[Q][Q]=B*B*AP+2*C*B*PQ+C*C*AQ;A[P][Q]=A[Q][P]=0;
  for(int K=0;K<3;++K){double KP=V[K][P],KQ=V[K][Q];V[K][P]=C*KP-B*KQ;V[K][Q]=B*KP+C*KQ;}
 }
 FMatrix44f M=FMatrix44f::Identity;for(int Axis=0;Axis<3;++Axis)for(int K=0;K<3;++K)M.M[Axis][K]=float(V[K][Axis]);
 R.Rotation=FQuat4f(M).GetNormalized();R.Scale=FVector3f(FMath::Sqrt(FMath::Max(A[0][0],1.e-16)),FMath::Sqrt(FMath::Max(A[1][1],1.e-16)),FMath::Sqrt(FMath::Max(A[2][2],1.e-16)));
 R.Opacity=float(1-FMath::Exp(-Mass/Area(R.Scale)));R.SH_DC=FVector3f(DC);for(int H=0;H<15;++H)R.SH[H]=FVector3f(SH[H]);
 // Validate the actual UE quaternion convention, not only the eigensolver.
 double Error=0,Norm=0;FVector3f Axes[3]={R.Rotation.RotateVector(FVector3f(1,0,0)),R.Rotation.RotateVector(FVector3f(0,1,0)),R.Rotation.RotateVector(FVector3f(0,0,1))};
 for(int X=0;X<3;++X)for(int Y=0;Y<3;++Y){double C=0;for(int K=0;K<3;++K)C+=double(Axes[K][X])*Axes[K][Y]*double(R.Scale[K])*R.Scale[K];Error+=FMath::Abs(C-Original[X][Y]);Norm+=FMath::Abs(Original[X][Y]);}
 checkf(FMath::IsFinite(Error)&&Error<=1.e-4*FMath::Max(Norm,1.e-10),TEXT("PCVR LOD covariance reconstruction failed: %g / %g"),Error,Norm);
 return R;
}
