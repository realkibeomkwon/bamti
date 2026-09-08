# 작업 지시서: 트레이 아이콘 순서가 재실행 후에도 유지되게 한다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

**`FIX-TRAY-HIDDEN-STABLE-KEY.md`(커밋 `8db755e`)를 끝낸 뒤에 하십시오.** 거기에서 만든 `TrayMirror::StableKey` 와 `TrayMirror::StableForKey` 를 그대로 씁니다. 새로 만들 식별자 규칙은 없습니다.

건드리는 파일은 `src/menu_bar.cpp` 와 `src/menu_bar.hpp` 두 개입니다.

---

## 1. 무엇이 잘못되었는가

상단바에서 트레이 아이콘을 Ctrl 을 누른 채 끌어 순서를 바꾸어도, 그 앱이나 bamti 를 다시 실행하면 순서가 원래대로 돌아갑니다.

앞선 작업에서 고친 숨김 목록과 **원인이 같습니다.** `bar_order` 에는 `StatusItem::id` 가 그대로 저장되는데, 트레이 항목의 그 값은 `TrayMirror::MakeId` 가 만든 `bamti.tray/<런타임 키 16진수>` 이고, 런타임 키는 창 핸들에서 나오기 때문에 프로세스가 다시 뜰 때마다 달라집니다.

실제로 저장된 값입니다.

```
"bar_order": ["bamti.widget/control_center", ..., "bamti.tray/4326aab04e92a79d", ...]
```

이 `4326aab04e92a79d` 는 카카오톡의 창 핸들이 `0x60818` 이던 시절의 값입니다. 카카오톡을 다시 실행하면 핸들이 `0x80FE2`, `0xA0F6C` 로 바뀌고 키도 `0x7d02465ca495e498`, `0xb4e3d0f39cebc8e8` 로 완전히 달라집니다. 그러면 `MenuBar::OrderedItems` 의 `items[i].id == id` 비교가 실패하고, 그 아이콘은 순서 목록에 없는 항목으로 취급되어 뒤로 밀립니다.

숨김 목록과 달리 이쪽은 **아직 고치지 않았습니다.** 앞 작업에서 범위를 좁혀 두었고, 이번에 이어서 합니다.

## 2. 어떻게 고치는가

`bar_order` 에 담기는 값을 **저장 형태**로 통일합니다. 트레이 항목만 `tray:` 접두사를 붙인 안정 식별자로 적고, 위젯 항목(`bamti.widget/...`)과 외부 항목(`mana/demo`)은 이미 안정적이므로 그대로 둡니다.

```
"bamti.tray/4326aab04e92a79d"   →   "tray:exe:kakaotalk.exe#222"
"bamti.widget/battery"          →   그대로
```

런타임 식별자가 나타나는 지점은 두 곳뿐입니다. `StatusItem::id` 와 `BarSegment::id` 입니다. **그 두 곳에서만 변환하면** 나머지 코드는 저장 형태로 일관되게 돌아갑니다. 특히 `ApplyVisiblePermutation`(`src/menu_bar.cpp:187`) 은 두 목록을 문자열로 직접 비교하므로, 넘기기 전에 정규화해 두면 함수 자체는 손댈 필요가 없습니다.

## 3. 고칠 것

### 3.1 정규화 함수를 더한다

`src/menu_bar.hpp` 의 비공개부에 선언하고 `src/menu_bar.cpp` 에 정의합니다.

```cpp
  // 순서 목록에 적는 형태로 바꾼다. 트레이 항목의 런타임 아이디는 창 핸들에서
  // 나오므로 재실행마다 달라진다. 위젯과 외부 항목은 이미 안정적이라 그대로 둔다.
  std::string OrderKey(const std::string& id) const;
```

```cpp
std::string MenuBar::OrderKey(const std::string& id) const {
  const uint64_t key = TrayMirror::ParseId(id);
  if (key == 0) {
    return id;
  }
  const std::string stable = tray_.StableForKey(key);
  if (stable.empty()) {
    return id;
  }
  return "tray:" + stable;
}
```

