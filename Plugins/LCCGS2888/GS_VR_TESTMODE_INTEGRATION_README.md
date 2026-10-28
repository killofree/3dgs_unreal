# LCCGS2888(7) 기반 VR 성능 기록 통합판 (테스트 모드 버튼)

## 기준본 보호
- **LCCGS2888(7).zip** 의 `LCCCinematicGS.uplugin`, 모든 원본 HLSL, 원본 렌더링 품질/환경/날씨/적설/조명/그림자 기능은 유지.
- V8 계측 코드에서 **진단 부분만** 통합. GS 없는 씬의 보호 가드(`GSDesktopPerfProbe.cpp`)와 `GaussianSplatAsset.cpp` 원본을 그대로 보존.
- 기준본 (7)에 없는 `GSRainSceneController` 독립 액터 등은 이식하지 않음.
- **Binaries / Intermediate 제외**: UE 5.8.3 프로젝트에서 다시 빌드. 기존 원본의 70MB 빌드 출력물을 섞지 않음.

## UI: 콘솔 입력 불필요
1. 플러그인을 `Plugins/LCCGS2888`에 설치하고 UE 에디터를 재빌드/실행.
2. 씬의 **Gaussian Splat Actor** 선택 > Details > **07 진단 및 고급**.
3. **VR 성능 테스트 모드 켜기** 버튼 클릭 또는 **VR 프레임 측정 활성화** 체크.
4. 에디터의 **VR Preview** 실행 -> 30~60초 테스트 -> VR Preview 종료.
5. 결과는 `<Project>/Saved/LCCGS/Profiling/`에 자동 저장.
6. 성능 자체를 비교하려면 VR Preview 종료 후 **테스트 모드 종료 (측정 끄기)** 버튼 클릭.

버튼은 **기록 및 FPS/Unit HUD 활성화 토글**. 에디터의 VR Preview를 자동 실행하거나 플러그인 환경 기능을 변경하지 않습니다. 테스트 모드 상태는 프로젝트의 `EditorPerProjectUserSettings.ini`에 저장되며, PIE 실행 중 변경할 수 없습니다.

### 버튼으로 일괄 적용되는 6개 옵션
- `gs.PCVR.RecordPreview=1`: 프레임 기록.
- `gs.PCVR.RecordEveryNFrames=1`: 매 프레임.
- `gs.PCVR.RecordGPUTrace=1`: `.utrace` 자동 저장.
- `gs.PCVR.AutoExportGPUTimers=1`: Unreal Insights 헤드리스 GPU 타이머 CSV.
- `gs.PCVR.RecordIndirectCounts=1`: 저빈도 비동기 Indirect 인스턴스 샘플 (GPU 추가 비용 발생).
- `gs.PCVR.PreviewStats=1`: VR Preview의 FPS/Unit 디스플레이 (GS 씬 한정).

버튼 OFF 시 에디터 세션에서 기록 전의 각 콘솔 변수 값 복원. 저장된 ON/OFF 상태는 다음 에디터 시작 시 재적용.

## 출력
- `GS_Preview_<timestamp>.csv`: 프레임별 GPU 전체 시간, 각 GS 함수 호출 수, CPU 명령 준비 시간, 입력 후보·reuse.
- `_Summary.txt`: 통합 CPU, 카운터 요약.
- `.utrace`: Unreal Insights GPU/CPU trace.
- `_GPU_Timers.csv`: GPU 타이머 통계, GSVRPreview region 추출 요청.
- `_GPU_GS_Timers.csv`: 주요 GS 타이머 subset 및 ms.
- `_RasterEyes.csv`: 좌우 눈별 Draw 호출, 입력 **상한**, viewport pixel sum, 선택적 비동기 간접 인스턴스 표본.
- `_FeatureSettings.csv`: GS 조명/안개/기타 옵션 스냅샷.
- `_GPU_Export_Status.txt`, `_GPU_Exporter.log`, `_GPU_Export.rsp`: 자동 내보내기 상태/로그.

## 계측 한계
- 실제 GPU ms는 **Insights GPU 타이머**에서만 유효. CPU 측정값은 GPU 셰이더 실행 시간이 아님.
- 호출 수는 C++ 함수 진입 횟수, 입력 후보 수는 실제 셰이더 Gaussian 검사 횟수가 아님.
- GPU Raster 부모 스코프와 LeftEye/RightEye 자식 타이머는 겹쳐 **합산 금지**.
- 조명·그림자·Fog가 결합된 셰이더에서는 독립적인 GPU ms를 아직 측정할 수 없음. 오버드로우·정확한 텍스처 샘플 수도 없음.
- GPU Indirect 표본을 읽는 진단은 성능 비용이 있으므로 측정 ON/OFF의 직접 FPS 비교에 주의.
- `GSVRPreview` region 필터와 내보내기 형식은 **로컬 UE 5.8.3에서 검증 필요**. `_GPU_Export_Status.txt`가 COMPLETE여도 GPU 행과 region 범위를 점검할 것.

## 검증 상태
- ZIP 무결성, 코드 차이, 소스 진단 연결 및 GS-only 가드 정적 검증 완료.
- **UE 5.8.3 Windows 실제 빌드/VR 실행 검증 미완료.**
