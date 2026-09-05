# 작업 지시서: 볼륨 변화를 폴링이 아니라 알림으로 받는다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`TASK-BAR-ICON-ART.md`까지 끝난 상태에서 시작하십시오. 이 작업은 `src/widgets/volume.{hpp,cpp}`와 `src/widgets/builtin.{hpp,cpp}`만 건드리므로 `TASK-BAR-REORDER.md`와 파일이 겹치지 않습니다.

---

## 0. 완료 조건

키보드 볼륨 키를 누르면 상단바 아이콘과 열려 있는 패널의 슬라이더가 **즉시** 따라옵니다. 사용자가 "지금 눌렀는데 지금 바뀌었다"고 느껴야 합니다.

---

## 1. 지금 왜 1초가 걸리는가

사용자가 실제로 써 보고 "1초 정도 뒤에 따라온다"고 보고했습니다. 코드에서 그 1초의 출처가 확인됩니다.

### 1-1. 지연 예산

| 구간 | 코드 위치 | 지연 |
|---|---|---|
| 볼륨 키를 눌러 시스템 볼륨이 바뀜 | — | 0 |
| 워커가 다음 표본 시각까지 잠들어 있음 | `src/widgets/builtin.cpp` `kVolumePeriodMs = 1000` | **0 ~ 1000ms** |
| `SampleVolume`이 읽어 `Publish` | `VolumeControl::Read` | 1ms 미만 |
| 알림 병합 | `src/status_registry.cpp` `kNotifyCoalesceMs = 50` | 0 ~ 50ms |
| 다시 그리기 | — | 무시할 수준 |

폴링 구간이 압도적입니다. 평균 500ms, 최악 1000ms입니다. 사용자가 보고한 "1초 정도"와 정확히 맞습니다.

### 1-2. 주기를 줄이는 것은 답이 아니다

`kVolumePeriodMs`를 100으로 낮추면 지연은 줄지만 초당 열 번 COM 호출을 하게 됩니다. 아무도 볼륨을 만지지 않는 대부분의 시간에 그 비용을 계속 냅니다. **주기를 줄이지 마십시오.**

Core Audio는 볼륨이 바뀔 때 알려 주는 콜백을 이미 제공합니다. 그것을 쓰면 지연이 사실상 0이 되고 평상시 비용도 0입니다. 남는 지연은 알림 병합 50ms뿐이며 이것은 사람이 느끼지 못합니다.

`kVolumePeriodMs = 1000` 폴링은 **그대로 두십시오.** 알림이 오지 않는 경우를 대비한 안전망입니다. 알림이 정상이면 폴링이 도는 시점에는 이미 값이 같아서 `Publish`의 지문 비교에 걸려 아무 일도 일어나지 않습니다.

---

## 2. 알림 받기

`IAudioEndpointVolumeCallback`을 구현해 `IAudioEndpointVolume::RegisterControlChangeNotify`로 등록합니다. 볼륨 키, 시스템 볼륨 믹서, 다른 앱의 변경까지 전부 이 콜백으로 들어옵니다.

### 2-1. 콜백 스레드에서 무엇을 해도 되는가

`OnNotify`는 우리 워커가 아니라 **MMDevAPI가 소유한 스레드**에서 불립니다. 여기서 다음을 하면 안 됩니다.

- COM 인터페이스 호출 금지. `Read`, `GetMasterVolumeLevelScalar`, `Publish` 어느 것도 부르지 마십시오. 교착하거나 오디오 엔진을 붙듭니다.
- `mu_` 잠금 금지. 워커가 그 잠금을 쥔 채 COM을 부르고 있으면 교착합니다.
- 로그 쓰기 금지. 파일 입출력이 오디오 스레드를 막습니다.

**허용되는 것은 원자적 깃발 하나를 세우고 `SetEvent`를 부르는 것뿐입니다.** 두 호출 모두 잠금이 필요 없고 즉시 끝납니다.

### 2-2. 콜백 객체의 수명

여기가 이 작업에서 유일하게 까다로운 부분입니다.

`UnregisterControlChangeNotify`를 부른 시점에 **이미 진행 중인 `OnNotify`가 있을 수 있습니다.** Windows는 Unregister가 진행 중인 콜백의 완료를 기다린다고 보장하지 않습니다. 따라서 콜백 객체가 남의 수명에 의존하면 안 됩니다.

콜백 객체는 다음을 **스스로 소유**해야 합니다.

```cpp
class VolumeNotify : public IAudioEndpointVolumeCallback {
  // wake_event_를 DuplicateHandle로 복제해 자기 것으로 가진다.
  HANDLE wake_ = nullptr;
  std::atomic<bool>* dirty_ = nullptr;  // 아래 2-3 참고
  LONG ref_ = 1;
};
```

