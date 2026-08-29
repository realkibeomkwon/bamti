#include "tray_probe.hpp"

#include "log.hpp"
#include "paths.hpp"

#include <windows.h>

#include <commctrl.h>
#include <oleauto.h>
#include <uiautomation.h>
#include <wincodec.h>
#include <wow64apiset.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "oleaut32")

namespace bamti {
namespace {

constexpr UINT kSmtoFlags = SMTO_ABORTIFHUNG;
constexpr UINT kSmtoMs = 200;
constexpr UINT kTreeDepthMax = 4;
constexpr UINT kTreeNodeMax = 500;
constexpr UINT kTreeMsMax = 5000;
constexpr UINT kButtonDumpMax = 128;
constexpr UINT kUiaDepthMax = 6;
constexpr UINT kUiaNodeMax = 300;
constexpr UINT kUiaMsMax = 10000;
constexpr UINT kCaptureMsMax = 5000;
constexpr UINT kRenderFullContent = 0x00000002;  // PW_RENDERFULLCONTENT
constexpr SIZE_T kRemoteBytes = 4096;
constexpr SIZE_T kRemoteTextOff = 256;
constexpr wchar_t kSingletonMutex[] = L"Local\\bamti.singleton";

constexpr const wchar_t* kTopClasses[] = {
    L"Shell_TrayWnd",
    L"Shell_SecondaryTrayWnd",
    L"NotifyIconOverflowWindow",
    L"TopLevelWindowForOverflowXamlIsland",
};

struct Report {
  std::wstring text;

  void Line(const wchar_t* fmt, ...) {
    wchar_t buf[4096]{};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    text += buf;
    text += L'\n';
  }
};

struct ToolbarRef {
  HWND hwnd = nullptr;
  std::wstring path;
  DWORD pid = 0;
};

struct TopLevel {
  const wchar_t* class_name = nullptr;
  HWND hwnd = nullptr;
};

struct HwndClass {
  HWND hwnd = nullptr;
  std::wstring cls;
};

struct Probe {
  Report report;
  std::vector<TopLevel> tops;
  std::vector<ToolbarRef> toolbars;
  std::vector<HwndClass> tree_hwnds;
  HWND first_tray = nullptr;
  HWND notify_area = nullptr;
  DWORD toolbar_count = 0;
  std::wstring toolbar_buttons;
  int uia_candidates = 0;
  BOOL printwindow_ok = FALSE;
  int capture_w = 0;
  int capture_h = 0;
  double non_black_pct = 0.0;
  double alpha_nz_pct = 0.0;
  bool capture_visible = false;
  bool capture_done = false;
  std::wstring os_line;
  bool bamti_resident = false;
  bool elevated = false;
  std::wstring wow_line;
};

struct TrayItemData {
  HWND hwnd;
  UINT uID;
  UINT uCallbackMessage;
  DWORD reserved[2];
  HICON hIcon;
};

struct CaptureJob {
  HWND hwnd = nullptr;
  HDC dc = nullptr;
  UINT flags = 0;
  BOOL ok = FALSE;
};

struct Handle {
  HANDLE value = nullptr;
  ~Handle() {
    if (value != nullptr && value != INVALID_HANDLE_VALUE) {
      CloseHandle(value);
    }
  }
  Handle() = default;
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};

struct RemoteMem {
  HANDLE process = nullptr;
  LPVOID addr = nullptr;
  ~RemoteMem() {
    if (process != nullptr && addr != nullptr) {
      VirtualFreeEx(process, addr, 0, MEM_RELEASE);
    }
  }
  RemoteMem() = default;
  RemoteMem(const RemoteMem&) = delete;
  RemoteMem& operator=(const RemoteMem&) = delete;
};

using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);

unsigned long long HwndU64(HWND hwnd) {
  return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd));
}

unsigned long long PtrU64(const void* p) {
  return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p));
}

DWORD HandleCount() {
  DWORD count = 0;
  GetProcessHandleCount(GetCurrentProcess(), &count);
  return count;
}

bool TimedOut(ULONGLONG start, UINT limit_ms) {
  return GetTickCount64() - start >= limit_ms;
}

const wchar_t* YesNo(bool v) {
  return v ? L"yes" : L"no";
}

std::wstring Sanitize(const wchar_t* s, size_t max_chars) {
  std::wstring out;
  if (s == nullptr) {
    return out;
  }
  for (size_t i = 0; s[i] != 0 && out.size() < max_chars; ++i) {
    const wchar_t c = s[i];
    if (c == L'\n' || c == L'\r' || c == L'\t') {
      out.push_back(L' ');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::wstring Quoted(const std::wstring& s) {
  std::wstring out = L"\"";
  out += s;
  out += L'"';
  return out;
}

std::wstring ClassOf(HWND hwnd) {
  wchar_t cls[256]{};
  if (hwnd == nullptr || GetClassNameW(hwnd, cls, 256) <= 0) {
    return L"?";
  }
  return cls;
}

std::wstring TitleOf(HWND hwnd) {
  wchar_t title[256]{};
  SetLastError(ERROR_SUCCESS);
  DWORD_PTR written = 0;
  if (SendMessageTimeoutW(hwnd, WM_GETTEXT, 256, reinterpret_cast<LPARAM>(title), kSmtoFlags, kSmtoMs, &written) ==
      0) {
    const DWORD err = GetLastError();
    wchar_t fail[64]{};
    swprintf_s(fail, L"(timeout err=%lu)", err);
    return fail;
  }
  return Sanitize(title, 200);
}

const wchar_t* MachineName(USHORT machine) {
  switch (machine) {
    case IMAGE_FILE_MACHINE_UNKNOWN:
      return L"UNKNOWN";
    case IMAGE_FILE_MACHINE_I386:
      return L"I386";
    case IMAGE_FILE_MACHINE_AMD64:
      return L"AMD64";
#ifdef IMAGE_FILE_MACHINE_ARM64
    case IMAGE_FILE_MACHINE_ARM64:
      return L"ARM64";
#endif
    default:
      return L"?";
  }
}

const wchar_t* ControlTypeName(CONTROLTYPEID id) {
  switch (id) {
    case UIA_ButtonControlTypeId:
      return L"Button";
    case UIA_CalendarControlTypeId:
      return L"Calendar";
    case UIA_CheckBoxControlTypeId:
      return L"CheckBox";
    case UIA_ComboBoxControlTypeId:
      return L"ComboBox";
    case UIA_EditControlTypeId:
      return L"Edit";
    case UIA_HyperlinkControlTypeId:
      return L"Hyperlink";
    case UIA_ImageControlTypeId:
      return L"Image";
    case UIA_ListItemControlTypeId:
      return L"ListItem";
    case UIA_ListControlTypeId:
      return L"List";
    case UIA_MenuControlTypeId:
      return L"Menu";
    case UIA_MenuBarControlTypeId:
      return L"MenuBar";
    case UIA_MenuItemControlTypeId:
      return L"MenuItem";
    case UIA_ProgressBarControlTypeId:
      return L"ProgressBar";
    case UIA_RadioButtonControlTypeId:
      return L"RadioButton";
    case UIA_ScrollBarControlTypeId:
      return L"ScrollBar";
    case UIA_SliderControlTypeId:
      return L"Slider";
    case UIA_SpinnerControlTypeId:
      return L"Spinner";
    case UIA_StatusBarControlTypeId:
      return L"StatusBar";
    case UIA_TabControlTypeId:
      return L"Tab";
    case UIA_TabItemControlTypeId:
      return L"TabItem";
    case UIA_TextControlTypeId:
      return L"Text";
    case UIA_ToolBarControlTypeId:
      return L"ToolBar";
    case UIA_ToolTipControlTypeId:
      return L"ToolTip";
    case UIA_TreeControlTypeId:
      return L"Tree";
    case UIA_TreeItemControlTypeId:
      return L"TreeItem";
    case UIA_CustomControlTypeId:
      return L"Custom";
    case UIA_GroupControlTypeId:
      return L"Group";
    case UIA_ThumbControlTypeId:
      return L"Thumb";
    case UIA_DataGridControlTypeId:
      return L"DataGrid";
    case UIA_DataItemControlTypeId:
      return L"DataItem";
    case UIA_DocumentControlTypeId:
      return L"Document";
    case UIA_SplitButtonControlTypeId:
      return L"SplitButton";
    case UIA_WindowControlTypeId:
      return L"Window";
    case UIA_PaneControlTypeId:
      return L"Pane";
    case UIA_HeaderControlTypeId:
      return L"Header";
    case UIA_HeaderItemControlTypeId:
      return L"HeaderItem";
    case UIA_TableControlTypeId:
      return L"Table";
    case UIA_TitleBarControlTypeId:
      return L"TitleBar";
    case UIA_SeparatorControlTypeId:
      return L"Separator";
    default:
      return L"?";
  }
}

std::wstring BstrTake(BSTR s) {
  if (s == nullptr) {
    return {};
  }
  std::wstring out = Sanitize(s, 200);
  SysFreeString(s);
  return out;
}

bool PatternAvailable(IUIAutomationElement* el, PROPERTYID id) {
  VARIANT v;
  VariantInit(&v);
  bool yes = false;
  if (SUCCEEDED(el->GetCurrentPropertyValue(id, &v))) {
    if (v.vt == VT_BOOL) {
      yes = v.boolVal == VARIANT_TRUE;
    } else if (v.vt == VT_I4) {
      yes = v.lVal != 0;
    }
  }
  VariantClear(&v);
  return yes;
}

void AttachParentConsole() {
  if (AttachConsole(ATTACH_PARENT_PROCESS) == FALSE) {
    return;
  }
  FILE* out = nullptr;
  freopen_s(&out, "CONOUT$", "w", stdout);
}

bool BamtiResident() {
  const HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kSingletonMutex);
  if (mutex == nullptr) {
    return false;
  }
  CloseHandle(mutex);
  return true;
}

bool TokenElevated() {
  Handle token;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) == FALSE) {
    return false;
  }
  TOKEN_ELEVATION elev{};
  DWORD len = 0;
  if (GetTokenInformation(token.value, TokenElevation, &elev, sizeof(elev), &len) == FALSE) {
    return false;
  }
  return elev.TokenIsElevated != 0;
}

