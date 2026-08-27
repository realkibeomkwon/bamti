#include "spotlight.hpp"

#include "dwm.hpp"
#include "theme.hpp"

#include <d2d1helper.h>
#include <dwmapi.h>
#include <imm.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <structuredquery.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <utility>

namespace bamti {
namespace {

constexpr int kWidthDip = 640;
constexpr int kShadowDip = 18;
constexpr int kPadDip = 12;
constexpr int kSearchHeightDip = 52;
constexpr int kCardRadiusDip = 16;
constexpr int kSearchRadiusDip = 12;
constexpr int kRowHeightDip = 40;
constexpr int kIconDip = 24;
constexpr int kMaxRows = 9;
constexpr int kMaxApps = 5;
constexpr int kMaxSettings = 4;
constexpr int kMaxFiles = 6;
constexpr int kGlyphDip = 18;
constexpr UINT_PTR kFileSearchTimer = 1;
constexpr UINT kFileSearchDelayMs = 140;
constexpr ULONGLONG kAppReloadMs = 60000;
constexpr COLORREF kSearchFillLight = RGB(255, 255, 255);
constexpr COLORREF kSearchFillDark = RGB(48, 48, 48);
constexpr COLORREF kSearchTextLight = RGB(24, 24, 24);
constexpr COLORREF kSearchTextDark = RGB(242, 242, 242);

struct SettingDef {
  const wchar_t* title;
  const wchar_t* aliases;
  const wchar_t* uri;
};

constexpr SettingDef kSettings[] = {
    {L"설정", L"settings home 홈", L"ms-settings:"},
    {L"시스템", L"system about 정보", L"ms-settings:about"},
    {L"디스플레이", L"display 화면 해상도 모니터 night", L"ms-settings:display"},
    {L"야간 모드", L"night light 야간조명", L"ms-settings:nightlight"},
    {L"소리", L"sound audio 오디오 볼륨", L"ms-settings:sound"},
    {L"알림", L"notifications 알림", L"ms-settings:notifications"},
    {L"포커스", L"focus 집중 방해 금지", L"ms-settings:quiethours"},
    {L"전원 및 배터리", L"power battery sleep 절전 배터리", L"ms-settings:powersleep"},
    {L"저장소", L"storage disk 디스크 저장소", L"ms-settings:storagesense"},
    {L"앱", L"apps features 프로그램", L"ms-settings:appsfeatures"},
    {L"설치된 앱", L"installed apps 설치", L"ms-settings:appsfeatures"},
    {L"기본 앱", L"defaults 기본앱", L"ms-settings:defaultapps"},
    {L"시작 앱", L"startup 시작프로그램", L"ms-settings:startupapps"},
    {L"Bluetooth 및 장치", L"bluetooth 블루투스 장치 devices", L"ms-settings:bluetooth"},
    {L"프린터 및 스캐너", L"printer scanner 프린터", L"ms-settings:printers"},
    {L"마우스", L"mouse 마우스", L"ms-settings:mousetouchpad"},
    {L"터치패드", L"touchpad 터치패드", L"ms-settings:devices-touchpad"},
    {L"입력", L"typing keyboard ime 키보드 입력기", L"ms-settings:typing"},
    {L"네트워크 및 인터넷", L"network 네트워크", L"ms-settings:network"},
    {L"Wi-Fi", L"wifi 와이파이 wireless 무선", L"ms-settings:network-wifi"},
    {L"이더넷", L"ethernet 이더넷", L"ms-settings:network-ethernet"},
    {L"VPN", L"vpn", L"ms-settings:network-vpn"},
    {L"비행기 모드", L"airplane 비행기", L"ms-settings:network-airplanemode"},
    {L"개인 설정", L"personalization 테마", L"ms-settings:personalization"},
    {L"배경", L"background wallpaper 배경화면", L"ms-settings:personalization-background"},
    {L"색", L"colors 색 다크 dark 테마색", L"ms-settings:colors"},
    {L"잠금 화면", L"lock screen 잠금화면", L"ms-settings:lockscreen"},
    {L"테마", L"themes 테마", L"ms-settings:themes"},
    {L"작업 표시줄", L"taskbar 작업표시줄", L"ms-settings:taskbar"},
    {L"시작", L"start menu 시작메뉴", L"ms-settings:personalization-start"},
    {L"계정", L"accounts 계정 사용자", L"ms-settings:yourinfo"},
    {L"로그인 옵션", L"pin password 암호 windows hello", L"ms-settings:signinoptions"},
    {L"시간 및 언어", L"time language 시간 언어", L"ms-settings:dateandtime"},
    {L"언어 및 지역", L"region 지역 language 언어", L"ms-settings:regionlanguage"},
    {L"음성", L"speech 음성 마이크", L"ms-settings:speech"},
    {L"게임 바", L"xbox game bar 게임", L"ms-settings:gaming-gamebar"},
    {L"접근성", L"accessibility ease 내레이터 접근성", L"ms-settings:easeofaccess"},
    {L"개인 정보 보호", L"privacy 개인정보", L"ms-settings:privacy"},
    {L"Windows 업데이트", L"update 업데이트", L"ms-settings:windowsupdate"},
    {L"Windows 보안", L"security defender 보안", L"ms-settings:windowsdefender"},
    {L"검색 설정", L"search windows search 검색설정", L"ms-settings:cortana-windowssearch"},
    {L"클립보드", L"clipboard 클립보드", L"ms-settings:clipboard"},
    {L"개발자용", L"developer 개발자", L"ms-settings:developers"},
    {L"저장소 센스", L"storage sense 저장소센스", L"ms-settings:storagepolicies"},
};

int DipToPx(int dip, UINT dpi) {
  return MulDiv(dip, static_cast<int>(dpi), 96);
}

std::wstring LowerCopy(std::wstring text) {
  if (!text.empty()) {
    CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
  }
  return text;
}

int MatchScore(std::wstring hay, const std::wstring& needle) {
  if (needle.empty()) {
    return -1;
  }
  hay = LowerCopy(std::move(hay));
  if (hay.size() >= needle.size() && hay.compare(0, needle.size(), needle) == 0) {
    return 0;
  }
  const size_t pos = hay.find(needle);
  if (pos == std::wstring::npos) {
    return -1;
  }
  if (pos > 0 && hay[pos - 1] != L' ' && hay[pos - 1] != L'-') {
    return 2;
  }
  return 1;
}

bool EndsWithIgnoreCase(const std::wstring& text, const wchar_t* suffix) {
  const size_t n = wcslen(suffix);
  if (text.size() < n) {
    return false;
  }
  return lstrcmpiW(text.c_str() + (text.size() - n), suffix) == 0;
}

bool SkipShortcut(const std::wstring& name) {
  const std::wstring lower = LowerCopy(name);
  return lower.find(L"uninstall") != std::wstring::npos || lower.find(L"제거") != std::wstring::npos;
}

bool SkipWalkDir(const wchar_t* name) {
  return lstrcmpiW(name, L"node_modules") == 0 || lstrcmpiW(name, L".git") == 0 ||
         lstrcmpiW(name, L"AppData") == 0;
}

std::wstring StemFromFile(const std::wstring& name) {
  std::wstring stem = name;
  if (EndsWithIgnoreCase(stem, L".lnk")) {
    stem.resize(stem.size() - 4);
  }
  return stem;
}

std::wstring DisplayNameForShortcut(const std::wstring& path, const std::wstring& filename) {
  SHFILEINFOW info{};
  if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_DISPLAYNAME) != 0 &&
      info.szDisplayName[0] != L'\0') {
    return info.szDisplayName;
  }
  return StemFromFile(filename);
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR path = nullptr;
  if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &path)) || path == nullptr) {
    return {};
  }
  std::wstring out = path;
  CoTaskMemFree(path);
  return out;
}