`wake_event_`를 그대로 받아 쓰지 마십시오. `BuiltinWidgets`의 소멸자가 `CloseHandle(wake_event_)`을 부르고 나면 그 핸들은 무효가 되고, 뒤늦게 도착한 콜백이 무효 핸들에 `SetEvent`를 겁니다. `DuplicateHandle`로 복제해 두면 원본이 닫혀도 복제본은 유효하며, 아무도 기다리지 않는 이벤트를 세우는 것은 무해합니다.

복제본은 **참조 카운트가 0이 될 때**, 즉 `Release()`의 마지막에서 `CloseHandle`하십시오. 생성자나 Unregister 시점이 아닙니다.

`dirty_`가 가리키는 원자 변수도 같은 문제를 겪습니다. 포인터로 넘기지 말고, `std::shared_ptr<std::atomic<bool>>`을 콜백과 `VolumeControl`이 함께 들게 하십시오. 그러면 어느 쪽이 먼저 죽어도 안전합니다.

### 2-3. 워커를 깨우는 경로

```
OnNotify (오디오 스레드)
  → *dirty_ = true
  → SetEvent(wake_)
      ↓
WorkerLoop가 wake_event_에서 깨어남
  → volume_dirty_를 소비하며 volume_due_ = 0
  → SampleDue가 volume_due_ <= now를 보고 SampleVolume 실행
  → Publish → 알림 → 다시 그리기
```

`volume_due_ = 0`으로 만드는 이유는 `SampleDue`가 `settings_.volume && volume_due_ <= now`로 판정하기 때문입니다. 0이면 항상 참입니다.

워커 루프에서 잠금을 쥔 채 다른 상태를 읽는 자리가 이미 있습니다(`acts.swap`, `level.swap` 등이 있는 블록). 깃발 소비를 그 블록에 함께 넣으십시오. 별도 잠금 구간을 만들지 마십시오.

---

## 3. 자기가 낸 변경을 되받지 않기

`SetMasterVolumeLevelScalar`와 `SetMute`의 마지막 인자는 이벤트 컨텍스트 GUID입니다. 지금은 둘 다 `nullptr`을 넘기고 있습니다.

이대로 알림을 켜면 사용자가 패널 슬라이더를 끄는 동안 이런 되먹임이 생깁니다.

```
슬라이더 드래그 → SetLevel → 시스템이 OnNotify → 워커가 SampleVolume
  → Publish → 패널 슬라이더 값 갱신 → 드래그 중인 손잡이가 흔들림
```

`volume.cpp`의 익명 이름공간에 고유 GUID를 하나 정의하고 두 Set 호출에 넘기십시오.

```cpp
// 우리가 낸 변경임을 알아보기 위한 표식이다.
constexpr GUID kVolumeEventContext = {0x...};
```

GUID는 새로 만드십시오(`New-Guid`). 다른 곳에서 가져다 쓰지 마십시오.

`OnNotify`에서 `data->guidEventContext == kVolumeEventContext`이면 **아무것도 하지 말고 즉시 S_OK를 돌려주십시오.** 우리가 낸 변경은 이미 `WorkerLoop`의 `volume_changed` 경로가 `SampleVolume`을 부르고 있습니다.

`data`가 널일 수 있습니다. 먼저 검사하십시오.

---

## 4. 등록과 해제

### 4-1. 등록 자리

`VolumeControl::Ensure`가 `IAudioEndpointVolume`을 막 얻은 직후입니다. `device_`를 채우는 블록 근처에 두십시오.

등록이 실패하면(`FAILED(hr)`) **전체를 실패로 만들지 마십시오.** 알림 없이 폴링만으로도 지금과 같이 동작합니다. 실패를 한 번만 로그로 남기고(`logged_fail_`과 같은 방식으로 반복을 막으십시오) 정상 반환하십시오.

### 4-2. 해제 자리

`VolumeControl::Release`에서, `volume_.Reset()` **보다 먼저** `UnregisterControlChangeNotify`를 부르십시오. 순서가 뒤집히면 인터페이스를 놓은 뒤에 해제를 시도하게 됩니다.

`Release`는 다음 자리에서 이미 불립니다. 전부 확인하십시오.

- `Read`/`SetLevel`/`SetMute`가 실패했을 때
- `WorkerLoop`에서 `!s.volume`일 때
- 워커 종료 직전(`volume_->Release()`)
- `~VolumeControl`