void CollectTopLevel(const wchar_t* class_name, Probe& probe) {
  HWND prev = nullptr;
  int n = 0;
  while ((prev = FindWindowExW(nullptr, prev, class_name, nullptr)) != nullptr) {
    probe.tops.push_back({class_name, prev});
    ++n;
    if (n >= 32) {
      probe.report.Line(L"  (FindWindowExW 32개 상한, class=%s)", class_name);
      break;
    }
  }
}

void MaybeNotifyArea(HWND hwnd, const std::wstring& cls, HWND root, Probe& probe) {
  if (root != probe.first_tray || probe.first_tray == nullptr) {
    return;
  }
  if (cls == L"TrayNotifyWnd") {
    probe.notify_area = hwnd;
    return;
  }
  if (probe.notify_area != nullptr) {
    return;
  }
  if (cls.find(L"SystemTray") != std::wstring::npos) {
    probe.notify_area = hwnd;
  }
}

void DumpWindow(HWND hwnd, int depth, const std::wstring& parent_path, HWND root, Probe& probe, UINT& nodes,
                const ULONGLONG start) {
  if (nodes >= kTreeNodeMax || TimedOut(start, kTreeMsMax)) {
    return;
  }
  ++nodes;

  const std::wstring cls = ClassOf(hwnd);
  const std::wstring title = TitleOf(hwnd);
  const unsigned style = static_cast<unsigned>(GetWindowLongPtrW(hwnd, GWL_STYLE) & 0xFFFFFFFFu);
  const unsigned exstyle = static_cast<unsigned>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & 0xFFFFFFFFu);
  RECT rc{};
  GetWindowRect(hwnd, &rc);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  const bool visible = IsWindowVisible(hwnd) != FALSE;
  const std::wstring path = parent_path.empty() ? cls : parent_path + L" > " + cls;

  probe.report.Line(L"%u  0x%llX  %s  %s  0x%08X  0x%08X  rect(%ld,%ld,%ld,%ld)  %s  %lu", depth, HwndU64(hwnd),
                    cls.c_str(), Quoted(title).c_str(), style, exstyle, rc.left, rc.top, rc.right, rc.bottom,
                    YesNo(visible), pid);
  probe.tree_hwnds.push_back({hwnd, cls});

  if (cls == L"ToolbarWindow32") {
    probe.toolbars.push_back({hwnd, path, pid});
  }
  MaybeNotifyArea(hwnd, cls, root, probe);

  if (depth >= static_cast<int>(kTreeDepthMax)) {
    return;
  }

  HWND child = GetWindow(hwnd, GW_CHILD);
  while (child != nullptr) {
    if (nodes >= kTreeNodeMax || TimedOut(start, kTreeMsMax)) {
      return;
    }
    DumpWindow(child, depth + 1, path, root, probe, nodes, start);
    child = GetWindow(child, GW_HWNDNEXT);
  }
}

bool SendTb(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, DWORD_PTR* result, DWORD* err) {
  SetLastError(ERROR_SUCCESS);
  *result = 0;
  if (SendMessageTimeoutW(hwnd, msg, wparam, lparam, kSmtoFlags, kSmtoMs, result) == 0) {
    *err = GetLastError();
    return false;
  }
  *err = 0;
  return true;
}

void WriteEnv(Probe& probe) {
  probe.report.Line(L"## 1. 환경");

  SYSTEMTIME local{};
  SYSTEMTIME utc{};
  GetLocalTime(&local);
  GetSystemTime(&utc);
  probe.report.Line(L"local=%04u-%02u-%02u %02u:%02u:%02u.%03u", local.wYear, local.wMonth, local.wDay, local.wHour,
                    local.wMinute, local.wSecond, local.wMilliseconds);
  probe.report.Line(L"utc=%04u-%02u-%02u %02u:%02u:%02u.%03u", utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute,
                    utc.wSecond, utc.wMilliseconds);

  OSVERSIONINFOW ver{};
  ver.dwOSVersionInfoSize = sizeof(ver);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto rtl = ntdll != nullptr ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))
                                    : nullptr;
  if (rtl == nullptr || rtl(&ver) != 0) {
    probe.report.Line(L"os=RtlGetVersion 실패");
    probe.os_line = L"unknown";
  } else {
    probe.report.Line(L"os=%lu.%lu.%lu platform=%lu", ver.dwMajorVersion, ver.dwMinorVersion, ver.dwBuildNumber,
                      ver.dwPlatformId);
    wchar_t os[64]{};
    swprintf_s(os, L"%lu.%lu.%lu", ver.dwMajorVersion, ver.dwMinorVersion, ver.dwBuildNumber);
    probe.os_line = os;
  }

#if defined(_M_X64)
  const wchar_t* compile_arch = L"x64";
#elif defined(_M_ARM64)
  const wchar_t* compile_arch = L"arm64";
#elif defined(_M_IX86)
  const wchar_t* compile_arch = L"x86";
#else
  const wchar_t* compile_arch = L"?";
#endif
  USHORT process_machine = 0;
  USHORT native_machine = 0;
  const BOOL wow_ok = IsWow64Process2(GetCurrentProcess(), &process_machine, &native_machine);
  if (wow_ok == FALSE) {
    probe.report.Line(L"arch compile=%s IsWow64Process2=fail err=%lu", compile_arch, GetLastError());
    probe.wow_line = L"IsWow64Process2=fail";
  } else {
    probe.report.Line(L"arch compile=%s process_machine=0x%04X(%s) native_machine=0x%04X(%s)", compile_arch,
                      process_machine, MachineName(process_machine), native_machine, MachineName(native_machine));
    wchar_t wow[128]{};
    swprintf_s(wow, L"process_machine=0x%04X native_machine=0x%04X", process_machine, native_machine);
    probe.wow_line = wow;
  }

  probe.elevated = TokenElevated();
  probe.report.Line(L"elevated=%s", YesNo(probe.elevated));

  probe.bamti_resident = BamtiResident();
  probe.report.Line(L"bamti_resident=%s mutex=%s", YesNo(probe.bamti_resident), kSingletonMutex);
  probe.report.Line(L"handles_start=%lu", HandleCount());
  probe.report.Line(L"");
}

void WriteTree(Probe& probe) {
  probe.report.Line(L"## 2. 창 트리");
  probe.report.Line(L"형식: depth  hwnd  class  title  style  exstyle  rect(l,t,r,b)  visible  pid");

  for (const wchar_t* cls : kTopClasses) {
    CollectTopLevel(cls, probe);
  }

  const ULONGLONG start = GetTickCount64();
  UINT nodes = 0;
  bool saw_tray = false;
  for (const TopLevel& top : probe.tops) {
    probe.report.Line(L"");
    probe.report.Line(L"### %s hwnd=0x%llX", top.class_name, HwndU64(top.hwnd));
    if (!saw_tray && lstrcmpW(top.class_name, L"Shell_TrayWnd") == 0) {
      probe.first_tray = top.hwnd;
      saw_tray = true;
    }
    DumpWindow(top.hwnd, 0, L"", top.hwnd, probe, nodes, start);
    if (nodes >= kTreeNodeMax) {
      probe.report.Line(L"중단: 노드 %u개 상한", kTreeNodeMax);
      break;
    }
    if (TimedOut(start, kTreeMsMax)) {
      probe.report.Line(L"중단: 시간 상한 %ums", kTreeMsMax);
      break;
    }
  }

  for (const wchar_t* cls : kTopClasses) {
    int n = 0;
    for (const TopLevel& top : probe.tops) {
      if (lstrcmpW(top.class_name, cls) == 0) {
        ++n;
      }
    }
    if (n == 0) {
      probe.report.Line(L"%s: 없음", cls);
    }
  }
  probe.report.Line(L"nodes=%u elapsed_ms=%llu", nodes, GetTickCount64() - start);
  probe.report.Line(L"");
}