`TrayMirror::ParseId` 는 `bamti.tray/` 로 시작하지 않으면 0 을 돌려주므로, 위젯 항목은 첫 번째 조건에서 그대로 빠져나갑니다. 이미 `tray:` 로 시작하는 값도 마찬가지입니다.

`stable` 이 비었을 때 원래 값을 돌려주는 것이 중요합니다. 아직 열거되지 않은 아이콘이나 식별할 재료가 없는 아이콘을 빈 문자열로 뭉뚱그리면, 서로 다른 항목이 같은 자리를 다투게 됩니다.

### 3.2 순서를 맞추는 비교를 정규화한다

`MenuBar::OrderedItems`(`src/menu_bar.cpp:1853`) 의 이중 반복문을 고칩니다.

**`OrderKey` 를 안쪽 반복문에서 부르지 마십시오.** `TrayMirror::StableForKey` 는 뮤텍스를 잠그는데, 이 함수는 배치를 다시 계산할 때마다 불립니다. 안쪽에서 부르면 항목 수의 제곱만큼 잠금이 일어납니다. 반복문에 들어가기 전에 한 번씩만 계산해 두십시오.

```cpp
  std::vector<StatusItem> out;
  out.reserve(items.size());
  std::vector<char> used(items.size(), 0);
  std::vector<std::string> item_keys;
  item_keys.reserve(items.size());
  for (const StatusItem& item : items) {
    item_keys.push_back(OrderKey(item.id));
  }
  for (const std::string& id : order) {
    for (size_t i = 0; i < items.size(); ++i) {
      if (!used[i] && item_keys[i] == id) {
        used[i] = 1;
        out.push_back(std::move(items[i]));
        break;
      }
    }
  }
```

아래쪽의 `IsChromeItemId(items[i].id)` 분기는 그대로 두십시오. 크롬 항목의 아이디는 변환 대상이 아닙니다.

### 3.3 끌기를 시작할 때 빠진 항목을 저장 형태로 넣는다

`MenuBar::BeginReorder`(`src/menu_bar.cpp:1911` 부터) 를 고칩니다. `reorder_order_` 는 `bar_order_` 를 복사한 것이므로 저장 형태입니다. 여기에 런타임 아이디를 그대로 넣으면 형식이 섞입니다.

```cpp
  for (const StatusItem& item : chrome) {
    const std::string key = OrderKey(item.id);
    if (!OrderContains(reorder_order_, key)) {
      missing_chrome.push_back(key);
    }
  }
```

```cpp
  for (const StatusItem& item : snap) {
    const std::string key = OrderKey(item.id);
    if (!OrderContains(reorder_order_, key)) {
      reorder_order_.push_back(key);
    }
  }
```

### 3.4 끌고 있는 동안의 배열도 저장 형태로 맞춘다

`MenuBar::UpdateReorder`(`src/menu_bar.cpp:1975` 근처) 를 고칩니다. `visual_ltr` 은 화면 세그먼트에서 모은 런타임 아이디입니다. `ApplyVisiblePermutation` 에 넘기기 전에 변환하십시오.

```cpp
  std::vector<std::string> visual_rtl(visual_ltr.rbegin(), visual_ltr.rend());
  for (std::string& one : visual_rtl) {
    one = OrderKey(one);
  }
  std::vector<std::string> next = ApplyVisiblePermutation(reorder_order_, visual_rtl);
```

**`ApplyVisiblePermutation` 자체는 고치지 마십시오.** 양쪽 목록이 같은 형태로 들어오면 지금 코드 그대로 맞아떨어집니다.

같은 함수 안의 `slots[i]->id == reorder_id_`(1945행) 는 **그대로 두십시오.** 여기는 화면 세그먼트의 아이디와 끌고 있는 항목의 아이디를 견주는 자리라서, 양쪽 모두 런타임 아이디입니다.