특히 **엔드포인트가 바뀔 때 재등록이 되는지**가 중요합니다. 기본 출력 장치가 바뀌면 `Read`가 실패해 `Release`가 불리고, 다음 `Ensure`가 새 엔드포인트를 잡습니다. 해제와 등록이 각각 `Release`와 `Ensure`에 들어 있으면 이 경로가 자동으로 맞습니다. **별도의 장치 전환 처리를 새로 만들지 마십시오.** 사용자가 장치 전환 반응 속도에는 이미 만족했습니다.

---

## 5. 하지 말아야 할 것

- `kVolumePeriodMs`를 줄이지 마십시오. 1000 그대로 둡니다.
- `kNotifyCoalesceMs`를 줄이지 마십시오. 50ms는 사람이 느끼지 못하고, 줄이면 다른 위젯의 그리기 비용이 늘어납니다.
- `IMMNotificationClient`를 등록하지 마십시오. 장치 전환은 이번 범위가 아닙니다.
- 콜백 안에서 `Publish`, `Read`, `Log`, `mu_.lock()` 중 어느 것도 부르지 마십시오.
- 워커 스레드를 하나 더 만들지 마십시오. 기존 워커를 깨우는 것으로 충분합니다.
- `OnNotify`가 준 `fMasterVolume`을 곧바로 상태로 쓰지 마십시오. 워커가 `Read`를 다시 부르는 편이 경로가 하나로 유지되어 안전하며, `volume read took` 로그가 이미 그 비용이 1ms 미만임을 보여 줍니다.

---

## 6. 측정

고친 뒤 **실제로 재서 기록하십시오.** 추측으로 채우지 마십시오.

`OnNotify`에서는 로그를 쓸 수 없으므로, 워커가 깃발을 소비하는 자리에서 재십시오.

1. `OnNotify`에 `QueryPerformanceCounter` 값을 원자 변수에 적어 둡니다(로그가 아니라 저장만 하는 것이므로 허용됩니다).
2. 워커가 깃발을 소비할 때 다시 재서 차이를 로그로 남깁니다.

```
volume notify -> sample %.2f ms
```

이 값이 **5ms를 넘으면 보고하십시오.** 워커가 다른 표본을 뜨느라 막혀 있다는 뜻이고, 그때는 원인을 따로 봐야 합니다.

측정이 끝나면 이 로그는 남겨 두십시오. 한 번만 찍도록 `logged_` 방식으로 막으면 됩니다.

---

## 7. 검증

1. Release 빌드가 경고 없이 통과합니다.
2. 볼륨 위젯을 켜고 키보드 볼륨 키를 한 번 누릅니다. 상단바 아이콘이 **즉시** 바뀝니다.
3. 볼륨 키를 빠르게 열 번 연타합니다. 값이 끝까지 따라오고 중간에 멈추거나 건너뛰지 않습니다.
4. 상태 패널을 연 채로 볼륨 키를 누릅니다. 패널 슬라이더가 즉시 움직입니다.
5. 패널 슬라이더를 천천히 끕니다. 손잡이가 손을 따라오기만 하고 되돌아가거나 떨리지 않습니다.
6. 음소거 키를 누릅니다. 아이콘이 즉시 음소거 모양으로 바뀝니다.
7. 볼륨 위젯을 껐다 켭니다. 알림이 다시 붙어 2번이 여전히 동작합니다.
8. 소리 설정에서 기본 출력 장치를 바꿉니다. 새 장치에서도 2번이 동작합니다.
9. bamti를 종료합니다. 로그에 `worker stop`이 남고 프로세스가 남지 않습니다.
10. 볼륨 위젯을 켠 채 30분 두고 작업 관리자에서 bamti의 CPU를 봅니다. 고치기 전과 같거나 낮습니다.

8번과 9번은 수명 문제를 잡는 항목이므로 건너뛰지 마십시오.

---

## 8. 커밋

```
perf: 볼륨 변화를 폴링이 아니라 알림으로 받는다
```

측정 기록은 커밋 본문이나 별도 문서에 남기십시오.

---

## 9. 측정 기록

2026-09-01, Release `build/Release/bamti.exe`. 키보드 볼륨 키(`VK_VOLUME_UP`)로 시스템 볼륨을 바꿨습니다.

| 항목 | 값 |
|---|---|
| `volume notify -> sample` | **0.04 ms** (`bamti.log` 2026-09-01 21:18:35.711) |
| 5ms 초과 여부 | 아니오 |
| `volume read took` | 0.73 ms (같은 세션, 21:17:46.156) |
| `kVolumePeriodMs` | 1000 (변경 없음) |
| `kNotifyCoalesceMs` | 50 (변경 없음) |
| 종료 | `worker stop` 21:26:33.804, 프로세스 잔류 없음 |

이벤트 컨텍스트 GUID는 `New-Guid`로 만든 `8b1318eb-4c3e-461b-a46b-44bd420555ea`입니다.