std::wstring ParentName(const std::wstring& path) {
  size_t end = path.find_last_not_of(L"\\/");
  if (end == std::wstring::npos) {
    return {};
  }
  const size_t slash = path.find_last_of(L"\\/", end);
  if (slash == std::wstring::npos || slash == 0) {
    return {};
  }
  const size_t prev = path.find_last_of(L"\\/", slash - 1);
  const size_t begin = prev == std::wstring::npos ? 0 : prev + 1;
  return path.substr(begin, slash - begin);
}

const wchar_t* KindLabel(Spotlight::Kind kind) {
  switch (kind) {
    case Spotlight::Kind::App:
      return L"앱";
    case Spotlight::Kind::Setting:
      return L"설정";
    case Spotlight::Kind::File:
      return L"파일";
    case Spotlight::Kind::Folder:
      return L"폴더";
  }
  return L"";
}

HICON LoadStockIcon(SHSTOCKICONID id) {
  SHSTOCKICONINFO info{};
  info.cbSize = sizeof(info);
  if (FAILED(SHGetStockIconInfo(id, SHGSI_ICON | SHGSI_LARGEICON, &info))) {
    return nullptr;
  }
  return info.hIcon;
}

void MakeEditInputOnly(HWND edit) {
  if (edit == nullptr) {
    return;
  }
  const LONG_PTR ex = GetWindowLongPtrW(edit, GWL_EXSTYLE);
  if ((ex & WS_EX_LAYERED) == 0) {
    SetWindowLongPtrW(edit, GWL_EXSTYLE, ex | WS_EX_LAYERED);
  }
  SetLayeredWindowAttributes(edit, 0, 0, LWA_ALPHA);
}

bool ShouldRefreshSearchVisual(UINT msg, WPARAM wparam) {
  switch (msg) {
    case WM_CHAR:
    case WM_DEADCHAR:
    case WM_UNICHAR:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_IME_COMPOSITION:
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_ENDCOMPOSITION:
    case WM_IME_CHAR:
    case WM_PASTE:
    case WM_CUT:
    case WM_CLEAR:
    case WM_UNDO:
      return true;
    case WM_MOUSEMOVE:
      return (wparam & MK_LBUTTON) != 0;
    default:
      return false;
  }
}

bool ShouldApplyFilter(UINT msg) {
  switch (msg) {
    case WM_CHAR:
    case WM_IME_COMPOSITION:
    case WM_IME_CHAR:
    case WM_IME_ENDCOMPOSITION:
    case WM_PASTE:
    case WM_CUT:
    case WM_CLEAR:
    case WM_UNDO:
      return true;
    default:
      return false;
  }
}

struct EditView {
  std::wstring text;
  UINT32 caret = 0;
};

EditView ReadEditView(HWND edit) {
  EditView view;
  if (edit == nullptr) {
    return view;
  }
  const int n = GetWindowTextLengthW(edit);
  if (n > 0) {
    view.text.resize(static_cast<size_t>(n));
    GetWindowTextW(edit, view.text.data(), n + 1);
  }
  DWORD start = 0;
  DWORD end = 0;
  SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
  if (start > view.text.size()) {
    start = static_cast<DWORD>(view.text.size());
  }
  if (end > view.text.size()) {
    end = static_cast<DWORD>(view.text.size());
  }
  if (end < start) {
    end = start;
  }
  view.caret = end;

  const HIMC himc = ImmGetContext(edit);
  if (himc == nullptr) {
    return view;
  }
  const LONG bytes = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
  LONG cursor = 0;
  if (bytes > 0) {
    std::wstring comp(static_cast<size_t>(bytes / sizeof(wchar_t)), L'\0');
    ImmGetCompositionStringW(himc, GCS_COMPSTR, comp.data(), bytes);
    cursor = ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0);
    if (cursor < 0) {
      cursor = static_cast<LONG>(comp.size());
    }

    bool already = false;
    if (end > start && view.text.compare(start, end - start, comp) == 0) {
      already = true;
      view.caret = start + static_cast<UINT32>(cursor);
    } else if (start >= comp.size() &&
               view.text.compare(start - static_cast<DWORD>(comp.size()), comp.size(), comp) == 0) {
      already = true;
      view.caret = start - static_cast<UINT32>(comp.size()) + static_cast<UINT32>(cursor);
    } else if (start + comp.size() <= view.text.size() && view.text.compare(start, comp.size(), comp) == 0) {
      already = true;
      view.caret = start + static_cast<UINT32>(cursor);
    }
    if (!already) {
      view.text.replace(start, end - start, comp);
      view.caret = start + static_cast<UINT32>(cursor);
    }
  }
  ImmReleaseContext(edit, himc);
  if (view.caret > view.text.size()) {
    view.caret = static_cast<UINT32>(view.text.size());
  }
  return view;
}

void CollectFolder(const std::wstring& root, std::vector<std::pair<std::wstring, std::wstring>>& out,
                   std::unordered_set<std::wstring>& seen) {
  if (root.empty()) {
    return;
  }
  WIN32_FIND_DATAW fd{};
  const HANDLE find = FindFirstFileW((root + L"\\*").c_str(), &fd);
  if (find == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    if (fd.cFileName[0] == L'.' &&
        (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
      continue;
    }
    const std::wstring full = root + L'\\' + fd.cFileName;
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      if ((fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) == 0) {
        CollectFolder(full, out, seen);
      }
      continue;
    }
    if (!EndsWithIgnoreCase(fd.cFileName, L".lnk") || SkipShortcut(fd.cFileName)) {
      continue;
    }
    std::wstring name = DisplayNameForShortcut(full, fd.cFileName);
    if (name.empty()) {
      continue;
    }
    std::wstring key = LowerCopy(name);
    if (!seen.insert(key).second) {
      continue;
    }
    out.push_back({std::move(name), full});
  } while (FindNextFileW(find, &fd) != FALSE);
  FindClose(find);
}