### 3.5 저장 경로는 손대지 않는다

`bar_order_` 가 항상 저장 형태를 담게 되므로, 설정을 기록하는 두 경로가 저절로 올바른 값을 씁니다. 확인만 하고 넘어가십시오.

- `MenuBar::ApplySettings` 의 `merged.bar_order = bar_order_;`(`src/menu_bar.cpp:2777`) 는 `BuiltinWidgets::SetSettings` 를 거쳐 비동기로 저장됩니다.
- `MenuBar::FinishReorder` 의 `next.bar_order = bar_order_;`(`src/menu_bar.cpp:2024`) 는 곧바로 저장합니다.

## 4. 검증

**빌드는 CMake 로 하십시오.** `bamti.vcxproj` 는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.

```
cmake -S D:\repos\bamti -B D:\repos\bamti\build
cmake --build D:\repos\bamti\build --config Release
```

**bamti 는 새로 띄우기 전에 먼저 종료시켜야 합니다.** 단일 인스턴스이지만 뒤에 뜬 쪽이 양보하는 방식이라, 그냥 띄우면 새 프로세스가 스스로 끝나고 예전 바이너리가 계속 돌아갑니다. WMI 가 돌려주는 새 pid 는 재시작의 증거가 되지 못합니다.

상단바 창(클래스 `bamti.MenuBar`)에 `WM_COMMAND` 로 명령 1번을 보내면 `DestroyWindow` 를 거쳐 작업 표시줄과 작업 영역까지 되돌린 뒤 끝납니다.

```powershell
Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class Bar {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static IntPtr Find() {
    IntPtr f = IntPtr.Zero;
    EnumWindows((h, p) => { var sb = new StringBuilder(256); GetClassNameW(h, sb, 256);
      if (sb.ToString() == "bamti.MenuBar") { f = h; return false; } return true; }, IntPtr.Zero);
    return f;
  }
}
'@ -Language CSharp
$h = [Bar]::Find()
if ($h -ne [IntPtr]::Zero) { [void][Bar]::PostMessageW($h, 0x0111, [IntPtr]1, [IntPtr]0) }
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Process bamti -EA SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
"종료 후 남은 프로세스: " + $(if (Get-Process bamti -EA SilentlyContinue) { (Get-Process bamti).Id -join ',' } else { '없음' })
```

프로세스가 사라진 것을 확인한 뒤에 새 바이너리를 WMI 로 띄웁니다. 세션이 끝나도 살아 있어야 하므로 `Start-Process` 를 쓰지 마십시오.

```powershell
$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
  CommandLine = '"D:\repos\bamti\build\Release\bamti.exe"'
}
"rc=$($r.ReturnValue) pid=$($r.ProcessId)"
```

재시작이 실제로 일어났는지는 로그로 판정하십시오. `[host] start` 줄이 새로 늘어야 합니다.

```powershell
Select-String -Path "$env:USERPROFILE\.bamti\bamti.log" -Pattern '\[host\] start ' | Select-Object -Last 2
```

**화면에 클릭이나 키 입력을 합성하지 마십시오.** 아래의 1번과 3번은 사용자에게 부탁하고 결과를 받으십시오. 설정 파일을 읽는 것과 화면을 캡처해서 위치를 재는 것은 괜찮습니다.

검증 대상으로는 **카카오톡**을 쓰십시오. GUID 없이 아이콘을 등록해서(`guid=0`) 창 핸들 기반 키를 쓰는, 이번 문제의 정확한 조건입니다. 앞선 작업의 검증에서 숨겨 두었을 수 있으니, 트레이 메뉴에서 다시 체크해 보이게 만든 뒤에 시작하십시오.

1. 사용자에게 상단바에서 Ctrl 을 누른 채 카카오톡 아이콘을 다른 자리로 끌어 달라고 부탁합니다.

