# 작업 지시서: 독의 탐색기 아이콘이 트레이 경로로 새는 것을 막는다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

건드리는 파일은 `src/dock.cpp` 하나입니다. 바꾸는 줄은 다섯 줄 남짓입니다.

---

## 1. 증상

독의 탐색기 아이콘을 눌러도 탐색기 창이 뜨지 않습니다. 같은 시각에 `Win + E` 를 누르면 창이 정상적으로 뜨고, 다른 독 아이콘도 모두 정상입니다. 부팅한 지 오래되면 증상이 저절로 사라집니다.

## 2. 원인 (로그로 확인한 사실입니다)

`%USERPROFILE%\.bamti\bamti.log.old` 에 증상이 있던 시간대의 기록이 남아 있습니다.

```
2026-09-08 10:19:24.237 [dock] click index=3 running=1 hwnd=0000000000000000 name=Windows 탐색기 route=tray
2026-09-08 10:19:24.237 [tray] intercept invoke key=0x74A4267B74A0D032 uid=100 version=0 right=0 event=WM_LBUTTONDBLCLK owner=0x10414 posted=1 asfw=1
```

클릭이 `route=tray` 로 빠졌고, **같은 밀리초에 트레이 아이콘 하나가 대신 눌렸습니다.** 10:20:11, 10:20:19, 10:20:53, 10:22:57 에도 같은 짝이 반복됩니다. 증상이 사라진 10:36 이후의 클릭은 전부 `route=activate` 또는 `route=launch` 입니다.

경로를 고르는 곳은 `src/dock.cpp` 의 `Dock::RevealDockApp` 입니다.

```cpp
const wchar_t* Dock::RevealDockApp(const DockApp& app, bool* launched) {
  ...
  if (app.running && app.hwnd != nullptr) {
    ActivateHwnd(app.hwnd);
    return L"activate";
  }
  if (app.running && tray_invoke_ && tray_invoke_(app.exe_path)) {   // <- 여기
    return L"tray";
  }
  const bool ok = LaunchDockApp(app);
  ...
}
```

`tray_invoke_` 는 `TrayMirror::InvokeByExe`(`src/tray_mirror.cpp`) 이고, **트레이 항목의 소유 프로세스 이미지 경로가 같기만 하면 그 항목을 누릅니다.** 조건이 두 가지 겹치면서 탐색기에서만 사고가 납니다.

1. `explorer.exe` 는 셸 자신이므로 **언제나 실행 중**입니다. 열린 탐색기 창이 하나도 없으면 `running=1`, `hwnd=0` 이 되어 첫 분기를 지나 트레이 분기로 들어갑니다.
2. 셸이 띄우는 트레이 아이콘(안전하게 하드웨어 제거 같은 것)의 소유 프로세스도 `explorer.exe` 입니다. 그래서 **독의 "Windows 탐색기" 항목과 아무 관계가 없는 아이콘이 매칭됩니다.**

부팅한 지 오래되면 증상이 사라진 것은 그 트레이 아이콘이 사라져서 매칭이 실패하고 `route=launch` 로 떨어졌기 때문입니다. 즉 증상은 시간이 아니라 **그 시점에 셸이 트레이 아이콘을 갖고 있었는가**에 달려 있습니다.

## 3. 고칠 것

`src/dock.cpp` 의 익명 이름공간에 판정 함수를 하나 추가합니다. 놓을 자리는 이미 같은 판정을 인라인으로 하고 있는 `PathImpliesGenericIcon`(현재 411행 부근) 바로 뒤가 좋습니다.

```cpp
bool IsExplorerExe(const std::wstring& path) {
  if (path.empty()) {
    return false;
  }
  const wchar_t* name = PathFindFileNameW(path.c_str());
  return name != nullptr && _wcsicmp(name, L"explorer.exe") == 0;
}
```

그리고 `Dock::RevealDockApp` 의 트레이 분기에 조건과 주석을 넣습니다.

```cpp
  // explorer.exe 는 셸 자신이라 항상 실행 중이고, 셸이 띄운 트레이 아이콘까지
  // 이 독 항목의 것으로 잡힌다. 그 아이콘을 눌러도 탐색기 창은 열리지 않는다.
  if (app.running && !IsExplorerExe(app.exe_path) && tray_invoke_ && tray_invoke_(app.exe_path)) {
    return L"tray";
  }
```

`Dock::LoadIconBitmap`(현재 1851행 부근)에 같은 판정이 인라인으로 들어 있습니다. 중복이므로 새 함수로 바꿉니다. **이 두 줄만 바꾸고 그 아래 아이콘 적재 논리는 손대지 마십시오.**

```cpp
  // 바꾸기 전
  const wchar_t* exe_name = PathFindFileNameW(app.exe_path.c_str());
  const bool explorer = exe_name != nullptr && _wcsicmp(exe_name, L"explorer.exe") == 0;
  if (explorer && !app.exe_path.empty()) {

  // 바꾼 뒤
  if (IsExplorerExe(app.exe_path)) {
```

## 4. 검증

**빌드는 CMake 로 하십시오.** `bamti.vcxproj` 는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.

```
cmake -S D:\repos\bamti -B D:\repos\bamti\build
cmake --build D:\repos\bamti\build --config Release
```

확인은 아래 순서로 합니다. **화면에 클릭이나 키 입력을 합성하지 마십시오.** 눌러야 하는 것이 있으면 무엇을 누르면 되는지 정리해서 사용자에게 부탁하고, 판정은 로그로 하십시오.

1. bamti 를 재시작합니다. **새로 띄우기 전에 먼저 종료시켜야 합니다.** 단일 인스턴스이지만 뒤에 뜬 쪽이 양보하는 방식이라, 그냥 띄우면 새 프로세스가 스스로 끝나고 예전 바이너리가 계속 돌아갑니다. WMI 가 돌려주는 새 pid 는 재시작의 증거가 되지 못합니다.

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

2. 사용자에게 **탐색기 창을 모두 닫은 상태에서 독의 탐색기 아이콘을 눌러 달라고** 부탁합니다.
3. `%USERPROFILE%\.bamti\bamti.log` 에서 판정합니다.
   - `name=Windows 탐색기` 인 클릭 줄의 `route` 가 **`launch` 여야 합니다.** `tray` 가 다시 나오면 실패입니다.
   - 같은 시각에 `[tray] intercept invoke` 가 따라 나오면 안 됩니다.
4. 트레이에 상주하는 앱(카카오톡 등)은 창이 없을 때 여전히 `route=tray` 로 나와야 합니다. 이 경로 자체를 없앤 것이 아니라는 확인입니다.

증상이 재현되지 않는 시간대일 수 있습니다. 그때는 3번의 `route=launch` 확인까지만 하고, **트레이 아이콘이 없어서 원래 경로도 `launch` 였을 수 있다는 사실을 함께 보고하십시오.** 억지로 재현하려고 트레이 아이콘을 만들어 내지 않아도 됩니다.

## 5. 하지 말 것

- `TrayMirror::InvokeByExe` 를 고치지 마십시오. 다른 앱의 트레이 복귀가 여기에 걸려 있습니다.
- 트레이 경로 자체를 삭제하지 마십시오. 탐색기만 예외로 두는 것이 이번 지시입니다.
- `LaunchDockApp` 이나 `TryDockLaunch` 의 순서를 바꾸지 마십시오. 실행 경로는 정상이었습니다.
- `app.running` 의 계산 방식을 바꾸지 마십시오.
- `src/dock.cpp` 외의 파일을 건드리지 마십시오.
- 이 지시서에 적히지 않은 정리나 개선을 함께 넣지 마십시오.