void WalkNamed(const std::wstring& dir, const std::wstring& needle, std::vector<Spotlight::FileHit>& out, int depth,
               int& remaining) {
  if (remaining <= 0 || dir.empty() || depth < 0) {
    return;
  }
  WIN32_FIND_DATAW fd{};
  const HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (find == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    if (remaining <= 0) {
      break;
    }
    if (fd.cFileName[0] == L'.' &&
        (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
      continue;
    }
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0) {
      continue;
    }
    const bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (is_dir && SkipWalkDir(fd.cFileName)) {
      continue;
    }
    const std::wstring full = dir + L'\\' + fd.cFileName;
    if (is_dir && depth > 0) {
      WalkNamed(full, needle, out, depth - 1, remaining);
    }
    const std::wstring lower = LowerCopy(fd.cFileName);
    if (lower.find(needle) == std::wstring::npos) {
      continue;
    }
    Spotlight::FileHit hit;
    hit.title = fd.cFileName;
    hit.path = full;
    hit.folder = is_dir;
    hit.detail = ParentName(full);
    out.push_back(std::move(hit));
    --remaining;
  } while (FindNextFileW(find, &fd) != FALSE);
  FindClose(find);
}

bool QueryIndexedFiles(const std::wstring& query, std::vector<Spotlight::FileHit>& out) {
  Microsoft::WRL::ComPtr<IQueryParserManager> manager;
  if (FAILED(CoCreateInstance(__uuidof(QueryParserManager), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager))) ||
      !manager) {
    return false;
  }
  Microsoft::WRL::ComPtr<IQueryParser> parser;
  if (FAILED(manager->CreateLoadedParser(L"SystemIndex", LANGIDFROMLCID(GetUserDefaultLCID()),
                                          IID_PPV_ARGS(&parser))) ||
      !parser) {
    return false;
  }
  Microsoft::WRL::ComPtr<IQuerySolution> solution;
  if (FAILED(parser->Parse(query.c_str(), nullptr, &solution)) || !solution) {
    return false;
  }
  Microsoft::WRL::ComPtr<ICondition> condition;
  if (FAILED(solution->GetQuery(&condition, nullptr)) || !condition) {
    return false;
  }
  Microsoft::WRL::ComPtr<ISearchFolderItemFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_SearchFolderItemFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
      !factory) {
    return false;
  }
  factory->SetDisplayName(L"bamti");
  factory->SetCondition(condition.Get());

  Microsoft::WRL::ComPtr<IShellItem> profile;
  if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&profile))) && profile) {
    Microsoft::WRL::ComPtr<IShellItemArray> scope;
    if (SUCCEEDED(SHCreateShellItemArrayFromShellItem(profile.Get(), IID_PPV_ARGS(&scope))) && scope) {
      factory->SetScope(scope.Get());
    }
  }

  Microsoft::WRL::ComPtr<IShellItem> folder;
  if (FAILED(factory->GetShellItem(IID_PPV_ARGS(&folder))) || !folder) {
    return false;
  }
  Microsoft::WRL::ComPtr<IEnumShellItems> enumer;
  if (FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&enumer))) || !enumer) {
    return false;
  }

  for (int n = 0; n < 12; ++n) {
    Microsoft::WRL::ComPtr<IShellItem> item;
    ULONG fetched = 0;
    if (enumer->Next(1, item.GetAddressOf(), &fetched) != S_OK || fetched == 0 || !item) {
      break;
    }
    Spotlight::FileHit hit;
    PWSTR name = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name != nullptr) {
      hit.title = name;
      CoTaskMemFree(name);
    }
    PWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
      hit.path = path;
      CoTaskMemFree(path);
    } else {
      PWSTR url = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_URL, &url)) && url != nullptr) {
        hit.path = url;
        CoTaskMemFree(url);
      }
    }
    if (hit.title.empty() || hit.path.empty()) {
      continue;
    }
    const std::wstring lower_path = LowerCopy(hit.path);
    if (lower_path.find(L"\\start menu\\") != std::wstring::npos) {
      continue;
    }
    SFGAOF attr = 0;
    if (SUCCEEDED(item->GetAttributes(SFGAO_FOLDER, &attr)) && (attr & SFGAO_FOLDER) != 0) {
      hit.folder = true;
    }
    hit.detail = ParentName(hit.path);
    out.push_back(std::move(hit));
  }
  return !out.empty();
}

D2D1_COLOR_F FillColor(bool dark) {
  return dark ? D2D1::ColorF(0.14f, 0.14f, 0.14f, 1.0f) : D2D1::ColorF(0.97f, 0.97f, 0.97f, 1.0f);
}

D2D1_COLOR_F SearchFillColor(bool dark) {
  return dark ? D2D1::ColorF(0.22f, 0.22f, 0.22f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
}

D2D1_COLOR_F TextColor(bool dark) {
  return dark ? D2D1::ColorF(0.95f, 0.95f, 0.95f, 1.0f) : D2D1::ColorF(0.09f, 0.09f, 0.09f, 1.0f);
}

D2D1_COLOR_F CueColor(bool dark) {
  return dark ? D2D1::ColorF(0.62f, 0.62f, 0.62f, 1.0f) : D2D1::ColorF(0.52f, 0.52f, 0.52f, 1.0f);
}

D2D1_COLOR_F DividerColor(bool dark) {
  return dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f);
}

void DrawMagnifier(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, D2D1_POINT_2F center, float radius) {
  const D2D1_ELLIPSE rim{center, radius, radius};
  rt->DrawEllipse(rim, brush, (std::max)(1.2f, radius * 0.22f));
  const D2D1_POINT_2F a{center.x + radius * 0.72f, center.y + radius * 0.72f};
  const D2D1_POINT_2F b{center.x + radius * 1.38f, center.y + radius * 1.38f};
  rt->DrawLine(a, b, brush, (std::max)(1.2f, radius * 0.28f));
}

}  // namespace