void WriteToolbars(Probe& probe) {
  probe.report.Line(L"## 3. ToolbarWindow32");
  probe.report.Line(L"형식: hwnd  부모 경로  pid  버튼수  실패사유");
  probe.toolbar_count = static_cast<DWORD>(probe.toolbars.size());
  if (probe.toolbars.empty()) {
    probe.report.Line(L"ToolbarWindow32: 없음");
    probe.report.Line(L"");
    return;
  }

  for (ToolbarRef& tb : probe.toolbars) {
    DWORD_PTR count = 0;
    DWORD err = 0;
    if (!SendTb(tb.hwnd, TB_BUTTONCOUNT, 0, 0, &count, &err)) {
      probe.report.Line(L"0x%llX  %s  %lu  -  timeout_or_fail err=%lu", HwndU64(tb.hwnd), tb.path.c_str(), tb.pid, err);
      if (!probe.toolbar_buttons.empty()) {
        probe.toolbar_buttons += L",";
      }
      probe.toolbar_buttons += L"fail";
      continue;
    }
    probe.report.Line(L"0x%llX  %s  %lu  %llu  -", HwndU64(tb.hwnd), tb.path.c_str(), tb.pid,
                      static_cast<unsigned long long>(count));
    if (!probe.toolbar_buttons.empty()) {
      probe.toolbar_buttons += L",";
    }
    wchar_t piece[32]{};
    swprintf_s(piece, L"%llu", static_cast<unsigned long long>(count));
    probe.toolbar_buttons += piece;
  }
  probe.report.Line(L"");
}

void DumpOneButton(HWND toolbar, HANDLE process, LPVOID remote, int idx, Probe& probe, int& passed) {
  BYTE* const remote_bytes = static_cast<BYTE*>(remote);
  LPVOID const remote_btn = remote;
  LPVOID const remote_text = remote_bytes + kRemoteTextOff;

  DWORD_PTR sent = 0;
  DWORD err = 0;
  if (!SendTb(toolbar, TB_GETBUTTON, static_cast<WPARAM>(idx), reinterpret_cast<LPARAM>(remote_btn), &sent, &err)) {
    probe.report.Line(L"idx=%d TB_GETBUTTON timeout_or_fail err=%lu", idx, err);
    return;
  }
  if (sent == 0) {
    probe.report.Line(L"idx=%d TB_GETBUTTON result=0", idx);
    return;
  }

  TBBUTTON btn{};
  SIZE_T read = 0;
  if (ReadProcessMemory(process, remote_btn, &btn, sizeof(btn), &read) == FALSE || read != sizeof(btn)) {
    probe.report.Line(L"idx=%d ReadProcessMemory(TBBUTTON) err=%lu read=%llu", idx, GetLastError(),
                      static_cast<unsigned long long>(read));
    return;
  }

  probe.report.Line(L"idx=%d  idCommand=%d  fsState=0x%02X  fsStyle=0x%02X  dwData=0x%llX", idx, btn.idCommand,
                    btn.fsState, btn.fsStyle, static_cast<unsigned long long>(btn.dwData));

  bool is_window = false;
  bool cb_ok = false;
  bool icon_ok = false;
  if (btn.dwData == 0) {
    probe.report.Line(L"     hwnd=0x0  IsWindow=no  ownerPid=  ownerClass=  ownerTitle=");
    probe.report.Line(L"     uID=  uCallbackMessage=  (WM_USER 이상인가: no)");
    probe.report.Line(L"     hIcon=0x0  GetIconInfo=fail  크기=");
  } else {
    TrayItemData data{};
    SIZE_T got = 0;
    if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(btn.dwData), &data, sizeof(data), &got) == FALSE ||
        got != sizeof(data)) {
      probe.report.Line(L"     ReadProcessMemory(TrayItemData) err=%lu read=%llu", GetLastError(),
                        static_cast<unsigned long long>(got));
    } else {
      is_window = IsWindow(data.hwnd) != FALSE;
      DWORD owner_pid = 0;
      GetWindowThreadProcessId(data.hwnd, &owner_pid);
      const std::wstring owner_cls = data.hwnd != nullptr ? ClassOf(data.hwnd) : std::wstring{};
      const std::wstring owner_title = data.hwnd != nullptr ? TitleOf(data.hwnd) : std::wstring{};
      probe.report.Line(L"     hwnd=0x%llX  IsWindow=%s  ownerPid=%lu  ownerClass=%s  ownerTitle=%s",
                        HwndU64(data.hwnd), YesNo(is_window), owner_pid, owner_cls.c_str(),
                        Quoted(owner_title).c_str());
      cb_ok = data.uCallbackMessage >= WM_USER;
      probe.report.Line(L"     uID=%u  uCallbackMessage=0x%X  (WM_USER 이상인가: %s)", data.uID, data.uCallbackMessage,
                        YesNo(cb_ok));

      ICONINFO ii{};
      icon_ok = data.hIcon != nullptr && GetIconInfo(data.hIcon, &ii) != FALSE;
      int iw = 0;
      int ih = 0;
      if (icon_ok) {
        BITMAP bm{};
        HBITMAP measure = ii.hbmColor != nullptr ? ii.hbmColor : ii.hbmMask;
        if (measure != nullptr && GetObjectW(measure, sizeof(bm), &bm) != 0) {
          iw = bm.bmWidth;
          ih = bm.bmHeight;
        }
        if (ii.hbmColor != nullptr) {
          DeleteObject(ii.hbmColor);
        }
        if (ii.hbmMask != nullptr) {
          DeleteObject(ii.hbmMask);
        }
      }
      if (icon_ok) {
        probe.report.Line(L"     hIcon=0x%llX  GetIconInfo=ok  크기=%dx%d", PtrU64(data.hIcon), iw, ih);
      } else {
        probe.report.Line(L"     hIcon=0x%llX  GetIconInfo=fail  크기=", PtrU64(data.hIcon));
      }
    }
  }

  const SIZE_T text_bytes = kRemoteBytes - kRemoteTextOff;
  std::vector<wchar_t> zeros(text_bytes / sizeof(wchar_t), 0);
  WriteProcessMemory(process, remote_text, zeros.data(), zeros.size() * sizeof(wchar_t), nullptr);
  DWORD_PTR text_len = 0;
  DWORD text_err = 0;
  std::wstring text;
  if (!SendTb(toolbar, TB_GETBUTTONTEXTW, static_cast<WPARAM>(btn.idCommand), reinterpret_cast<LPARAM>(remote_text),
              &text_len, &text_err)) {
    wchar_t fail[64]{};
    swprintf_s(fail, L"(timeout err=%lu)", text_err);
    text = fail;
  } else if (static_cast<LONG_PTR>(text_len) < 0) {
    text.clear();
  } else {
    const size_t chars = static_cast<size_t>(text_len) + 1;
    std::vector<wchar_t> buf(chars + 1, 0);
    SIZE_T got = 0;
    const SIZE_T want = (std::min)(buf.size() * sizeof(wchar_t), text_bytes);
    if (ReadProcessMemory(process, remote_text, buf.data(), want, &got) != FALSE) {
      buf.back() = 0;
      text = Sanitize(buf.data(), 200);
    }
  }
  probe.report.Line(L"     text=%s", Quoted(text).c_str());

  if (is_window && cb_ok && icon_ok) {
    ++passed;
  }
}

void WriteButtons(Probe& probe) {
  probe.report.Line(L"## 4. 버튼 데이터");
  probe.report.Line(L"sizeof(TBBUTTON)=%llu sizeof(TrayItemData)=%llu",
                    static_cast<unsigned long long>(sizeof(TBBUTTON)),
                    static_cast<unsigned long long>(sizeof(TrayItemData)));

  int total_passed = 0;
  bool any = false;
  for (const ToolbarRef& tb : probe.toolbars) {
    DWORD_PTR count = 0;
    DWORD err = 0;
    if (!SendTb(tb.hwnd, TB_BUTTONCOUNT, 0, 0, &count, &err) || count == 0) {
      continue;
    }
    any = true;
    probe.report.Line(L"");
    probe.report.Line(L"### toolbar 0x%llX buttons=%llu pid=%lu", HwndU64(tb.hwnd),
                      static_cast<unsigned long long>(count), tb.pid);

    Handle process;
    process.value = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
                                    PROCESS_QUERY_LIMITED_INFORMATION,
                                FALSE, tb.pid);
    if (process.value == nullptr) {
      probe.report.Line(L"OpenProcess err=%lu", GetLastError());
      continue;
    }
    RemoteMem remote;
    remote.process = process.value;
    remote.addr = VirtualAllocEx(process.value, nullptr, kRemoteBytes, MEM_COMMIT, PAGE_READWRITE);
    if (remote.addr == nullptr) {
      probe.report.Line(L"VirtualAllocEx err=%lu", GetLastError());
      continue;
    }

    const int n = static_cast<int>(count);
    const int limit = (std::min)(n, static_cast<int>(kButtonDumpMax));
    int passed = 0;
    for (int i = 0; i < limit; ++i) {
      DumpOneButton(tb.hwnd, process.value, remote.addr, i, probe, passed);
    }
    if (n > limit) {
      probe.report.Line(L"중단: 버튼 %u개 상한 (count=%d)", kButtonDumpMax, n);
    }
    probe.report.Line(L"검증 통과 버튼 수=%d / %d", passed, limit);
    total_passed += passed;
  }

  if (!any) {
    probe.report.Line(L"해당 없음");
  }
  probe.report.Line(L"검증 통과 버튼 수 합계=%d", total_passed);
  probe.report.Line(L"");
}

