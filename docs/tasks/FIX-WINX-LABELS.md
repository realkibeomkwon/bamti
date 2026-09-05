# 작업 지시서: 빠른 링크 메뉴의 제어판 항목 이름을 실제 대상에 맞춘다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`TASK-BAR-WINX-MENU.md`(커밋 `347595a`)의 후속입니다. 건드리는 파일은 `src/winx_menu.cpp`와 `src/menu_bar.cpp`입니다.

---

## 1. 무엇이 문제인가

빠른 링크 메뉴의 `4 - Control Panel.lnk` 항목이 **`제어판`으로 표시되는데 실제로는 설정 앱이 열립니다.** 이름과 동작이 어긋납니다.

Windows 자신의 Win+X 메뉴는 같은 자리에 `설정`이라고 적습니다. 지시서 3절에서 확정하지 못했던 자리이고, 이번에 측정으로 원인이 밝혀졌습니다.

**원인은 `desktop.ini`가 낡았다는 것입니다.** 셸이 이 파일에 매기는 표시 이름을 확인한 결과, `SHLoadIndirectString`이 돌려주는 값과 정확히 같았습니다.

```
G2  4 - Control Panel.lnk  =>  제어판
```

즉 지금 구현은 파일 시스템이 말하는 바를 **정확히** 옮기고 있습니다. 틀린 것은 구현이 아니라 `desktop.ini`의 자원 번호(`shell32.dll,-4161`)입니다. Windows 11이 이 항목의 대상을 설정 앱으로 바꾸면서 표시 이름은 화면에서만 갈아 끼웠고, 사용자 프로필 안의 `desktop.ini`는 그대로 남았습니다.

바로 가기의 대상을 확인하면 분명합니다. `TargetPath`가 비어 있고 셸 항목만 들어 있습니다.

```
4 - Control Panel.lnk    target='' args=''
```

---

## 2. 어떻게 고치는가

**한국어 문자열을 코드에 박지 마십시오.** 다른 언어의 Windows에서 깨집니다.

설정 앱의 표시 이름을 운영 체제에서 가져오면 어느 언어에서든 맞습니다. 이 컴퓨터에서 확인한 값입니다.

```
shell:AppsFolder\windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel
  =>  설정
```

`src/winx_menu.cpp`의 `IsControlPanelLnk(name)`이 참인 항목에 대해, `desktop.ini`에서 얻은 이름 대신 다음 순서로 이름을 정하십시오.

1. `SHCreateItemFromParsingName`에 위의 `shell:AppsFolder\...` 경로를 넘겨 `IShellItem`을 얻는다.
2. `GetDisplayName(SIGDN_NORMALDISPLAY)`로 표시 이름을 얻는다.
3. 성공하면 그 값을 쓴다. **실패하면 지금처럼 `desktop.ini`의 값을 그대로 쓴다.**

세 단계의 결과를 `Log(L"winx", L"settings label=%s hr=0x%08lx", ...)`로 남기십시오.

`CoTaskMemFree`로 `GetDisplayName`이 준 문자열을 반드시 놓아주십시오.

---

## 3. 함께 정리할 것

`src/menu_bar.cpp`의 `ShowStartContextMenu`에 반환값을 버리는 호출이 한 줄 남아 있습니다.

```cpp
  HibernateAvailable();
```

메뉴를 만드는 데 쓰이지 않고 로그만 남깁니다. 같은 함수를 `OpenBarSubmenu`가 전원 서브메뉴를 채울 때 이미 부르고 있으므로 **이 줄을 지우십시오.**

---

## 4. 고치지 않는 것

`desktop.ini`가 낡아서 화면과 다른 이름이 셋 더 있습니다.

| 파일 | 지금 나오는 이름 | Windows 화면 |
| --- | --- | --- |
| `10 - AppsAndFeatures.lnk` | 프로그램 및 기능 | 설치된 앱 |
| `09 - Mobility Center.lnk` | Windows 모바일 센터 | 모바일 센터 |
| `1 - Desktop.lnk` | 바탕 화면 | 데스크톱 |

**이 셋은 이번에 건드리지 마십시오.** 제어판과 달리 이름이 가리키는 대상과 어긋나지 않고, 운영 체제에서 새 이름을 가져올 경로를 찾지 못했습니다. 한국어를 코드에 박는 것 말고는 방법이 없으므로 사용자의 판단을 기다립니다.

---

## 5. 검증

1. Release 클린 빌드가 경고 없이 통과해야 합니다.
2. 시작 단추를 우클릭했을 때 그 자리가 `설정`으로 나와야 합니다.
3. 그 항목을 눌러 설정 앱이 열리는지 확인하십시오.
4. 2절의 로그에 `settings label=설정 hr=0x00000000`이 남아야 합니다.
5. 나머지 항목의 이름과 순서가 이전과 같아야 합니다.

레지스트리에 쓰는 확인 절차는 넣지 마십시오.