Spotlight::~Spotlight() {
  Hide();
  DestroyAppIcons();
  DestroyFileIcons();
  if (settings_icon_ != nullptr) {
    DestroyIcon(settings_icon_);
    settings_icon_ = nullptr;
  }
  if (folder_icon_ != nullptr) {
    DestroyIcon(folder_icon_);
    folder_icon_ = nullptr;
  }
  ReleaseLayer();
  if (edit_font_ != nullptr) {
    DeleteObject(edit_font_);
    edit_font_ = nullptr;
  }
  if (search_brush_ != nullptr) {
    DeleteObject(search_brush_);
    search_brush_ = nullptr;
  }
  if (hwnd_ != nullptr) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void Spotlight::Warmup(HWND owner, bool dark) {
  owner_ = owner;
  dark_ = dark;
  if (!EnsureWindow(owner)) {
    return;
  }
  EnsureApps();
  if (settings_icon_ == nullptr) {
    settings_icon_ = LoadStockIcon(SIID_SOFTWARE);
  }
  if (folder_icon_ == nullptr) {
    folder_icon_ = LoadStockIcon(SIID_FOLDER);
  }
  ApplyChrome();
}

void Spotlight::Toggle(HWND owner, bool dark) {
  if (visible_) {
    Hide();
    return;
  }
  owner_ = owner;
  dark_ = dark;
  if (!EnsureWindow(owner)) {
    return;
  }
  EnsureApps();
  filter_.clear();
  scroll_ = 0;
  hot_ = -1;
  DestroyFileIcons();
  files_.clear();
  matches_.clear();
  if (edit_ != nullptr) {
    SetWindowTextW(edit_, L"");
  }
  ApplyChrome();
  LayoutWindow();
  visible_ = true;
  ShowWindow(hwnd_, SW_SHOW);
  SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  Present();
  HWND foreground = GetForegroundWindow();
  DWORD other_tid = 0;
  if (foreground != nullptr) {
    other_tid = GetWindowThreadProcessId(foreground, nullptr);
  }
  const DWORD self_tid = GetCurrentThreadId();
  if (other_tid != 0 && other_tid != self_tid) {
    AttachThreadInput(self_tid, other_tid, TRUE);
  }
  SetForegroundWindow(hwnd_);
  if (other_tid != 0 && other_tid != self_tid) {
    AttachThreadInput(self_tid, other_tid, FALSE);
  }
  if (edit_ != nullptr) {
    SetFocus(edit_);
  }
}

void Spotlight::Hide() {
  if (hwnd_ != nullptr) {
    KillTimer(hwnd_, kFileSearchTimer);
  }
  if (!visible_ && !closing_) {
    if (hwnd_ != nullptr) {
      ShowWindow(hwnd_, SW_HIDE);
    }
    return;
  }
  visible_ = false;
  closing_ = false;
  hot_ = -1;
  if (hwnd_ != nullptr) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

LRESULT CALLBACK Spotlight::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  Spotlight* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<Spotlight*>(create->lpCreateParams);
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else {
    self = reinterpret_cast<Spotlight*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (self != nullptr) {
    return self->HandleMessage(msg, wparam, lparam);
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK Spotlight::EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* self = reinterpret_cast<Spotlight*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self == nullptr || self->edit_prev_ == nullptr) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_NCPAINT || msg == WM_ERASEBKGND || msg == WM_PRINTCLIENT) {
    return 0;
  }
  if (msg == WM_PAINT) {
    self->PaintEdit(hwnd);
    return 0;
  }
  if (msg == WM_SETFOCUS) {
    const LRESULT result = CallWindowProcW(self->edit_prev_, hwnd, msg, wparam, lparam);
    DestroyCaret();
    CreateCaret(hwnd, nullptr, (std::max)(1, self->Dip(1)), self->Dip(20));
    HideCaret(hwnd);
    self->Present();
    return result;
  }
  if (msg == WM_KILLFOCUS) {
    DestroyCaret();
    self->Present();
    return CallWindowProcW(self->edit_prev_, hwnd, msg, wparam, lparam);
  }
  if (msg == WM_KEYDOWN) {
    if (wparam == VK_ESCAPE) {
      self->Hide();
      return 0;
    }
    if (wparam == VK_RETURN) {
      self->ActivateHot();
      return 0;
    }
    if (wparam == VK_DOWN) {
      self->MoveHot(1);
      return 0;
    }
    if (wparam == VK_UP) {
      self->MoveHot(-1);
      return 0;
    }
  }
  const LRESULT result = CallWindowProcW(self->edit_prev_, hwnd, msg, wparam, lparam);
  if (ShouldApplyFilter(msg)) {
    self->ApplyFilter();
  } else if (ShouldRefreshSearchVisual(msg, wparam)) {
    self->Present();
  }
  return result;
}

LRESULT Spotlight::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      BeginPaint(hwnd_, &ps);
      EndPaint(hwnd_, &ps);
      Present();
      return 0;
    }
    case WM_TIMER:
      if (wparam == kFileSearchTimer) {
        KillTimer(hwnd_, kFileSearchTimer);
        QueryFiles(filter_);
        RebuildMatches();
        LayoutWindow();
      }
      return 0;
    case WM_MOUSEMOVE: {
      const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      UpdateHot(pt);
      return 0;
    }
    case WM_MOUSELEAVE:
      return 0;
    case WM_LBUTTONDOWN: {
      const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (PtInRect(&search_rect_, pt) && edit_ != nullptr) {
        SetFocus(edit_);
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (const Row* row = HitTest(pt)) {
        if (Selectable(*row)) {
          ActivateMatch(row->match);
        }
      }
      return 0;
    }
    case WM_MOUSEWHEEL:
      if (!matches_.empty()) {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        scroll_ -= (delta / WHEEL_DELTA) * Dip(kRowHeightDip);
        RebuildRows();
        Present();
      }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        const Row* row = HitTest(pt);
        if (row != nullptr && Selectable(*row)) {
          SetCursor(LoadCursorW(nullptr, IDC_HAND));
          return TRUE;
        }
      }
      break;
    case WM_MOUSEACTIVATE:
      return MA_ACTIVATE;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
      const HDC hdc = reinterpret_cast<HDC>(wparam);
      const COLORREF text = dark_ ? kSearchTextDark : kSearchTextLight;
      const COLORREF fill = dark_ ? kSearchFillDark : kSearchFillLight;
      SetTextColor(hdc, text);
      SetBkColor(hdc, fill);
      SetBkMode(hdc, OPAQUE);
      if (search_brush_ == nullptr) {
        search_brush_ = CreateSolidBrush(fill);
      }
      return reinterpret_cast<LRESULT>(search_brush_);
    }
    case WM_COMMAND:
      if (HIWORD(wparam) == EN_CHANGE && reinterpret_cast<HWND>(lparam) == edit_) {
        ApplyFilter();
      }
      return 0;
    case WM_KEYDOWN:
      if (wparam == VK_ESCAPE) {
        Hide();
        return 0;
      }
      if (wparam == VK_RETURN) {
        ActivateHot();
        return 0;
      }
      if (wparam == VK_DOWN) {
        MoveHot(1);
        return 0;
      }
      if (wparam == VK_UP) {
        MoveHot(-1);
        return 0;
      }
      break;
    case WM_ACTIVATE:
      if (LOWORD(wparam) == WA_INACTIVE && visible_) {
        closing_ = true;
        Hide();
      }
      return 0;
    case WM_DPICHANGED:
      if (visible_) {
        LayoutWindow();
      }
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd_, kFileSearchTimer);
      hwnd_ = nullptr;
      edit_ = nullptr;
      visible_ = false;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

bool Spotlight::EnsureWindow(HWND owner) {
  if (hwnd_ != nullptr) {
    return true;
  }
  HINSTANCE instance = nullptr;
  if (owner != nullptr) {
    instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
  }
  if (instance == nullptr) {
    instance = GetModuleHandleW(nullptr);
  }

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
  wc.lpszClassName = kSpotlightClass;
  if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kSpotlightClass, L"", WS_POPUP, 0, 0, 0, 0,
                          owner, nullptr, instance, this);
  if (hwnd_ == nullptr) {
    return false;
  }

  edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 0, 0, hwnd_, nullptr,
                          instance, nullptr);
  if (edit_ == nullptr) {
    return false;
  }
  SetWindowTheme(edit_, L"", L"");
  MakeEditInputOnly(edit_);
  SetWindowLongPtrW(edit_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  edit_prev_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));
  return true;
}