std::wstring ElementBstr(IUIAutomationElement* el, HRESULT (STDMETHODCALLTYPE IUIAutomationElement::*getter)(BSTR*)) {
  BSTR s = nullptr;
  if (FAILED((el->*getter)(&s))) {
    return {};
  }
  return BstrTake(s);
}

void WalkUia(IUIAutomationTreeWalker* walker, IUIAutomationElement* el, int depth, Probe& probe, UINT& nodes,
             const ULONGLONG start, std::vector<std::wstring>& names, bool* stopped) {
  if (*stopped || nodes >= kUiaNodeMax || TimedOut(start, kUiaMsMax)) {
    *stopped = true;
    return;
  }
  ++nodes;

  CONTROLTYPEID type = 0;
  el->get_CurrentControlType(&type);
  const std::wstring name = ElementBstr(el, &IUIAutomationElement::get_CurrentName);
  const std::wstring autoid = ElementBstr(el, &IUIAutomationElement::get_CurrentAutomationId);
  const std::wstring cls = ElementBstr(el, &IUIAutomationElement::get_CurrentClassName);
  RECT rc{};
  el->get_CurrentBoundingRectangle(&rc);
  BOOL offscreen = FALSE;
  el->get_CurrentIsOffscreen(&offscreen);
  const bool invoke = PatternAvailable(el, UIA_IsInvokePatternAvailablePropertyId);
  const bool legacy = PatternAvailable(el, UIA_IsLegacyIAccessiblePatternAvailablePropertyId);
  const bool expand = PatternAvailable(el, UIA_IsExpandCollapsePatternAvailablePropertyId);
  const long width = rc.right - rc.left;

  probe.report.Line(L"%d  %s(%d)  name=%s  AutomationId=%s  ClassName=%s  BoundingRectangle(%ld,%ld,%ld,%ld)  "
                    L"IsOffscreen=%s",
                    depth, ControlTypeName(type), static_cast<int>(type), Quoted(name).c_str(), Quoted(autoid).c_str(),
                    Quoted(cls).c_str(), rc.left, rc.top, rc.right, rc.bottom, YesNo(offscreen != FALSE));
  probe.report.Line(L"       patterns: Invoke=%s  LegacyIAccessible=%s  ExpandCollapse=%s", YesNo(invoke),
                    YesNo(legacy), YesNo(expand));

  if (type == UIA_ButtonControlTypeId && !name.empty() && width > 0) {
    ++probe.uia_candidates;
    names.push_back(name);
  }

  if (depth >= static_cast<int>(kUiaDepthMax)) {
    return;
  }

  Microsoft::WRL::ComPtr<IUIAutomationElement> child;
  if (FAILED(walker->GetFirstChildElement(el, child.GetAddressOf())) || child == nullptr) {
    return;
  }
  while (child != nullptr) {
    WalkUia(walker, child.Get(), depth + 1, probe, nodes, start, names, stopped);
    if (*stopped) {
      return;
    }
    Microsoft::WRL::ComPtr<IUIAutomationElement> next;
    if (FAILED(walker->GetNextSiblingElement(child.Get(), next.GetAddressOf()))) {
      break;
    }
    child = next;
  }
}

void WriteUia(Probe& probe) {
  probe.report.Line(L"## 5. UI Automation");
  Microsoft::WRL::ComPtr<IUIAutomation> uia;
  const HRESULT created =
      CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia.GetAddressOf()));
  if (FAILED(created) || uia == nullptr) {
    probe.report.Line(L"CoCreateInstance(CLSID_CUIAutomation) hr=0x%08X", static_cast<unsigned>(created));
    probe.report.Line(L"");
    return;
  }

  Microsoft::WRL::ComPtr<IUIAutomationTreeWalker> walker;
  const HRESULT walk_hr = uia->get_ControlViewWalker(walker.GetAddressOf());
  if (FAILED(walk_hr) || walker == nullptr) {
    probe.report.Line(L"get_ControlViewWalker hr=0x%08X", static_cast<unsigned>(walk_hr));
    probe.report.Line(L"");
    return;
  }

  const ULONGLONG start = GetTickCount64();
  UINT nodes = 0;
  bool stopped = false;
  std::vector<std::wstring> names;
  std::vector<HWND> walked;
  auto already = [&walked](HWND hwnd) {
    for (HWND seen : walked) {
      if (seen == hwnd) {
        return true;
      }
    }
    return false;
  };
  auto walk_hwnd = [&](HWND hwnd, const wchar_t* label) {
    if (stopped || hwnd == nullptr || already(hwnd)) {
      return;
    }
    walked.push_back(hwnd);
    probe.report.Line(L"");
    probe.report.Line(L"### %s hwnd=0x%llX", label, HwndU64(hwnd));
    Microsoft::WRL::ComPtr<IUIAutomationElement> root;
    const HRESULT from = uia->ElementFromHandle(hwnd, root.GetAddressOf());
    if (FAILED(from) || root == nullptr) {
      probe.report.Line(L"ElementFromHandle hr=0x%08X", static_cast<unsigned>(from));
      return;
    }
    WalkUia(walker.Get(), root.Get(), 0, probe, nodes, start, names, &stopped);
  };

  if (probe.tops.empty()) {
    probe.report.Line(L"최상위 창 없음");
  }
  for (const TopLevel& top : probe.tops) {
    walk_hwnd(top.hwnd, top.class_name);
  }
  // Shell_TrayWnd의 ControlView는 XAML 섬 HWND로 내려가지 않을 수 있어, 3-2에서 본 자식에도 붙는다.
  for (const HwndClass& node : probe.tree_hwnds) {
    walk_hwnd(node.hwnd, node.cls.c_str());
  }
  if (nodes >= kUiaNodeMax) {
    probe.report.Line(L"중단: 노드 %u개 상한", kUiaNodeMax);
  } else if (TimedOut(start, kUiaMsMax)) {
    probe.report.Line(L"중단: 시간 상한 %ums", kUiaMsMax);
  }
  probe.report.Line(L"nodes=%u elapsed_ms=%llu", nodes, GetTickCount64() - start);
  probe.report.Line(L"tray_icon_candidates=%d", probe.uia_candidates);
  for (const std::wstring& name : names) {
    probe.report.Line(L"  candidate_name=%s", Quoted(name).c_str());
  }
  probe.report.Line(L"");
}

DWORD WINAPI CaptureThread(LPVOID param) {
  auto* job = static_cast<CaptureJob*>(param);
  job->ok = PrintWindow(job->hwnd, job->dc, job->flags);
  return 0;
}

bool SavePng(const std::wstring& path, UINT width, UINT height, BYTE* bits) {
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
      factory == nullptr) {
    return false;
  }
  Microsoft::WRL::ComPtr<IWICStream> stream;
  if (FAILED(factory->CreateStream(stream.GetAddressOf()))) {
    return false;
  }
  if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
    return false;
  }
  Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()))) {
    return false;
  }
  if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
    return false;
  }
  Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
  if (FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr))) {
    return false;
  }
  if (FAILED(frame->Initialize(nullptr))) {
    return false;
  }
  if (FAILED(frame->SetSize(width, height))) {
    return false;
  }
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(frame->SetPixelFormat(&format))) {
    return false;
  }
  const UINT stride = width * 4;
  const UINT bytes = stride * height;
  if (FAILED(frame->WritePixels(height, stride, bytes, bits))) {
    return false;
  }
  if (FAILED(frame->Commit())) {
    return false;
  }
  if (FAILED(encoder->Commit())) {
    return false;
  }
  return true;
}

