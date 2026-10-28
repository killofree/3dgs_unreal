// Opt-in diagnostics only. No rendering state, resource size, or GPU policy changes.
#include "DynamicRHI.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/CoreDelegates.h"
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <dxgi1_4.h>
#include <d3d12.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif
static void RecordGSVRHitchTelemetry(double Elapsed,double Now)
{
 static const bool Enabled=FParse::Param(FCommandLine::Get(),TEXT("GSVRHitchTelemetry"));
 if(!Enabled)return;
 static double LastSample=-1,PreviousFrame=0;
 const double WallMs=PreviousFrame>0?(Now-PreviousFrame)*1000.0:0;PreviousFrame=Now;
 const bool Hitch=Elapsed>=5 && WallMs>50;
 if(!Hitch && LastSample>=0 && Now-LastSample<0.25)return;
 LastSample=Now;const double QueryStart=FPlatformTime::Seconds();
 const uint64 QPC=FPlatformTime::Cycles64();
 const int64 UtcUnixMs=(FDateTime::UtcNow().GetTicks()-FDateTime(1970,1,1).GetTicks())/ETimespan::TicksPerMillisecond;
#if PLATFORM_WINDOWS
 struct FAdapterProbe {
  IDXGIAdapter3* Adapter=nullptr;ID3D12Device* Device=nullptr;HRESULT Init=E_FAIL;void* DXGIDll=nullptr;
  FAdapterProbe(){
   if(!GDynamicRHI || FCString::Stricmp(GDynamicRHI->GetName(),TEXT("D3D12"))!=0)return;
   Device=static_cast<ID3D12Device*>(GDynamicRHI->RHIGetNativeDevice());if(!Device)return;
   DXGIDll=FPlatformProcess::GetDllHandle(TEXT("dxgi.dll"));if(!DXGIDll)return;
   using FCreateFactory=HRESULT(WINAPI*)(UINT,REFIID,void**);
   auto Create=reinterpret_cast<FCreateFactory>(FPlatformProcess::GetDllExport(DXGIDll,TEXT("CreateDXGIFactory2")));
   if(!Create)return;IDXGIFactory4* Factory=nullptr;Init=Create(0,__uuidof(IDXGIFactory4),reinterpret_cast<void**>(&Factory));
   if(SUCCEEDED(Init)){Init=Factory->EnumAdapterByLuid(Device->GetAdapterLuid(),__uuidof(IDXGIAdapter3),reinterpret_cast<void**>(&Adapter));Factory->Release();}
  }
  ~FAdapterProbe(){if(Adapter)Adapter->Release();if(DXGIDll)FPlatformProcess::FreeDllHandle(DXGIDll);}
 };
 static FAdapterProbe Probe;
 DXGI_QUERY_VIDEO_MEMORY_INFO Local={},Shared={};
 const HRESULT LocalHR=Probe.Adapter?Probe.Adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&Local):Probe.Init;
 const HRESULT SharedHR=Probe.Adapter?Probe.Adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&Shared):Probe.Init;
 const LUID Luid=Probe.Device?Probe.Device->GetAdapterLuid():LUID{};
 UE_LOG(LogTemp,Display,TEXT("[GSHitch][Memory] Elapsed=%.6f QPC=%llu UtcUnixMs=%lld PID=%u LuidHigh=%u LuidLow=%u WallMs=%.4f Hitch=%d LocalHR=0x%08x LocalUsage=%llu LocalBudget=%llu LocalReservation=%llu SharedHR=0x%08x SharedUsage=%llu SharedBudget=%llu QueryMs=%.4f ProcessScope=Unreal"),
  Elapsed,QPC,UtcUnixMs,FPlatformProcess::GetCurrentProcessId(),uint32(Luid.HighPart),Luid.LowPart,WallMs,Hitch?1:0,uint32(LocalHR),Local.CurrentUsage,Local.Budget,Local.CurrentReservation,uint32(SharedHR),Shared.CurrentUsage,Shared.Budget,(FPlatformTime::Seconds()-QueryStart)*1000);
#else
 UE_LOG(LogTemp,Display,TEXT("[GSHitch][Unsupported] Elapsed=%.6f QPC=%llu UtcUnixMs=%lld PlatformWindows=0"),Elapsed,QPC,UtcUnixMs);
#endif
}
// Explicit startup-only validation: query the native device without loading or
// running a scene. Dormant during ordinary editor sessions and production runs.
static void ValidateGSHitchTelemetryAtStartup()
{
 if(FParse::Param(FCommandLine::Get(),TEXT("GSVRHitchTelemetryPreflight")))
  RecordGSVRHitchTelemetry(0,FPlatformTime::Seconds());
}
struct FGSHitchTelemetryStartupRegistration
{
 FGSHitchTelemetryStartupRegistration(){FCoreDelegates::OnPostEngineInit.AddStatic(&ValidateGSHitchTelemetryAtStartup);}
};
static FGSHitchTelemetryStartupRegistration GSHitchTelemetryStartupRegistration;