bool Spotlight::EnsureRenderer() {
  if (!d2d_) {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.ReleaseAndGetAddressOf()))) {
      return false;
    }
  }
  if (!dwrite_) {
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.ReleaseAndGetAddressOf())))) {
      return false;
    }
  }
  if (!format_ || !search_format_ || !meta_format_ || font_dpi_ != Dpi()) {
    font_dpi_ = Dpi();
    format_.Reset();
    search_format_.Reset();
    meta_format_.Reset();
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) == 0) {
      wcscpy_s(locale, L"en-US");
    }
    const float row_size = 14.0f * static_cast<float>(font_dpi_) / 96.0f;
    const float search_size = 18.0f * static_cast<float>(font_dpi_) / 96.0f;
    const float meta_size = 11.0f * static_cast<float>(font_dpi_) / 96.0f;
    const wchar_t* families[] = {L"Segoe UI Variable", L"Segoe UI"};
    auto make_format = [&](float size, IDWriteTextFormat** out) -> HRESULT {
      HRESULT hr = E_FAIL;
      for (const wchar_t* family : families) {
        hr = dwrite_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, size, locale, out);
        if (SUCCEEDED(hr)) {
          break;
        }
      }
      return hr;
    };
    if (FAILED(make_format(row_size, format_.ReleaseAndGetAddressOf())) || !format_) {
      return false;
    }
    if (FAILED(make_format(search_size, search_format_.ReleaseAndGetAddressOf())) || !search_format_) {
      return false;
    }
    if (FAILED(make_format(meta_size, meta_format_.ReleaseAndGetAddressOf())) || !meta_format_) {
      return false;
    }
    format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    search_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    search_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    search_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    meta_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    meta_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    meta_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
  }
  if (!rt_) {
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    if (FAILED(d2d_->CreateDCRenderTarget(&props, rt_.ReleaseAndGetAddressOf()))) {
      return false;
    }
  }
  return true;
}

bool Spotlight::EnsureLayer(int width, int height) {
  if (width <= 0 || height <= 0) {
    return false;
  }
  if (layer_dc_ != nullptr && layer_bmp_ != nullptr && layer_w_ == width && layer_h_ == height) {
    return true;
  }
  ReleaseLayer();
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  layer_dc_ = CreateCompatibleDC(nullptr);
  if (layer_dc_ == nullptr) {
    return false;
  }
  layer_bmp_ = CreateDIBSection(layer_dc_, &bmi, DIB_RGB_COLORS, &layer_bits_, nullptr, 0);
  if (layer_bmp_ == nullptr) {
    ReleaseLayer();
    return false;
  }
  layer_old_ = SelectObject(layer_dc_, layer_bmp_);
  layer_w_ = width;
  layer_h_ = height;
  return true;
}

void Spotlight::ReleaseLayer() {
  if (layer_dc_ != nullptr && layer_old_ != nullptr) {
    SelectObject(layer_dc_, layer_old_);
    layer_old_ = nullptr;
  }
  if (layer_bmp_ != nullptr) {
    DeleteObject(layer_bmp_);
    layer_bmp_ = nullptr;
  }
  if (layer_dc_ != nullptr) {
    DeleteDC(layer_dc_);
    layer_dc_ = nullptr;
  }
  layer_bits_ = nullptr;
  layer_w_ = 0;
  layer_h_ = 0;
}

void Spotlight::DestroyAppIcons() {
  for (auto& app : apps_) {
    if (app.icon != nullptr) {
      DestroyIcon(app.icon);
      app.icon = nullptr;
    }
  }
}

void Spotlight::DestroyFileIcons() {
  for (auto& file : files_) {
    if (file.icon != nullptr) {
      DestroyIcon(file.icon);
      file.icon = nullptr;
    }
  }
}

void Spotlight::ReloadApps() {
  DestroyAppIcons();
  apps_.clear();
  std::unordered_set<std::wstring> seen;
  std::vector<std::pair<std::wstring, std::wstring>> raw;
  CollectFolder(KnownFolder(FOLDERID_StartMenu), raw, seen);
  CollectFolder(KnownFolder(FOLDERID_CommonStartMenu), raw, seen);
  std::sort(raw.begin(), raw.end(), [](const auto& a, const auto& b) {
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE, a.first.c_str(),
                           static_cast<int>(a.first.size()), b.first.c_str(), static_cast<int>(b.first.size()), nullptr,
                           nullptr, 0) == CSTR_LESS_THAN;
  });
  apps_.reserve(raw.size());
  for (auto& item : raw) {
    AppEntry entry;
    entry.name = std::move(item.first);
    entry.path = std::move(item.second);
    apps_.push_back(std::move(entry));
  }
  apps_loaded_at_ = GetTickCount64();
}

void Spotlight::EnsureApps() {
  if (apps_.empty() || GetTickCount64() - apps_loaded_at_ > kAppReloadMs) {
    ReloadApps();
  }
}

HICON Spotlight::EnsureIcon(const Match& match) {
  switch (match.kind) {
    case Kind::App:
      if (match.index >= 0 && match.index < static_cast<int>(apps_.size())) {
        AppEntry& entry = apps_[static_cast<size_t>(match.index)];
        if (entry.icon == nullptr && !entry.path.empty()) {
          SHFILEINFOW info{};
          if (SHGetFileInfoW(entry.path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) != 0) {
            entry.icon = info.hIcon;
          }
        }
        return entry.icon;
      }
      break;
    case Kind::Setting:
      return settings_icon_;
    case Kind::Folder:
      if (match.index >= 0 && match.index < static_cast<int>(files_.size())) {
        FileHit& hit = files_[static_cast<size_t>(match.index)];
        if (hit.icon == nullptr && !hit.path.empty()) {
          SHFILEINFOW info{};
          if (SHGetFileInfoW(hit.path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON | SHGFI_ADDOVERLAYS) !=
              0) {
            hit.icon = info.hIcon;
          }
        }
        return hit.icon != nullptr ? hit.icon : folder_icon_;
      }
      return folder_icon_;
    case Kind::File:
      if (match.index >= 0 && match.index < static_cast<int>(files_.size())) {
        FileHit& hit = files_[static_cast<size_t>(match.index)];
        if (hit.icon == nullptr && !hit.path.empty()) {
          SHFILEINFOW info{};
          if (SHGetFileInfoW(hit.path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON | SHGFI_ADDOVERLAYS) !=
              0) {
            hit.icon = info.hIcon;
          }
        }
        return hit.icon;
      }
      break;
  }
  return nullptr;
}

void Spotlight::QueryFiles(const std::wstring& needle) {
  DestroyFileIcons();
  files_.clear();
  if (needle.empty()) {
    return;
  }
  if (!QueryIndexedFiles(needle, files_)) {
    int remaining = 12;
    const std::wstring lower = LowerCopy(needle);
    const KNOWNFOLDERID folders[] = {FOLDERID_Desktop, FOLDERID_Documents, FOLDERID_Downloads, FOLDERID_Pictures,
                                     FOLDERID_Videos,  FOLDERID_Music};
    for (const KNOWNFOLDERID& id : folders) {
      WalkNamed(KnownFolder(id), lower, files_, 2, remaining);
      if (remaining <= 0) {
        break;
      }
    }
  }
}