void CaptureOne(Probe& probe, HWND target, const wchar_t* label, const std::wstring* png_path, bool primary) {
  const std::wstring target_cls = ClassOf(target);
  const bool visible = IsWindowVisible(target) != FALSE;
  RECT rc{};
  GetWindowRect(target, &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  probe.report.Line(L"%s hwnd=0x%llX class=%s visible=%s rect(%ld,%ld,%ld,%ld)", label, HwndU64(target),
                    target_cls.c_str(), YesNo(visible), rc.left, rc.top, rc.right, rc.bottom);
  if (w <= 0 || h <= 0) {
    probe.report.Line(L"  사유: 캡처 크기 %dx%d", w, h);
    return;
  }

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (dib == nullptr || bits == nullptr) {
    probe.report.Line(L"  CreateDIBSection err=%lu", GetLastError());
    if (dib != nullptr) {
      DeleteObject(dib);
    }
    return;
  }
  HDC mem_dc = CreateCompatibleDC(nullptr);
  if (mem_dc == nullptr) {
    probe.report.Line(L"  CreateCompatibleDC err=%lu", GetLastError());
    DeleteObject(dib);
    return;
  }
  HGDIOBJ old = SelectObject(mem_dc, dib);

  CaptureJob job;
  job.hwnd = target;
  job.dc = mem_dc;
  job.flags = kRenderFullContent;
  Handle thread;
  thread.value = CreateThread(nullptr, 0, CaptureThread, &job, 0, nullptr);
  bool timed_out = false;
  if (thread.value == nullptr) {
    probe.report.Line(L"  CreateThread err=%lu", GetLastError());
    SelectObject(mem_dc, old);
    DeleteDC(mem_dc);
    DeleteObject(dib);
    return;
  }
  if (WaitForSingleObject(thread.value, kCaptureMsMax) == WAIT_TIMEOUT) {
    timed_out = true;
  }

  probe.report.Line(L"  PrintWindow=%s timeout=%s flags=0x%X size=%dx%d", YesNo(job.ok != FALSE), YesNo(timed_out),
                    kRenderFullContent, w, h);

  double non_black_pct = 0.0;
  double alpha_nz_pct = 0.0;
  if (!timed_out && bits != nullptr) {
    const auto* px = static_cast<const std::uint8_t*>(bits);
    const size_t total = static_cast<size_t>(w) * static_cast<size_t>(h);
    size_t non_black = 0;
    size_t alpha_nz = 0;
    for (size_t i = 0; i < total; ++i) {
      const std::uint8_t b = px[i * 4 + 0];
      const std::uint8_t g = px[i * 4 + 1];
      const std::uint8_t r = px[i * 4 + 2];
      const std::uint8_t a = px[i * 4 + 3];
      if (r != 0 || g != 0 || b != 0) {
        ++non_black;
      }
      if (a != 0) {
        ++alpha_nz;
      }
    }
    non_black_pct = total == 0 ? 0.0 : (100.0 * static_cast<double>(non_black) / static_cast<double>(total));
    alpha_nz_pct = total == 0 ? 0.0 : (100.0 * static_cast<double>(alpha_nz) / static_cast<double>(total));
    probe.report.Line(L"  non_black_pct=%.2f alpha_nz_pct=%.2f pixels=%llu", non_black_pct, alpha_nz_pct,
                      static_cast<unsigned long long>(total));
    if (png_path != nullptr) {
      DeleteFileW(png_path->c_str());
      const bool saved = SavePng(*png_path, static_cast<UINT>(w), static_cast<UINT>(h), static_cast<BYTE*>(bits));
      probe.report.Line(L"  png=%s saved=%s", png_path->c_str(), YesNo(saved));
      if (primary) {
        probe.capture_done = saved;
      }
    }
  } else if (timed_out) {
    probe.report.Line(L"  사유: PrintWindow 시간 상한 %ums (DC는 프로세스 종료 시 정리)", kCaptureMsMax);
  }

  if (primary) {
    probe.printwindow_ok = job.ok;
    probe.capture_w = w;
    probe.capture_h = h;
    probe.non_black_pct = non_black_pct;
    probe.alpha_nz_pct = alpha_nz_pct;
    probe.capture_visible = visible;
  }

  if (!timed_out) {
    SelectObject(mem_dc, old);
    DeleteDC(mem_dc);
    DeleteObject(dib);
  }
}

void WriteCapture(Probe& probe, const std::wstring& png_path) {
  probe.report.Line(L"## 6. PrintWindow");
  if (probe.first_tray == nullptr) {
    probe.report.Line(L"사유: Shell_TrayWnd 없음");
    probe.report.Line(L"");
    return;
  }

  HWND primary = probe.notify_area != nullptr ? probe.notify_area : probe.first_tray;
  probe.report.Line(L"notify_area_hwnd=0x%llX first_tray_hwnd=0x%llX", HwndU64(probe.notify_area),
                    HwndU64(probe.first_tray));
  CaptureOne(probe, primary, L"target", &png_path, true);
  if (probe.first_tray != primary) {
    CaptureOne(probe, probe.first_tray, L"shell_tray", nullptr, false);
  }
  probe.report.Line(L"");
}

constexpr LONG kParkY = 32000;
constexpr UINT kParkWaitMs = 3000;
constexpr UINT kFlashPollMs = 16;
constexpr UINT kFlashBurstMs = 250;
constexpr wchar_t kBridgeClass[] = L"Windows.UI.Composition.DesktopWindowContentBridge";
constexpr wchar_t kNotifyClass[] = L"TrayNotifyWnd";
constexpr wchar_t kPrimaryTrayClass[] = L"Shell_TrayWnd";
constexpr wchar_t kSecondaryTrayClass[] = L"Shell_SecondaryTrayWnd";

struct SavedTray {
  HWND hwnd = nullptr;
  std::wstring cls;
  RECT rc{};
  bool visible = false;
};

struct ParkCapture {
  HWND hwnd = nullptr;
  std::wstring cls;
  RECT rect{};
  bool visible = false;
  BOOL print_ok = FALSE;
  bool timeout = false;
  int w = 0;
  int h = 0;
  double non_black_pct = 0.0;
  double alpha_nz_pct = 0.0;
  std::vector<std::uint8_t> bits;
  bool png_saved = false;
  std::wstring png_path;
};

struct TrayButton {
  std::wstring name;
  std::wstring automation_id;
  std::wstring class_name;
  RECT screen{};
  BOOL offscreen = FALSE;
};

struct ChannelStd {
  bool in_bounds = false;
  int pixels = 0;
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
  double Max() const { return (std::max)(r, (std::max)(g, b)); }
};

void EnumTrayWindows(const auto& fn) {
  if (HWND primary = FindWindowW(kPrimaryTrayClass, nullptr)) {
    fn(primary);
  }
  HWND secondary = nullptr;
  while ((secondary = FindWindowExW(nullptr, secondary, kSecondaryTrayClass, nullptr)) != nullptr) {
    fn(secondary);
  }
}

HWND FindChildClass(HWND parent, const wchar_t* cls) {
  if (parent == nullptr || cls == nullptr) {
    return nullptr;
  }
  return FindWindowExW(parent, nullptr, cls, nullptr);
}

bool CaptureScreenRect(const RECT& rc, std::vector<std::uint8_t>* bits, int* w, int* h) {
  if (bits == nullptr || w == nullptr || h == nullptr) {
    return false;
  }
  const int width = rc.right - rc.left;
  const int height = rc.bottom - rc.top;
  if (width <= 0 || height <= 0) {
    return false;
  }
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* dib_bits = nullptr;
  HBITMAP dib = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &dib_bits, nullptr, 0);
  if (dib == nullptr || dib_bits == nullptr) {
    if (dib != nullptr) {
      DeleteObject(dib);
    }
    return false;
  }
  HDC mem = CreateCompatibleDC(nullptr);
  if (mem == nullptr) {
    DeleteObject(dib);
    return false;
  }
  HGDIOBJ old = SelectObject(mem, dib);
  HDC screen = GetDC(nullptr);
  const BOOL ok = screen != nullptr && BitBlt(mem, 0, 0, width, height, screen, rc.left, rc.top, SRCCOPY) != FALSE;
  if (screen != nullptr) {
    ReleaseDC(nullptr, screen);
  }
  if (ok) {
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    bits->assign(static_cast<const std::uint8_t*>(dib_bits), static_cast<const std::uint8_t*>(dib_bits) + n);
    *w = width;
    *h = height;
  }
  SelectObject(mem, old);
  DeleteDC(mem);
  DeleteObject(dib);
  return ok != FALSE;
}

double MeanAbsDiff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
  const size_t n = (std::min)(a.size(), b.size());
  if (n == 0) {
    return 255.0;
  }
  double sum = 0.0;
  for (size_t i = 0; i < n; ++i) {
    sum += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
  }
  return sum / static_cast<double>(n);
}