2. 설정 파일에 저장 형태로 기록되었는지 확인합니다.

   ```powershell
   $j = Get-Content "$env:USERPROFILE\.bamti\settings.json" -Raw | ConvertFrom-Json
   $j.topbar.widgets.bar_order -join ', '
   ```

   `tray:exe:kakaotalk.exe#222` 같은 항목이 있어야 합니다. `bamti.tray/` 로 시작하는 16진수 형태면 3.3 이나 3.4 가 덜 된 것입니다.

3. **여기가 이 작업의 핵심 판정입니다.** 사용자에게 카카오톡을 완전히 종료했다가 다시 실행해 달라고 부탁한 뒤(그래야 창 핸들이 바뀝니다), bamti 를 WMI 로 다시 띄웁니다. 카카오톡 아이콘이 2번에서 옮겨 둔 자리에 그대로 있어야 합니다.

   자리를 눈으로만 보지 말고 픽셀로 재십시오. 카카오톡 아이콘은 선명한 노란색이라 색으로 특정할 수 있습니다. 상단바 창은 `FindWindowW` 로 잡히지 않으므로 `EnumWindows` 로 클래스 이름 `bamti.MenuBar` 를 찾아 `GetWindowRect` 로 사각형을 얻고, 그 영역을 `CopyFromScreen` 으로 캡처한 뒤 `R>200 && G>170 && B<90` 인 픽셀의 x 범위를 재면 됩니다. 옮기기 전과 옮긴 뒤의 x 범위를 비교하십시오.

   로그에서 카카오톡의 창 핸들이 실제로 바뀌었는지도 함께 확인하십시오.

   ```powershell
   Select-String -Path "$env:USERPROFILE\.bamti\bamti.log" -Pattern 'intercept item tip="KakaoTalk"' | Select-Object -Last 3
   ```

4. 끌기 도중의 동작이 예전과 같은지 확인합니다. 아이콘을 끌 때 다른 아이콘들이 자리를 비켜 주어야 하고, 놓은 자리에 그대로 있어야 합니다. 3.4 에서 형식이 어긋나면 끌어도 아무 일이 일어나지 않거나 항목이 사라집니다.

5. 위젯 항목(배터리, 볼륨, 네트워크)의 순서도 함께 바꿔 보고, 재실행 후 유지되는지 확인합니다. 이쪽은 예전에도 유지되던 동작이라 회귀가 없어야 합니다.

6. CPU 사용률이 예전과 같은지 확인합니다. 3.2 의 미리 계산을 빠뜨리고 안쪽 반복문에서 `OrderKey` 를 부르면 여기서 드러납니다.

## 5. 하지 말 것

- **`ApplyVisiblePermutation`(`src/menu_bar.cpp:187`) 을 고치지 마십시오.** 넘기는 두 목록의 형태를 맞추는 것으로 충분합니다.
- `UpdateReorder` 의 `slots[i]->id == reorder_id_` 비교를 바꾸지 마십시오. 그 자리는 양쪽 모두 런타임 아이디입니다.
- `TrayMirror::MakeId` 와 `TrayMirror::ParseId` 를 고치지 마십시오. 런타임 아이디는 좌표 조회와 클릭 전달에 쓰입니다. 이번 작업은 **순서 목록에 적는 값만** 바꿉니다.
- 옛 `bamti.tray/<16진수>` 항목을 새 형태로 옮기는 마이그레이션을 넣지 마십시오. 옛 값에서 실행 파일 이름을 되찾을 방법이 없습니다. 그 항목들은 매칭되지 않아 뒤로 밀리고, 사용자가 한 번 다시 정렬하면 새 형태로 저장됩니다.
- `kBarOrderMax` 를 바꾸지 마십시오.
- 숨김 목록(`tray_hidden`)과 관련된 코드를 함께 손대지 마십시오. 앞 작업에서 끝난 부분입니다.
- 이 지시서에 적히지 않은 정리나 개선을 함께 넣지 마십시오.