void Spotlight::RebuildMatches() {
  matches_.clear();
  if (filter_.empty()) {
    return;
  }
  const std::wstring needle = LowerCopy(filter_);
  std::vector<Match> apps;
  std::vector<Match> settings;
  std::vector<Match> files;

  for (int i = 0; i < static_cast<int>(apps_.size()); ++i) {
    const int score = MatchScore(apps_[static_cast<size_t>(i)].name, needle);
    if (score >= 0) {
      apps.push_back(Match{Kind::App, i, score});
    }
  }
  for (int i = 0; i < static_cast<int>(sizeof(kSettings) / sizeof(kSettings[0])); ++i) {
    std::wstring hay = kSettings[i].title;
    hay.push_back(L' ');
    hay += kSettings[i].aliases;
    const int score = MatchScore(std::move(hay), needle);
    if (score >= 0) {
      settings.push_back(Match{Kind::Setting, i, score});
    }
  }
  for (int i = 0; i < static_cast<int>(files_.size()); ++i) {
    const Kind kind = files_[static_cast<size_t>(i)].folder ? Kind::Folder : Kind::File;
    files.push_back(Match{kind, i, 1});
  }

  auto by_score = [](const Match& a, const Match& b) {
    if (a.score != b.score) {
      return a.score < b.score;
    }
    return a.index < b.index;
  };
  std::sort(apps.begin(), apps.end(), by_score);
  std::sort(settings.begin(), settings.end(), by_score);

  if (apps.size() > static_cast<size_t>(kMaxApps)) {
    apps.resize(static_cast<size_t>(kMaxApps));
  }
  if (settings.size() > static_cast<size_t>(kMaxSettings)) {
    settings.resize(static_cast<size_t>(kMaxSettings));
  }
  if (files.size() > static_cast<size_t>(kMaxFiles)) {
    files.resize(static_cast<size_t>(kMaxFiles));
  }
  matches_.insert(matches_.end(), apps.begin(), apps.end());
  matches_.insert(matches_.end(), settings.begin(), settings.end());
  matches_.insert(matches_.end(), files.begin(), files.end());
}

void Spotlight::LayoutWindow() {
  if (hwnd_ == nullptr) {
    return;
  }
  RebuildMatches();
  const UINT dpi = Dpi();
  const int shadow = DipToPx(kShadowDip, dpi);
  const int width = DipToPx(kWidthDip, dpi) + shadow * 2;
  const int pad = DipToPx(kPadDip, dpi);
  const int search_h = DipToPx(kSearchHeightDip, dpi);
  const int row_h = DipToPx(kRowHeightDip, dpi);

  rows_visible_ = (std::min)(static_cast<int>(matches_.size()), kMaxRows);
  const int list_h = rows_visible_ > 0 ? pad / 2 + rows_visible_ * row_h : 0;
  const int height = shadow * 2 + pad + search_h + list_h + pad;

  POINT cursor{};
  GetCursorPos(&cursor);
  HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
  if (monitor == nullptr) {
    monitor = MonitorFromWindow(owner_ != nullptr ? owner_ : hwnd_, MONITOR_DEFAULTTONEAREST);
  }
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  GetMonitorInfoW(monitor, &info);

  int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2;
  int y = info.rcWork.top + (info.rcWork.bottom - info.rcWork.top) / 5;
  if (x < info.rcWork.left) {
    x = info.rcWork.left;
  }
  if (y < info.rcWork.top) {
    y = info.rcWork.top;
  }
  if (y + height > info.rcWork.bottom) {
    y = (std::max)(info.rcWork.top, info.rcWork.bottom - height);
  }

  SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);

  search_rect_ = {shadow + pad, shadow + pad, width - shadow - pad, shadow + pad + search_h};

  if (edit_font_ != nullptr) {
    DeleteObject(edit_font_);
    edit_font_ = nullptr;
  }
  LOGFONTW lf{};
  lf.lfHeight = -DipToPx(18, dpi);
  lf.lfWeight = FW_NORMAL;
  lf.lfQuality = CLEARTYPE_QUALITY;
  lf.lfCharSet = DEFAULT_CHARSET;
  wcscpy_s(lf.lfFaceName, L"Segoe UI Variable");
  edit_font_ = CreateFontIndirectW(&lf);
  if (edit_font_ != nullptr) {
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_), TRUE);
  }

  const int glyph = DipToPx(kGlyphDip, dpi);
  const int inset_x = DipToPx(12, dpi) + glyph + DipToPx(10, dpi);
  const int inset_y = DipToPx(4, dpi);
  SetWindowPos(edit_, nullptr, search_rect_.left + inset_x, search_rect_.top + inset_y,
               search_rect_.right - search_rect_.left - inset_x - DipToPx(12, dpi), search_h - inset_y * 2,
               SWP_NOZORDER | SWP_NOACTIVATE);

  RebuildRows();
  Present();
}

void Spotlight::RebuildRows() {
  rows_.clear();
  if (hwnd_ == nullptr) {
    return;
  }
  RECT client{};
  GetClientRect(hwnd_, &client);
  const int shadow = Dip(kShadowDip);
  const int pad = Dip(kPadDip);
  const int search_h = Dip(kSearchHeightDip);
  const int row_h = Dip(kRowHeightDip);
  const int icon = Dip(kIconDip);
  const int header_bottom = shadow + pad + search_h + pad / 2;

  const int max_rows = (std::max)(0, rows_visible_);
  const int max_scroll = (std::max)(0, static_cast<int>(matches_.size()) - max_rows) * row_h;
  if (scroll_ > max_scroll) {
    scroll_ = max_scroll;
  }
  if (scroll_ < 0) {
    scroll_ = 0;
  }
  scroll_ = row_h > 0 ? (scroll_ / row_h) * row_h : 0;

  const int first = row_h > 0 ? scroll_ / row_h : 0;
  int y = header_bottom;
  for (int i = 0; i < max_rows && first + i < static_cast<int>(matches_.size()); ++i) {
    Row row{};
    row.rect = {shadow + pad, y, client.right - shadow - pad, y + row_h};
    row.match = matches_[static_cast<size_t>(first + i)];
    const int icon_y = y + (row_h - icon) / 2;
    row.icon_rect = {shadow + pad + Dip(10), icon_y, shadow + pad + Dip(10) + icon, icon_y + icon};
    rows_.push_back(row);
    y += row_h;
  }

  if (hot_ >= static_cast<int>(rows_.size())) {
    hot_ = rows_.empty() ? -1 : static_cast<int>(rows_.size()) - 1;
  }
  if (hot_ < 0 && !rows_.empty()) {
    hot_ = FirstSelectable();
  }
}