ParkCapture CaptureWindowBits(HWND hwnd, const std::wstring* png_path) {
  ParkCapture cap;
  cap.hwnd = hwnd;
  cap.cls = ClassOf(hwnd);
  cap.visible = hwnd != nullptr && IsWindowVisible(hwnd) != FALSE;
  if (hwnd != nullptr) {
    GetWindowRect(hwnd, &cap.rect);
  }
  cap.w = cap.rect.right - cap.rect.left;
  cap.h = cap.rect.bottom - cap.rect.top;
  if (hwnd == nullptr || cap.w <= 0 || cap.h <= 0) {
    return cap;
  }

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = cap.w;
  bmi.bmiHeader.biHeight = -cap.h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (dib == nullptr || bits == nullptr) {
    if (dib != nullptr) {
      DeleteObject(dib);
    }
    return cap;
  }
  HDC mem_dc = CreateCompatibleDC(nullptr);
  if (mem_dc == nullptr) {
    DeleteObject(dib);
    return cap;
  }
  HGDIOBJ old = SelectObject(mem_dc, dib);

  CaptureJob job;
  job.hwnd = hwnd;
  job.dc = mem_dc;
  job.flags = kRenderFullContent;
  Handle thread;
  thread.value = CreateThread(nullptr, 0, CaptureThread, &job, 0, nullptr);
  if (thread.value == nullptr) {
    SelectObject(mem_dc, old);
    DeleteDC(mem_dc);
    DeleteObject(dib);
    return cap;
  }
  if (WaitForSingleObject(thread.value, kCaptureMsMax) == WAIT_TIMEOUT) {
    cap.timeout = true;
    return cap;
  }
  cap.print_ok = job.ok;
  const size_t n = static_cast<size_t>(cap.w) * static_cast<size_t>(cap.h);
  cap.bits.assign(static_cast<const std::uint8_t*>(bits), static_cast<const std::uint8_t*>(bits) + n * 4);
  size_t non_black = 0;
  size_t alpha_nz = 0;
  for (size_t i = 0; i < n; ++i) {
    const std::uint8_t b = cap.bits[i * 4 + 0];
    const std::uint8_t g = cap.bits[i * 4 + 1];
    const std::uint8_t r = cap.bits[i * 4 + 2];
    const std::uint8_t a = cap.bits[i * 4 + 3];
    if (r != 0 || g != 0 || b != 0) {
      ++non_black;
    }
    if (a != 0) {
      ++alpha_nz;
    }
  }
  cap.non_black_pct = n == 0 ? 0.0 : (100.0 * static_cast<double>(non_black) / static_cast<double>(n));
  cap.alpha_nz_pct = n == 0 ? 0.0 : (100.0 * static_cast<double>(alpha_nz) / static_cast<double>(n));
  if (png_path != nullptr) {
    cap.png_path = *png_path;
    DeleteFileW(png_path->c_str());
    cap.png_saved = SavePng(*png_path, static_cast<UINT>(cap.w), static_cast<UINT>(cap.h), cap.bits.data());
  }
  SelectObject(mem_dc, old);
  DeleteDC(mem_dc);
  DeleteObject(dib);
  return cap;
}

ChannelStd RectChannelStd(const ParkCapture& cap, const RECT& screen) {
  ChannelStd out;
  if (cap.w <= 0 || cap.h <= 0 || cap.bits.size() < static_cast<size_t>(cap.w) * static_cast<size_t>(cap.h) * 4) {
    return out;
  }
  const int x0 = (std::max)(0, static_cast<int>(screen.left - cap.rect.left));
  const int y0 = (std::max)(0, static_cast<int>(screen.top - cap.rect.top));
  const int x1 = (std::min)(cap.w, static_cast<int>(screen.right - cap.rect.left));
  const int y1 = (std::min)(cap.h, static_cast<int>(screen.bottom - cap.rect.top));
  if (x1 <= x0 || y1 <= y0) {
    return out;
  }
  out.in_bounds = true;
  double sum_r = 0.0;
  double sum_g = 0.0;
  double sum_b = 0.0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(cap.w) + static_cast<size_t>(x)) * 4;
      sum_b += cap.bits[i + 0];
      sum_g += cap.bits[i + 1];
      sum_r += cap.bits[i + 2];
      ++out.pixels;
    }
  }
  if (out.pixels == 0) {
    return out;
  }
  const double n = static_cast<double>(out.pixels);
  const double mean_r = sum_r / n;
  const double mean_g = sum_g / n;
  const double mean_b = sum_b / n;
  double var_r = 0.0;
  double var_g = 0.0;
  double var_b = 0.0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(cap.w) + static_cast<size_t>(x)) * 4;
      const double db = cap.bits[i + 0] - mean_b;
      const double dg = cap.bits[i + 1] - mean_g;
      const double dr = cap.bits[i + 2] - mean_r;
      var_b += db * db;
      var_g += dg * dg;
      var_r += dr * dr;
    }
  }
  out.r = std::sqrt(var_r / n);
  out.g = std::sqrt(var_g / n);
  out.b = std::sqrt(var_b / n);
  return out;
}

Microsoft::WRL::ComPtr<IUIAutomationCondition> MakeTrayButtonCondition(IUIAutomation* uia) {
  Microsoft::WRL::ComPtr<IUIAutomationCondition> empty;
  if (uia == nullptr) {
    return empty;
  }
  VARIANT vn;
  VariantInit(&vn);
  vn.vt = VT_BSTR;
  vn.bstrVal = SysAllocString(L"NotifyItemIcon");
  Microsoft::WRL::ComPtr<IUIAutomationCondition> id_notify;
  const HRESULT nhr = uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, vn, id_notify.GetAddressOf());
  VariantClear(&vn);
  if (FAILED(nhr) || id_notify == nullptr) {
    return empty;
  }

  VARIANT vs;
  VariantInit(&vs);
  vs.vt = VT_BSTR;
  vs.bstrVal = SysAllocString(L"SystemTrayIcon");
  Microsoft::WRL::ComPtr<IUIAutomationCondition> id_system;
  const HRESULT shr = uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, vs, id_system.GetAddressOf());
  VariantClear(&vs);
  if (FAILED(shr) || id_system == nullptr) {
    return empty;
  }

  VARIANT vt;
  VariantInit(&vt);
  vt.vt = VT_I4;
  vt.lVal = UIA_ButtonControlTypeId;
  Microsoft::WRL::ComPtr<IUIAutomationCondition> type_btn;
  const HRESULT thr = uia->CreatePropertyCondition(UIA_ControlTypePropertyId, vt, type_btn.GetAddressOf());
  VariantClear(&vt);
  if (FAILED(thr) || type_btn == nullptr) {
    return empty;
  }

  Microsoft::WRL::ComPtr<IUIAutomationCondition> id_or;
  if (FAILED(uia->CreateOrCondition(id_notify.Get(), id_system.Get(), id_or.GetAddressOf())) || id_or == nullptr) {
    return empty;
  }
  Microsoft::WRL::ComPtr<IUIAutomationCondition> cond;
  if (FAILED(uia->CreateAndCondition(id_or.Get(), type_btn.Get(), cond.GetAddressOf()))) {
    return empty;
  }
  return cond;
}

void CollectButtonsFrom(IUIAutomation* uia, IUIAutomationCondition* cond, HWND hwnd, std::vector<TrayButton>* out) {
  if (uia == nullptr || cond == nullptr || hwnd == nullptr || out == nullptr) {
    return;
  }
  Microsoft::WRL::ComPtr<IUIAutomationElement> root;
  if (FAILED(uia->ElementFromHandle(hwnd, root.GetAddressOf())) || root == nullptr) {
    return;
  }
  Microsoft::WRL::ComPtr<IUIAutomationElementArray> arr;
  if (FAILED(root->FindAll(TreeScope_Descendants, cond, arr.GetAddressOf())) || arr == nullptr) {
    return;
  }
  int n = 0;
  arr->get_Length(&n);
  for (int i = 0; i < n; ++i) {
    Microsoft::WRL::ComPtr<IUIAutomationElement> el;
    if (FAILED(arr->GetElement(i, el.GetAddressOf())) || el == nullptr) {
      continue;
    }
    TrayButton btn;
    btn.name = ElementBstr(el.Get(), &IUIAutomationElement::get_CurrentName);
    btn.automation_id = ElementBstr(el.Get(), &IUIAutomationElement::get_CurrentAutomationId);
    btn.class_name = ElementBstr(el.Get(), &IUIAutomationElement::get_CurrentClassName);
    el->get_CurrentBoundingRectangle(&btn.screen);
    el->get_CurrentIsOffscreen(&btn.offscreen);
    out->push_back(std::move(btn));
  }
}

void DedupButtons(std::vector<TrayButton>* buttons) {
  if (buttons == nullptr) {
    return;
  }
  std::sort(buttons->begin(), buttons->end(), [](const TrayButton& a, const TrayButton& b) {
    if (a.screen.left != b.screen.left) {
      return a.screen.left < b.screen.left;
    }
    return a.name < b.name;
  });
  std::vector<TrayButton> unique;
  unique.reserve(buttons->size());
  for (const TrayButton& btn : *buttons) {
    bool seen = false;
    for (const TrayButton& have : unique) {
      if (have.screen.left == btn.screen.left && have.screen.top == btn.screen.top &&
          have.screen.right == btn.screen.right && have.screen.bottom == btn.screen.bottom &&
          have.automation_id == btn.automation_id) {
        seen = true;
        break;
      }
    }
    if (!seen) {
      unique.push_back(btn);
    }
  }
  *buttons = std::move(unique);
}

