# 작업 지시서: 상단바 시작 로고를 맥 애플 로고의 비율에 맞춘다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

상수 하나를 바꾸는 작업입니다. 건드리는 파일은 `src/clock_renderer.cpp` 하나입니다.

---

## 1. 무엇이 어긋났는가

상단바 왼쪽의 윈도우 로고가 맥의 애플 로고보다 큽니다.

맥 메뉴바 스크린샷을 실측한 값입니다.

| 항목 | 픽셀 | 메뉴바 높이에 대한 비율 |
| --- | --- | --- |
| 메뉴바 높이 | 60 | 1.000 |
| 애플 로고 세로 (잎 포함) | 34 | 0.567 |
| 애플 로고 가로 | 27 | 0.450 |

bamti 는 상단바 높이가 32 DIP(`src/menu_bar.cpp` 의 `kBarHeightDip`)이고 로고가 20 DIP 이므로 비율이 **0.625** 입니다. 게다가 애플 로고는 세로로 긴 실루엣인 반면 bamti 의 로고는 네 칸이 꽉 찬 정사각형이라, 같은 세로 길이에서도 먹의 양이 더 많아 보입니다.

## 2. 얼마로 맞추는가

**16 DIP 입니다.** 세로 비율 0.567 과 가로 비율 0.450 의 중간값(기하평균 0.505)을 32 DIP 에 적용하면 16.2 가 나옵니다. 정사각형 로고의 면적감을 애플 로고와 맞추는 값이고, 상단바의 다른 상태 아이콘이 이미 16 DIP 라서 크기가 한 줄로 정돈됩니다. 사용자가 이 값을 선택했습니다.

## 3. 고칠 것

`src/clock_renderer.cpp` 18행의 상수 하나만 바꾸고, 근거를 주석으로 남깁니다.

```cpp
// 바꾸기 전
constexpr float kStartLogoDip = 20.0f;

// 바꾼 뒤
// 맥 메뉴바 실측: 바 높이의 세로 0.567, 가로 0.450. 정사각형 로고의 면적을
// 맞추면 0.505 이고, 32 DIP 바에서 16 DIP 다. 다른 상태 아이콘과도 같은 크기다.
constexpr float kStartLogoDip = 16.0f;
```

이 상수만 바꾸면 됩니다. `DrawStartButton` 이 로고의 왼쪽과 위쪽을 이 값으로 계산하므로 **가운데 정렬은 저절로 유지됩니다.**

```cpp
const float logo_left = hit_left + (kStartHitWidthDip - kStartLogoDip) * 0.5f;
const float logo_top = (height_dip - kStartLogoDip) * 0.5f;
```

## 4. 검증

**빌드는 CMake 로 하십시오.**

```
cmake -S D:\repos\bamti -B D:\repos\bamti\build
cmake --build D:\repos\bamti\build --config Release
```

1. `git diff` 가 `src/clock_renderer.cpp` 의 상수 한 줄과 주석만 보여야 합니다.
2. 새 바이너리를 WMI 로 띄웁니다.

```powershell
$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
  CommandLine = '"D:\repos\bamti\build\Release\bamti.exe"'
}
"rc=$($r.ReturnValue) pid=$($r.ProcessId)"
```

3. 상단바 왼쪽 끝을 화면 캡처해서 로고의 세로 길이를 셉니다. 96 DPI 기준으로 **16 픽셀**, 150% 배율이면 24 픽셀이어야 합니다. 화면 캡처로 읽는 것은 괜찮지만 **클릭이나 키 입력을 합성하지 마십시오.**
4. 로고를 눌렀을 때 열리던 메뉴가 그대로 열려야 합니다. 눌러 보는 확인이 필요하면 사용자에게 부탁하십시오.

## 5. 하지 말 것

- `kStartHitWidthDip`(34), `kStartHoverInsetXDip`, `kStartHoverInsetYDip` 를 바꾸지 마십시오. 누르는 영역과 호버 알약은 지금 그대로 둡니다.
- `kLogoView`(11.5)를 바꾸지 마십시오. 로고 도형의 내부 단위이고, 이 값을 건드리면 네 칸의 간격 비율이 무너집니다.
- 로고의 그러데이션 색이나 모서리 반지름을 손대지 마십시오.
- `kBarHeightDip` 을 바꾸지 마십시오. 상단바 높이는 이번 작업의 대상이 아닙니다.
- `src/clock_renderer.cpp` 외의 파일을 건드리지 마십시오.