void Spotlight::Present() {
  if (hwnd_ == nullptr || !EnsureRenderer()) {
    return;
  }
  RECT window{};
  GetWindowRect(hwnd_, &window);
  const int width = window.right - window.left;
  const int height = window.bottom - window.top;
  if (!EnsureLayer(width, height) || layer_bits_ == nullptr || layer_dc_ == nullptr) {
    return;
  }
  std::memset(layer_bits_, 0, static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

  const RECT bind{0, 0, width, height};
  if (FAILED(rt_->BindDC(layer_dc_, &bind))) {
    rt_.Reset();
    return;
  }
  rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  rt_->BeginDraw();

  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), brush.GetAddressOf());
  const float shadow = static_cast<float>(Dip(kShadowDip));
  const float radius = static_cast<float>(Dip(kCardRadiusDip));
  const float card_l = shadow;
  const float card_t = shadow;
  const float card_r = static_cast<float>(width) - shadow;
  const float card_b = static_cast<float>(height) - shadow;
  if (brush) {
    for (int i = 6; i >= 1; --i) {
      const float o = static_cast<float>(i) * 1.15f;
      brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.028f * static_cast<float>(7 - i)));
      const D2D1_ROUNDED_RECT sh{D2D1::RectF(card_l - o * 0.15f, card_t + o * 0.35f, card_r + o * 0.15f, card_b + o),
                                 radius + o * 0.4f, radius + o * 0.4f};
      rt_->FillRoundedRectangle(sh, brush.Get());
    }
    brush->SetColor(FillColor(dark_));
    const D2D1_ROUNDED_RECT card{D2D1::RectF(card_l, card_t, card_r, card_b), radius, radius};
    rt_->FillRoundedRectangle(card, brush.Get());
  }

  const float search_radius = static_cast<float>(Dip(kSearchRadiusDip));
  if (brush) {
    brush->SetColor(SearchFillColor(dark_));
    const D2D1_ROUNDED_RECT search{
        D2D1::RectF(static_cast<float>(search_rect_.left), static_cast<float>(search_rect_.top),
                    static_cast<float>(search_rect_.right), static_cast<float>(search_rect_.bottom)),
        search_radius, search_radius};
    rt_->FillRoundedRectangle(search, brush.Get());
  }

  if (brush) {
    brush->SetColor(CueColor(dark_));
    const float glyph_r = static_cast<float>(Dip(7));
    const float cx = static_cast<float>(search_rect_.left + Dip(22));
    const float cy = static_cast<float>(search_rect_.top + search_rect_.bottom) * 0.5f;
    DrawMagnifier(rt_.Get(), brush.Get(), D2D1::Point2F(cx, cy), glyph_r);
  }

  if (brush && search_format_ && dwrite_ && edit_ != nullptr) {
    RECT text_rc{};
    GetClientRect(edit_, &text_rc);
    MapWindowPoints(edit_, hwnd_, reinterpret_cast<POINT*>(&text_rc), 2);
    const float tw = static_cast<float>(text_rc.right - text_rc.left);
    const float th = static_cast<float>(text_rc.bottom - text_rc.top);
    if (tw > 1.0f && th > 1.0f) {
      const EditView typed = ReadEditView(edit_);
      const bool cue = typed.text.empty();
      const wchar_t* label = cue ? L"검색" : typed.text.c_str();
      const UINT32 label_len = static_cast<UINT32>(cue ? wcslen(label) : typed.text.size());
      Microsoft::WRL::ComPtr<IDWriteTextLayout> search_layout;
      if (SUCCEEDED(dwrite_->CreateTextLayout(label, label_len, search_format_.Get(), tw, th,
                                              search_layout.GetAddressOf()))) {
        brush->SetColor(cue ? CueColor(dark_) : TextColor(dark_));
        rt_->DrawTextLayout(D2D1::Point2F(static_cast<float>(text_rc.left), static_cast<float>(text_rc.top)),
                            search_layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
        if (GetFocus() == edit_) {
          const UINT32 caret_index = cue ? 0 : typed.caret;
          FLOAT cx = 0.0f;
          FLOAT cy = 0.0f;
          DWRITE_HIT_TEST_METRICS hit{};
          search_layout->HitTestTextPosition(caret_index, FALSE, &cx, &cy, &hit);
          const float caret_h = hit.height > 1.0f ? hit.height : search_format_->GetFontSize();
          const float x0 = static_cast<float>(text_rc.left) + cx;
          const float y0 = static_cast<float>(text_rc.top) + cy;
          brush->SetColor(TextColor(dark_));
          rt_->FillRectangle(D2D1::RectF(x0, y0, x0 + 1.5f, y0 + caret_h), brush.Get());
          SetCaretPos(static_cast<int>(cx + 0.5f), static_cast<int>(cy + 0.5f));
        }
      }
    }
  }

  if (brush && !rows_.empty()) {
    brush->SetColor(DividerColor(dark_));
    const float x1 = static_cast<float>(search_rect_.left);
    const float x2 = static_cast<float>(search_rect_.right);
    const float y = static_cast<float>(search_rect_.bottom + Dip(kPadDip) / 4);
    rt_->DrawLine(D2D1::Point2F(x1, y), D2D1::Point2F(x2, y), brush.Get(), 1.0f);
  }

  for (size_t i = 0; i < rows_.size(); ++i) {
    const Row& row = rows_[i];
    if (static_cast<int>(i) == hot_ && Selectable(row) && brush) {
      brush->SetColor(MenuItemHoverFill(dark_, false));
      const D2D1_ROUNDED_RECT hover{
          D2D1::RectF(static_cast<float>(row.rect.left), static_cast<float>(row.rect.top),
                      static_cast<float>(row.rect.right), static_cast<float>(row.rect.bottom)),
          static_cast<float>(Dip(8)), static_cast<float>(Dip(8))};
      rt_->FillRoundedRectangle(hover, brush.Get());
    }

    std::wstring label;
    std::wstring detail;
    switch (row.match.kind) {
      case Kind::App:
        if (row.match.index >= 0 && row.match.index < static_cast<int>(apps_.size())) {
          label = apps_[static_cast<size_t>(row.match.index)].name;
        }
        break;
      case Kind::Setting:
        if (row.match.index >= 0 &&
            row.match.index < static_cast<int>(sizeof(kSettings) / sizeof(kSettings[0]))) {
          label = kSettings[row.match.index].title;
        }
        break;
      case Kind::File:
      case Kind::Folder:
        if (row.match.index >= 0 && row.match.index < static_cast<int>(files_.size())) {
          label = files_[static_cast<size_t>(row.match.index)].title;
          detail = files_[static_cast<size_t>(row.match.index)].detail;
        }
        break;
    }
    if (detail.empty()) {
      detail = KindLabel(row.match.kind);
    }
    if (!label.empty() && format_ && brush) {
      const float meta_w = static_cast<float>(Dip(72));
      const float text_l = static_cast<float>(row.icon_rect.right + Dip(10));
      const float text_r = static_cast<float>(row.rect.right - Dip(12));
      const float h = static_cast<float>(row.rect.bottom - row.rect.top);
      Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
      if (SUCCEEDED(dwrite_->CreateTextLayout(label.c_str(), static_cast<UINT32>(label.size()), format_.Get(),
                                              (std::max)(8.0f, text_r - text_l - meta_w - 8.0f), h,
                                              layout.GetAddressOf()))) {
        brush->SetColor(TextColor(dark_));
        rt_->DrawTextLayout(D2D1::Point2F(text_l, static_cast<float>(row.rect.top)), layout.Get(), brush.Get(),
                            D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
      }
      if (meta_format_ && !detail.empty()) {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> meta;
        if (SUCCEEDED(dwrite_->CreateTextLayout(detail.c_str(), static_cast<UINT32>(detail.size()), meta_format_.Get(),
                                                meta_w, h, meta.GetAddressOf()))) {
          brush->SetColor(CueColor(dark_));
          rt_->DrawTextLayout(D2D1::Point2F(text_r - meta_w, static_cast<float>(row.rect.top)), meta.Get(), brush.Get(),
                              D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
        }
      }
    }
  }

  const HRESULT hr = rt_->EndDraw();
  if (hr == D2DERR_RECREATE_TARGET) {
    rt_.Reset();
    return;
  }

  for (const Row& row : rows_) {
    if (HICON icon = EnsureIcon(row.match)) {
      const int sz = row.icon_rect.bottom - row.icon_rect.top;
      DrawIconEx(layer_dc_, row.icon_rect.left, row.icon_rect.top, icon, sz, sz, 0, nullptr, DI_NORMAL);
    }
  }

  POINT src{0, 0};
  SIZE size{width, height};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, layer_dc_, &src, 0, &blend, ULW_ALPHA);
}

void Spotlight::PaintEdit(HWND edit) {
  PAINTSTRUCT ps{};
  BeginPaint(edit, &ps);
  EndPaint(edit, &ps);
}

void Spotlight::ApplyChrome() {
  if (hwnd_ == nullptr) {
    return;
  }
  const BOOL dark = dark_ ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd_, dwm::kUseImmersiveDarkMode, &dark, sizeof(dark));
  const int backdrop = dwm::kBackdropNone;
  DwmSetWindowAttribute(hwnd_, dwm::kSystemBackdropType, &backdrop, sizeof(backdrop));
  const int corner = dwm::kCornerDoNotRound;
  DwmSetWindowAttribute(hwnd_, dwm::kWindowCornerPreference, &corner, sizeof(corner));
  const MARGINS margins{0, 0, 0, 0};
  DwmExtendFrameIntoClientArea(hwnd_, &margins);
  LONG style = GetWindowLongW(hwnd_, GWL_STYLE);
  if ((style & WS_CLIPCHILDREN) != 0) {
    SetWindowLongW(hwnd_, GWL_STYLE, style & ~WS_CLIPCHILDREN);
  }
  if (edit_ != nullptr) {
    SetWindowTheme(edit_, L"", L"");
    MakeEditInputOnly(edit_);
  }
  if (search_brush_ != nullptr) {
    DeleteObject(search_brush_);
    search_brush_ = nullptr;
  }
  search_brush_ = CreateSolidBrush(dark_ ? kSearchFillDark : kSearchFillLight);
}