void RestoreSavedTrays(const std::vector<SavedTray>& saved) {
  for (const SavedTray& item : saved) {
    if (item.hwnd == nullptr || IsWindow(item.hwnd) == FALSE) {
      continue;
    }
    SetWindowPos(item.hwnd, nullptr, item.rc.left, item.rc.top, item.rc.right - item.rc.left,
                 item.rc.bottom - item.rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
    if (item.visible) {
      ShowWindow(item.hwnd, SW_SHOWNA);
    }
  }
}

struct ParkGuard {
  std::vector<SavedTray> saved;
  bool restored = false;

  void Restore() {
    if (restored) {
      return;
    }
    restored = true;
    RestoreSavedTrays(saved);
  }

  ~ParkGuard() { Restore(); }
};

void PrintSummary(const Probe& probe, const std::wstring& report_path, ULONGLONG elapsed_ms) {
  wprintf(L"os=%s elevated=%s %s\n", probe.os_line.c_str(), YesNo(probe.elevated), probe.wow_line.c_str());
  wprintf(L"bamti_resident=%s\n", YesNo(probe.bamti_resident));
  wprintf(L"toolbar32=%lu buttons=%s\n", probe.toolbar_count,
          probe.toolbar_buttons.empty() ? L"-" : probe.toolbar_buttons.c_str());
  wprintf(L"uia_button_candidates=%d\n", probe.uia_candidates);
  if (probe.capture_done || probe.first_tray != nullptr) {
    wprintf(L"printwindow ok=%s size=%dx%d non_black=%.2f%% alpha_nz=%.2f%% visible=%s elapsed_ms=%llu\n",
            YesNo(probe.printwindow_ok != FALSE), probe.capture_w, probe.capture_h, probe.non_black_pct,
            probe.alpha_nz_pct, YesNo(probe.capture_visible), elapsed_ms);
  } else {
    wprintf(L"printwindow skipped elapsed_ms=%llu\n", elapsed_ms);
  }
  wprintf(L"%s\n", report_path.c_str());
}

}  // namespace

int RunTrayProbe() {
  const ULONGLONG t0 = GetTickCount64();
  AttachParentConsole();
  Log(L"probe", L"tray probe start");

  const std::wstring dir = DataDir();
  if (dir.empty()) {
    Log(L"probe", L"DataDir empty");
    return 1;
  }
  const std::wstring report_path = JoinPath(dir, L"probe-tray.txt");
  const std::wstring png_path = JoinPath(dir, L"probe-tray.png");

  Probe probe;
  WriteEnv(probe);
  WriteTree(probe);
  WriteToolbars(probe);
  WriteButtons(probe);
  WriteUia(probe);
  WriteCapture(probe, png_path);

  const ULONGLONG elapsed = GetTickCount64() - t0;
  probe.report.Line(L"handles_end=%lu", HandleCount());
  probe.report.Line(L"elapsed_ms=%llu", elapsed);

  DeleteFileW(report_path.c_str());
  FILE* file = nullptr;
  if (_wfopen_s(&file, report_path.c_str(), L"w, ccs=UTF-8") != 0 || file == nullptr) {
    Log(L"probe", L"report open fail path=%s", report_path.c_str());
    PrintSummary(probe, report_path, elapsed);
    return 1;
  }
  fputws(probe.report.text.c_str(), file);
  fclose(file);

  Log(L"probe", L"tray probe done elapsed_ms=%llu report=%s", elapsed, report_path.c_str());
  PrintSummary(probe, report_path, elapsed);
  return 0;
}

