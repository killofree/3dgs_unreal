# VR360Skybox v0.5 — 바닥 정렬 우선 버전 (UE 5.8.3)

## 이번에 추가한 기능
* `VR360 | Floor Alignment > Panorama Pitch Degrees`: 파노라마 영상의 위아래 보이는 각도를 조절해 영상 속 지면/지평선 정렬. 기본 0°, 권장 시작 ±5°.
* `Show Floor Plane`: 실제 씬 기준 바닥 평면 표시 (기본 OFF). 360 영상 속 바닥과는 별개인 메쉬이며 회색 기본 머티리얼로 보일 수 있음.
* `Floor World Z`: 평면의 월드 Z 높이(cm), 기본 0.
* `Floor Size Cm`: 평면 크기(cm), 기본 800.
* 기존 큐브 방향 기반 샘플링, Video Source, 재생 기능 유지.

## 설치
1. 언리얼 에디터 종료 후 현재 `D:\unreal\SDGS_VR_TEST_00\Plugins\VR360Skybox` 백업, 이 버전으로 교체.
2. UE 5.8.3 프로젝트에서 컴파일.
3. 레벨에 기존 `VR360SkyboxActor`가 있다면 선택 후 `Create / Assign Cube Material` 버튼 클릭.
4. 새 머티리얼 `/Game/VR360SkyboxGenerated/M_VR360CubeSky_FloorV05`가 할당되는지 확인. 기존 v0.4 머티리얼을 덮어쓰지 않음.
5. Details > Floor Alignment에서 `Panorama Pitch Degrees`를 조금씩 조절하고 VR Preview에서 검사.
6. 실제 높이 기준이 필요할 때에만 `Show Floor Plane`을 ON으로 하고 `Floor World Z` 조절. 기본 평면은 시각적 기준이며 충돌을 갖지 않음.

## 중요한 구분
파노라마 Pitch 조절은 영상의 **지면이 보이는 위치**를 바꾸는 것이지, 촬영된 지면을 실제 3D 기하로 복구하거나 카메라의 높이를 변경하는 기능이 아닙니다. 영상 바닥과 실제 3D 평면이 만나는 부분은 원본 영상의 촬영 높이·깊이 정보가 없어서 자동으로 일치할 수 없습니다. 영상과 실제 바닥을 자동으로 부드럽게 이어주는 블렌딩 기능은 아직 포함되지 않았습니다.

이 ZIP은 UE 5.8.3 컴파일 및 VR 실기기 검증을 수행하지 않은 소스 버전입니다.