void Spotlight::ApplyFilter() {
  filter_.clear();
  if (edit_ != nullptr) {
    filter_ = ReadEditView(edit_).text;
  }
  if (hwnd_ != nullptr) {
    KillTimer(hwnd_, kFileSearchTimer);
  }
  DestroyFileIcons();
  files_.clear();
  scroll_ = 0;
  hot_ = filter_.empty() ? -1 : 0;
  LayoutWindow();
  if (hwnd_ != nullptr && !filter_.empty()) {
    SetTimer(hwnd_, kFileSearchTimer, kFileSearchDelayMs, nullptr);
  }
}

void Spotlight::UpdateHot(POINT client) {
  TRACKMOUSEEVENT track{};
  track.cbSize = sizeof(track);
  track.dwFlags = TME_LEAVE;
  track.hwndTrack = hwnd_;
  TrackMouseEvent(&track);

  int next = hot_;
  bool hit = false;
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (PtInRect(&rows_[i].rect, client) && Selectable(rows_[i])) {
      next = static_cast<int>(i);
      hit = true;
      break;
    }
  }
  if (!hit || next == hot_) {
    return;
  }
  hot_ = next;
  Present();
}

void Spotlight::MoveHot(int delta) {
  if (matches_.empty()) {
    hot_ = -1;
    return;
  }
  const int row_h = Dip(kRowHeightDip);
  const int visible = (std::max)(1, rows_visible_);
  int first = row_h > 0 ? scroll_ / row_h : 0;
  int index = first + (hot_ < 0 ? (delta > 0 ? -1 : 0) : hot_);
  index += delta;
  if (index < 0) {
    index = 0;
  }
  if (index >= static_cast<int>(matches_.size())) {
    index = static_cast<int>(matches_.size()) - 1;
  }
  if (index < first) {
    first = index;
  } else if (index >= first + visible) {
    first = index - visible + 1;
  }
  scroll_ = first * row_h;
  RebuildRows();
  hot_ = index - first;
  Present();
}

void Spotlight::ActivateMatch(const Match& match) {
  switch (match.kind) {
    case Kind::App:
      if (match.index >= 0 && match.index < static_cast<int>(apps_.size())) {
        LaunchPath(apps_[static_cast<size_t>(match.index)].path);
      }
      break;
    case Kind::Setting:
      if (match.index >= 0 && match.index < static_cast<int>(sizeof(kSettings) / sizeof(kSettings[0]))) {
        LaunchPath(kSettings[match.index].uri);
      }
      break;
    case Kind::File:
    case Kind::Folder:
      if (match.index >= 0 && match.index < static_cast<int>(files_.size())) {
        LaunchPath(files_[static_cast<size_t>(match.index)].path);
      }
      break;
  }
  Hide();
}

void Spotlight::ActivateHot() {
  if (rows_.empty()) {
    return;
  }
  int index = hot_;
  if (index < 0 || index >= static_cast<int>(rows_.size())) {
    index = FirstSelectable();
  }
  if (index < 0 || index >= static_cast<int>(rows_.size())) {
    return;
  }
  ActivateMatch(rows_[static_cast<size_t>(index)].match);
}

void Spotlight::LaunchPath(const std::wstring& path) {
  if (path.empty()) {
    return;
  }
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"open";
  info.lpFile = path.c_str();
  info.nShow = SW_SHOWNORMAL;
  ShellExecuteExW(&info);
}

const Spotlight::Row* Spotlight::HitTest(POINT client) const {
  for (const auto& row : rows_) {
    if (PtInRect(&row.rect, client)) {
      return &row;
    }
  }
  return nullptr;
}

bool Spotlight::Selectable(const Row&) const {
  return true;
}

int Spotlight::FirstSelectable() const {
  return rows_.empty() ? -1 : 0;
}

UINT Spotlight::Dpi() const {
  HWND source = hwnd_ != nullptr ? hwnd_ : owner_;
  if (source == nullptr) {
    return 96;
  }
  const UINT dpi = GetDpiForWindow(source);
  return dpi == 0 ? 96 : dpi;
}

int Spotlight::Dip(int value) const {
  return DipToPx(value, Dpi());
}

}  // namespace bamti