int RunTrayProbeParked() {
  const ULONGLONG t0 = GetTickCount64();
  AttachParentConsole();
  Log(L"probe", L"tray parked probe start");

  const std::wstring dir = DataDir();
  if (dir.empty()) {
    Log(L"probe", L"DataDir empty");
    return 1;
  }
  const std::wstring report_path = JoinPath(dir, L"probe-tray-parked.txt");
  const std::wstring bridge_png = JoinPath(dir, L"probe-parked-bridge.png");
  const std::wstring notify_png = JoinPath(dir, L"probe-parked-notify.png");

  Report report;

  Probe env_probe;
  WriteEnv(env_probe);
  report.text += env_probe.report.text;

  ParkGuard guard;
  EnumTrayWindows([&](HWND hwnd) {
    SavedTray item;
    item.hwnd = hwnd;
    item.cls = ClassOf(hwnd);
    item.visible = IsWindowVisible(hwnd) != FALSE;
    GetWindowRect(hwnd, &item.rc);
    guard.saved.push_back(item);
  });

  report.Line(L"## 2. 주차 전 상태");
  if (guard.saved.empty()) {
    report.Line(L"Shell_TrayWnd: 없음");
  }
  RECT original_edge{};
  bool have_edge = false;
  for (const SavedTray& item : guard.saved) {
    report.Line(L"hwnd=0x%llX class=%s visible=%s rect(%ld,%ld,%ld,%ld)", HwndU64(item.hwnd), item.cls.c_str(),
                YesNo(item.visible), item.rc.left, item.rc.top, item.rc.right, item.rc.bottom);
    if (!have_edge && item.cls == kPrimaryTrayClass) {
      original_edge = item.rc;
      have_edge = true;
    }
  }
  report.Line(L"");

  std::vector<std::uint8_t> edge_before;
  int edge_w = 0;
  int edge_h = 0;
  bool edge_before_ok = false;
  if (have_edge && original_edge.top < kParkY / 2) {
    edge_before_ok = CaptureScreenRect(original_edge, &edge_before, &edge_w, &edge_h);
  }
  report.Line(L"## 3. 주차");
  report.Line(L"mode=SetWindowPos y=%ld ShowWindow=no", kParkY);
  bool park_ok = true;
  for (const SavedTray& item : guard.saved) {
    const BOOL pos = SetWindowPos(item.hwnd, nullptr, item.rc.left, kParkY, 0, 0,
                                  SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    RECT now{};
    GetWindowRect(item.hwnd, &now);
    report.Line(L"hwnd=0x%llX SetWindowPos=%s now_rect(%ld,%ld,%ld,%ld) visible=%s", HwndU64(item.hwnd),
                YesNo(pos != FALSE), now.left, now.top, now.right, now.bottom,
                YesNo(IsWindowVisible(item.hwnd) != FALSE));
    if (pos == FALSE || now.top < kParkY / 2) {
      park_ok = false;
    }
  }

  int flash_hits = 0;
  int flash_samples = 0;
  double max_edge_diff = 0.0;
  const ULONGLONG park_start = GetTickCount64();
  while (GetTickCount64() - park_start < kParkWaitMs) {
    ++flash_samples;
    bool at_edge = false;
    EnumTrayWindows([&](HWND hwnd) {
      RECT rc{};
      if (GetWindowRect(hwnd, &rc) != FALSE && rc.top < kParkY / 2) {
        at_edge = true;
      }
    });
    if (have_edge && original_edge.top < kParkY / 2) {
      std::vector<std::uint8_t> edge_now;
      int w = 0;
      int h = 0;
      if (edge_before_ok && CaptureScreenRect(original_edge, &edge_now, &w, &h)) {
        const double diff = MeanAbsDiff(edge_before, edge_now);
        if (diff > max_edge_diff) {
          max_edge_diff = diff;
        }
        // 원래 태스크바 픽셀과 거의 같으면 가장자리에 아직 남아 있다.
        if (diff < 6.0) {
          at_edge = true;
        }
      }
    }
    if (at_edge) {
      ++flash_hits;
    }
    const UINT sleep_ms = (GetTickCount64() - park_start < kFlashBurstMs) ? kFlashPollMs : 50;
    Sleep(sleep_ms);
  }
  const bool flashed = flash_hits > 0;
  report.Line(L"wait_ms=%u flash_samples=%d flash_hits=%d edge_mean_abs_diff_max=%.2f flashed=%s", kParkWaitMs,
              flash_samples, flash_hits, max_edge_diff, YesNo(flashed));
  report.Line(L"park_ok=%s", YesNo(park_ok));
  report.Line(L"");

  HWND tray = FindWindowW(kPrimaryTrayClass, nullptr);
  HWND bridge = FindChildClass(tray, kBridgeClass);
  HWND notify = FindChildClass(tray, kNotifyClass);
  report.Line(L"## 4. 주차 후 대상 창");
  auto dump_hwnd = [&](const wchar_t* label, HWND hwnd) {
    if (hwnd == nullptr) {
      report.Line(L"%s: 없음", label);
      return;
    }
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    report.Line(L"%s hwnd=0x%llX class=%s visible=%s rect(%ld,%ld,%ld,%ld)", label, HwndU64(hwnd), ClassOf(hwnd).c_str(),
                YesNo(IsWindowVisible(hwnd) != FALSE), rc.left, rc.top, rc.right, rc.bottom);
  };
  dump_hwnd(L"Shell_TrayWnd", tray);
  dump_hwnd(L"DesktopWindowContentBridge", bridge);
  dump_hwnd(L"TrayNotifyWnd", notify);
  report.Line(L"");

  report.Line(L"## 5. UI Automation 알림 영역 버튼");
  std::vector<TrayButton> buttons;
  Microsoft::WRL::ComPtr<IUIAutomation> uia;
  const HRESULT created =
      CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia.GetAddressOf()));
  if (FAILED(created) || uia == nullptr) {
    report.Line(L"CoCreateInstance(CLSID_CUIAutomation) hr=0x%08X", static_cast<unsigned>(created));
  } else {
    Microsoft::WRL::ComPtr<IUIAutomationCondition> cond = MakeTrayButtonCondition(uia.Get());
    if (cond == nullptr) {
      report.Line(L"CreatePropertyCondition 실패");
    } else {
      const ULONGLONG uia_t0 = GetTickCount64();
      CollectButtonsFrom(uia.Get(), cond.Get(), bridge != nullptr ? bridge : tray, &buttons);
      if (buttons.empty() && tray != nullptr && bridge != nullptr) {
        CollectButtonsFrom(uia.Get(), cond.Get(), tray, &buttons);
      }
      DedupButtons(&buttons);
      report.Line(L"elapsed_ms=%llu count=%llu", GetTickCount64() - uia_t0,
                  static_cast<unsigned long long>(buttons.size()));
      report.Line(L"형식: order  name  AutomationId  ClassName  BoundingRectangle  IsOffscreen");
      int order = 0;
      for (const TrayButton& btn : buttons) {
        report.Line(L"%d  %s  %s  %s  rect(%ld,%ld,%ld,%ld)  %s", order, Quoted(btn.name).c_str(),
                    Quoted(btn.automation_id).c_str(), Quoted(btn.class_name).c_str(), btn.screen.left, btn.screen.top,
                    btn.screen.right, btn.screen.bottom, YesNo(btn.offscreen != FALSE));
        ++order;
      }
    }
  }
  report.Line(L"");

  report.Line(L"## 6. PrintWindow");
  const ParkCapture bridge_cap = CaptureWindowBits(bridge, &bridge_png);
  const ParkCapture notify_cap = CaptureWindowBits(notify, &notify_png);
  auto dump_cap = [&](const wchar_t* label, const ParkCapture& cap) {
    report.Line(L"%s hwnd=0x%llX class=%s visible=%s rect(%ld,%ld,%ld,%ld)", label, HwndU64(cap.hwnd), cap.cls.c_str(),
                YesNo(cap.visible), cap.rect.left, cap.rect.top, cap.rect.right, cap.rect.bottom);
    report.Line(L"  PrintWindow=%s timeout=%s flags=0x%X size=%dx%d", YesNo(cap.print_ok != FALSE),
                YesNo(cap.timeout), kRenderFullContent, cap.w, cap.h);
    report.Line(L"  non_black_pct=%.2f alpha_nz_pct=%.2f pixels=%llu", cap.non_black_pct, cap.alpha_nz_pct,
                static_cast<unsigned long long>(cap.bits.size() / 4));
    if (!cap.png_path.empty()) {
      report.Line(L"  png=%s saved=%s", cap.png_path.c_str(), YesNo(cap.png_saved));
    }
  };
  dump_cap(L"bridge", bridge_cap);
  dump_cap(L"notify", notify_cap);
  report.Line(L"");

  report.Line(L"## 7. 아이콘 사각형 표준편차");
  report.Line(L"형식: order  name  bridge(in,px,std_r,std_g,std_b,std_max)  notify(...)");
  int bridge_pass = 0;
  int notify_pass = 0;
  int considered = 0;
  double bridge_sum = 0.0;
  double notify_sum = 0.0;
  int order = 0;
  for (const TrayButton& btn : buttons) {
    const ChannelStd bs = RectChannelStd(bridge_cap, btn.screen);
    const ChannelStd ns = RectChannelStd(notify_cap, btn.screen);
    report.Line(L"%d  %s  bridge(%s,%d,%.2f,%.2f,%.2f,%.2f)  notify(%s,%d,%.2f,%.2f,%.2f,%.2f)", order,
                Quoted(btn.name).c_str(), YesNo(bs.in_bounds), bs.pixels, bs.r, bs.g, bs.b, bs.Max(),
                YesNo(ns.in_bounds), ns.pixels, ns.r, ns.g, ns.b, ns.Max());
    if (bs.in_bounds || ns.in_bounds) {
      ++considered;
      if (bs.in_bounds) {
        bridge_sum += bs.Max();
        if (bs.Max() >= 8.0) {
          ++bridge_pass;
        }
      }
      if (ns.in_bounds) {
        notify_sum += ns.Max();
        if (ns.Max() >= 8.0) {
          ++notify_pass;
        }
      }
    }
    ++order;
  }
  const int half = considered / 2;
  const bool capture_ok = considered > 0 && ((bridge_pass > half) || (notify_pass > half));
  const wchar_t* target = L"none";
  if (capture_ok) {
    if (bridge_pass > half && notify_pass > half) {
      target = bridge_sum >= notify_sum ? L"DesktopWindowContentBridge" : L"TrayNotifyWnd";
    } else if (bridge_pass > half) {
      target = L"DesktopWindowContentBridge";
    } else {
      target = L"TrayNotifyWnd";
    }
  }
  report.Line(L"considered=%d half=%d bridge_pass=%d notify_pass=%d", considered, half, bridge_pass, notify_pass);
  report.Line(L"capture_path=%s target=%s", YesNo(capture_ok), target);
  report.Line(L"");

  report.Line(L"## 8. 복귀");
  guard.Restore();
  bool restore_ok = true;
  EnumTrayWindows([&](HWND hwnd) {
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    const bool visible = IsWindowVisible(hwnd) != FALSE;
    bool matched = false;
    for (const SavedTray& item : guard.saved) {
      if (item.hwnd == hwnd) {
        matched = true;
        const bool pos_ok = std::abs(rc.top - item.rc.top) < 8;
        report.Line(L"hwnd=0x%llX visible=%s rect(%ld,%ld,%ld,%ld) pos_ok=%s orig_visible=%s", HwndU64(hwnd),
                    YesNo(visible), rc.left, rc.top, rc.right, rc.bottom, YesNo(pos_ok), YesNo(item.visible));
        if (!pos_ok) {
          restore_ok = false;
        }
      }
    }
    if (!matched) {
      report.Line(L"hwnd=0x%llX 복귀 대상 아님 rect(%ld,%ld,%ld,%ld)", HwndU64(hwnd), rc.left, rc.top, rc.right,
                  rc.bottom);
    }
  });
  report.Line(L"restore_ok=%s flashed_during_park=%s", YesNo(restore_ok), YesNo(flashed));

  const ULONGLONG elapsed = GetTickCount64() - t0;
  report.Line(L"");
  report.Line(L"handles_end=%lu", HandleCount());
  report.Line(L"elapsed_ms=%llu", elapsed);

  DeleteFileW(report_path.c_str());
  FILE* file = nullptr;
  if (_wfopen_s(&file, report_path.c_str(), L"w, ccs=UTF-8") != 0 || file == nullptr) {
    Log(L"probe", L"parked report open fail path=%s", report_path.c_str());
    wprintf(L"report open fail %s\n", report_path.c_str());
    return 1;
  }
  fputws(report.text.c_str(), file);
  fclose(file);

  wprintf(L"bamti_resident=%s park_ok=%s restore_ok=%s flashed=%s\n", YesNo(env_probe.bamti_resident), YesNo(park_ok),
          YesNo(restore_ok), YesNo(flashed));
  wprintf(L"buttons=%llu capture_path=%s target=%s\n", static_cast<unsigned long long>(buttons.size()),
          YesNo(capture_ok), target);
  wprintf(L"bridge PrintWindow=%s size=%dx%d non_black=%.2f%%\n", YesNo(bridge_cap.print_ok != FALSE), bridge_cap.w,
          bridge_cap.h, bridge_cap.non_black_pct);
  wprintf(L"notify PrintWindow=%s size=%dx%d non_black=%.2f%%\n", YesNo(notify_cap.print_ok != FALSE), notify_cap.w,
          notify_cap.h, notify_cap.non_black_pct);
  wprintf(L"%s\n", report_path.c_str());
  Log(L"probe", L"tray parked probe done elapsed_ms=%llu report=%s capture=%s target=%s", elapsed, report_path.c_str(),
      YesNo(capture_ok), target);
  return 0;
}

}  // namespace bamti
