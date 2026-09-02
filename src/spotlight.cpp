#include "spotlight.hpp"

#include "dwm.hpp"
#include "log.hpp"
#include "theme.hpp"
#include "watchdog.hpp"

#include <d2d1helper.h>
#include <dwmapi.h>
#include <imm.h>
#include <knownfolders.h>
#include <propkey.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shobjidl_core.h>
#include <structuredquery.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <thread>
#include <unordered_set>
#include <utility>

namespace bamti {
namespace {

constexpr int kWidthDip = 680;
constexpr int kShadowDip = 18;
constexpr int kPadDip = 12;
constexpr int kSearchHeightDip = 48;
constexpr int kRowHeightDip = 36;
constexpr int kHeaderHeightDip = 24;
constexpr int kMaxListDip = 420;
constexpr int kIconDip = 28;
constexpr int kMaxApps = 4;
constexpr int kMaxSettings = 3;
constexpr int kMaxFolders = 4;
constexpr int kMaxDocuments = 4;
constexpr int kGlyphDip = 18;
constexpr UINT_PTR kFileSearchTimer = 1;
constexpr UINT kFileSearchDoneMsg = WM_APP + 40;
constexpr UINT kIconReadyMsg = WM_APP + 41;
constexpr UINT kAppsReadyMsg = WM_APP + 42;
constexpr UINT kFileSearchDelayMs = 40;
constexpr ULONGLONG kWalkBudgetMs = 45;
// 앱 설치와 제거는 드문 사건이다. 1분마다 다시 읽으면 열 때마다 재열거하는 것과 같다.
constexpr ULONGLONG kAppReloadMs = 600000;
UINT g_file_search_count = 0;
ULONGLONG g_file_search_window = 0;
UINT g_icon_ready_count = 0;
ULONGLONG g_icon_ready_window = 0;
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

std::wstring TrimCopy(std::wstring text) {
  while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) {
    text.erase(text.begin());
  }
  while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) {
    text.pop_back();
  }
  return text;
}

bool WordBreak(wchar_t ch) {
  return ch == L' ' || ch == L'-' || ch == L'_' || ch == L'.' || ch == L'\\' || ch == L'/' || ch == L'[' ||
         ch == L'(' || ch == L'+';
}

int MatchScore(std::wstring hay, const std::wstring& needle) {
  if (needle.empty()) {
    return -1;
  }
  hay = LowerCopy(std::move(hay));
  if (hay == needle) {
    return 0;
  }
  if (hay.size() >= needle.size() && hay.compare(0, needle.size(), needle) == 0) {
    return 1;
  }
  size_t pos = hay.find(needle);
  while (pos != std::wstring::npos) {
    if (pos > 0 && WordBreak(hay[pos - 1])) {
      return 2;
    }
    pos = hay.find(needle, pos + 1);
  }
  if (hay.find(needle) == std::wstring::npos) {
    return -1;
  }
  if (needle.size() <= 2) {
    return -1;
  }
  return 4;
}

int MatchQuery(const std::wstring& hay, const std::wstring& needle) {
  const int direct = MatchScore(hay, needle);
  if (direct >= 0) {
    return direct;
  }
  int worst = 0;
  bool any = false;
  std::wstring token;
  for (const wchar_t ch : needle) {
    if (ch == L' ' || ch == L'\t') {
      if (!token.empty()) {
        const int part = MatchScore(hay, token);
        if (part < 0) {
          return -1;
        }
        worst = (std::max)(worst, part);
        any = true;
        token.clear();
      }
    } else {
      token.push_back(ch);
    }
  }
  if (!token.empty()) {
    const int part = MatchScore(hay, token);
    if (part < 0) {
      return -1;
    }
    worst = (std::max)(worst, part);
    any = true;
  }
  return any ? 6 + worst : -1;
}

// 별칭으로 걸린 앱은 이름으로 걸린 앱보다 항상 뒤에 놓는다. 점수는 작을수록 앞이다.
constexpr int kAliasPenalty = 20;

int MatchApp(const Spotlight::AppEntry& app, const std::wstring& needle) {
  const int direct = MatchQuery(app.name, needle);
  if (direct >= 0) {
    return direct;
  }
  int best = -1;
  for (const std::wstring& key : app.keys) {
    const int one = MatchQuery(key, needle);
    if (one >= 0 && (best < 0 || one < best)) {
      best = one;
    }
  }
  return best < 0 ? -1 : best + kAliasPenalty;
}

bool LooksLikeHangul(const std::wstring& text) {
  for (const wchar_t ch : text) {
    if ((ch >= 0xAC00 && ch <= 0xD7A3) || (ch >= 0x1100 && ch <= 0x11FF) || (ch >= 0x3130 && ch <= 0x318F)) {
      return true;
    }
  }
  return false;
}

std::wstring IndexedSearchQuery(const std::wstring& needle) {
  std::wstring query;
  std::wstring token;
  auto flush = [&]() {
    if (token.empty()) {
      return;
    }
    std::wstring escaped;
    escaped.reserve(token.size() + 4);
    for (const wchar_t ch : token) {
      if (ch == L'"') {
        escaped.append(L"\"\"");
      } else {
        escaped.push_back(ch);
      }
    }
    if (!query.empty()) {
      query += L" AND ";
    }
    query += L"System.FileName:~~\"";
    query += escaped;
    query += L"\"";
    token.clear();
  };
  for (const wchar_t ch : needle) {
    if (ch == L' ' || ch == L'\t') {
      flush();
    } else {
      token.push_back(ch);
    }
  }
  flush();
  return query;
}

std::wstring FileLeaf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// 검색 보조 키를 뽑는다. 중복과 빈 값은 넣지 않고, 모두 소문자로 저장한다.
void AppendSearchKey(std::vector<std::wstring>& keys, std::wstring value) {
  value = LowerCopy(TrimCopy(std::move(value)));
  if (value.size() < 2) {
    return;
  }
  for (const std::wstring& one : keys) {
    if (one == value) {
      return;
    }
  }
  keys.push_back(std::move(value));
}

// 식별자에서 별칭을 뽑는다. 패키지 앱의 AUMID(Publisher.Name_해시!진입점)와
// 그 변형(진입점이나 해시가 없는 형태)을 함께 다룬다.
void AppendFamilyKeys(std::vector<std::wstring>& keys, const std::wstring& id) {
  std::wstring family = id;
  const size_t bang = family.find(L'!');
  if (bang != std::wstring::npos) {
    family.resize(bang);
  }
  const size_t underscore = family.rfind(L'_');
  if (underscore != std::wstring::npos) {
    family.resize(underscore);
  }
  AppendSearchKey(keys, family);
  // 마지막 마디가 앱 이름이다. "Microsoft.Windows.RemoteDesktop"의 "RemoteDesktop",
  // "Microsoft.WindowsCalculator"의 "WindowsCalculator"가 여기서 나온다.
  const size_t dot = family.rfind(L'.');
  if (dot != std::wstring::npos) {
    AppendSearchKey(keys, family.substr(dot + 1));
  }
}

// 파일 경로에서 별칭을 뽑는다. 실행 파일 이름을 확장자와 함께, 그리고 확장자 없이 넣는다.
void AppendLeafKeys(std::vector<std::wstring>& keys, const std::wstring& path) {
  const std::wstring leaf = FileLeaf(path);
  if (leaf.empty()) {
    return;
  }
  AppendSearchKey(keys, leaf);
  const size_t dot = leaf.rfind(L'.');
  if (dot != std::wstring::npos && dot > 0) {
    AppendSearchKey(keys, leaf.substr(0, dot));
  }
}

void CollectSearchKeys(const std::wstring& id, const std::wstring& target, std::vector<std::wstring>& keys) {
  // 식별자가 경로가 아니면 패키지 계열이다. 진입점이 붙지 않은 형태도 여기에 들어온다.
  if (id.find_first_of(L"\\/:") == std::wstring::npos) {
    AppendFamilyKeys(keys, id);
  } else {
    AppendLeafKeys(keys, id);
  }
  // 대상 실행 파일은 종류를 가리지 않고 언제나 태운다. 표시 이름이 번역된 시스템
  // 도구는 이 경로로만 영어 이름을 얻는다(예: 원격 데스크톱 연결 → mstsc).
  if (!target.empty()) {
    AppendLeafKeys(keys, target);
  }
}

bool FilenameContains(const Spotlight::FileHit& hit, const std::wstring& needle) {
  const std::wstring lower_needle = LowerCopy(needle);
  if (LowerCopy(hit.title).find(lower_needle) != std::wstring::npos) {
    return true;
  }
  return LowerCopy(FileLeaf(hit.path)).find(lower_needle) != std::wstring::npos;
}

int FileHitScore(const Spotlight::FileHit& hit, const std::wstring& needle) {
  const int title = MatchQuery(hit.title, needle);
  if (title >= 0) {
    return title;
  }
  return MatchQuery(FileLeaf(hit.path), needle);
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
  return lstrcmpiW(a.c_str(), b.c_str()) == 0;
}

void AppendUniqueHit(std::vector<Spotlight::FileHit>& out, Spotlight::FileHit&& hit) {
  if (hit.path.empty()) {
    return;
  }
  for (const auto& existing : out) {
    if (SamePath(existing.path, hit.path)) {
      return;
    }
  }
  out.push_back(std::move(hit));
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
  static constexpr const wchar_t* kSkip[] = {
      L"node_modules",
      L".git",
      L"AppData",
      L"Windows",
      L"Windows.old",
      L"Program Files",
      L"Program Files (x86)",
      L"ProgramData",
      L"$Recycle.Bin",
      L"System Volume Information",
      L"Recovery",
      L"PerfLogs",
      L"$WINDOWS.~BT",
      L"$WINDOWS.~WS",
  };
  for (const wchar_t* skip : kSkip) {
    if (lstrcmpiW(name, skip) == 0) {
      return true;
    }
  }
  return false;
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

std::vector<std::wstring> FixedDriveRoots() {
  std::vector<std::wstring> roots;
  const DWORD mask = GetLogicalDrives();
  wchar_t root[] = L"A:\\";
  for (int i = 0; i < 26; ++i) {
    if ((mask & (1u << i)) == 0) {
      continue;
    }
    root[0] = static_cast<wchar_t>(L'A' + i);
    if (GetDriveTypeW(root) != DRIVE_FIXED) {
      continue;
    }
    roots.emplace_back(root);
  }
  return roots;
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
    case Spotlight::Kind::Header:
      return L"";
    case Spotlight::Kind::App:
      return L"응용 프로그램";
    case Spotlight::Kind::Setting:
      return L"시스템 설정";
    case Spotlight::Kind::File:
      return L"문서";
    case Spotlight::Kind::Folder:
      return L"폴더";
  }
  return L"";
}

const wchar_t* SectionTitle(int index) {
  switch (index) {
    case 0:
      return L"최고 순위";
    case 1:
      return L"응용 프로그램";
    case 2:
      return L"시스템 설정";
    case 3:
      return L"폴더";
    case 4:
      return L"문서";
    default:
      return L"";
  }
}

int KindRank(Spotlight::Kind kind) {
  switch (kind) {
    case Spotlight::Kind::App:
      return 0;
    case Spotlight::Kind::Folder:
      return 1;
    case Spotlight::Kind::Setting:
      return 2;
    case Spotlight::Kind::File:
      return 3;
    default:
      return 9;
  }
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
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_PASTE:
    case WM_CUT:
    case WM_CLEAR:
    case WM_UNDO:
      return true;
    case WM_KEYDOWN:
      switch (wparam) {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_HOME:
        case VK_END:
        case VK_DELETE:
          return true;
        default:
          return false;
      }
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

EditView ReadEditView(HWND edit, const std::wstring& ime_comp, LONG ime_cursor) {
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
  if (!ime_comp.empty()) {
    if (start > view.text.size()) {
      start = static_cast<DWORD>(view.text.size());
    }
    view.text.replace(start, end - start, ime_comp);
    LONG cursor = ime_cursor;
    if (cursor < 0) {
      cursor = static_cast<LONG>(ime_comp.size());
    }
    if (cursor > static_cast<LONG>(ime_comp.size())) {
      cursor = static_cast<LONG>(ime_comp.size());
    }
    view.caret = start + static_cast<UINT32>(cursor);
  }
  if (view.caret > view.text.size()) {
    view.caret = static_cast<UINT32>(view.text.size());
  }
  return view;
}

std::wstring CompositionString(HIMC himc, DWORD index) {
  const LONG bytes = ImmGetCompositionStringW(himc, index, nullptr, 0);
  if (bytes <= 0) {
    return {};
  }
  std::wstring text(static_cast<size_t>(bytes / sizeof(wchar_t)), L'\0');
  ImmGetCompositionStringW(himc, index, text.data(), bytes);
  while (!text.empty() && text.back() == L'\0') {
    text.pop_back();
  }
  return text;
}

struct RawApp {
  std::wstring name;
  std::wstring id;
  std::wstring target;
};

void CollectFolder(const std::wstring& root, std::vector<RawApp>& out, std::unordered_set<std::wstring>& seen) {
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
    RawApp app;
    app.name = std::move(name);
    app.id = full;
    app.target = full;
    out.push_back(std::move(app));
  } while (FindNextFileW(find, &fd) != FALSE);
  FindClose(find);
}

bool LooksLikeFilesystemPath(const std::wstring& text) {
  if (text.size() >= 3 && text[1] == L':' && (text[2] == L'\\' || text[2] == L'/')) {
    return true;
  }
  return text.size() >= 2 && text[0] == L'\\' && text[1] == L'\\';
}

double SpotlightElapsedMs(const LARGE_INTEGER& freq, const LARGE_INTEGER& t0, const LARGE_INTEGER& t1) {
  if (freq.QuadPart == 0) {
    return 0.0;
  }
  return static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

HICON IconFromShellItem(IShellItem* item) {
  if (item == nullptr) {
    return nullptr;
  }
  Microsoft::WRL::ComPtr<IShellItemImageFactory> factory;
  if (FAILED(item->QueryInterface(IID_PPV_ARGS(&factory))) || !factory) {
    return nullptr;
  }
  HBITMAP color = nullptr;
  const SIZE px{48, 48};
  if (FAILED(factory->GetImage(px, SIIGBF_ICONBACKGROUND | SIIGBF_BIGGERSIZEOK, &color)) || color == nullptr) {
    return nullptr;
  }
  BITMAP bm{};
  if (GetObjectW(color, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight == 0) {
    DeleteObject(color);
    return nullptr;
  }
  const int w = bm.bmWidth;
  const int h = std::abs(bm.bmHeight);
  const HBITMAP mask = CreateBitmap(w, h, 1, 1, nullptr);
  ICONINFO info{};
  info.fIcon = TRUE;
  info.hbmMask = mask;
  info.hbmColor = color;
  const HICON icon = CreateIconIndirect(&info);
  if (mask != nullptr) {
    DeleteObject(mask);
  }
  DeleteObject(color);
  return icon;
}

HICON ExtractAppsFolderIcon(const std::wstring& app_id) {
  if (app_id.empty()) {
    return nullptr;
  }
  const std::wstring parsing = L"shell:AppsFolder\\" + app_id;
  Microsoft::WRL::ComPtr<IShellItem> item;
  if (FAILED(SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(&item))) || !item) {
    return nullptr;
  }
  return IconFromShellItem(item.Get());
}

bool CollectAppsFolder(std::vector<RawApp>& out, std::unordered_set<std::wstring>& seen) {
  Microsoft::WRL::ComPtr<IShellItem> apps;
  HRESULT hr = SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY, nullptr, IID_PPV_ARGS(&apps));
  if (FAILED(hr) || !apps) {
    return false;
  }
  Microsoft::WRL::ComPtr<IEnumShellItems> e;
  hr = apps->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&e));
  if (FAILED(hr) || !e) {
    return false;
  }
  Microsoft::WRL::ComPtr<IShellItem> item;
  ULONG fetched = 0;
  while (e->Next(1, item.ReleaseAndGetAddressOf(), &fetched) == S_OK && fetched == 1) {
    PWSTR name = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) || name == nullptr) {
      continue;
    }
    PWSTR id = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_PARENTRELATIVEFORADDRESSBAR, &id)) || id == nullptr) {
      CoTaskMemFree(name);
      continue;
    }
    std::wstring key = LowerCopy(name);
    if (seen.insert(key).second && name[0] != L'\0' && id[0] != L'\0') {
      // 데스크톱 앱은 표시 이름이 번역되어 있어 영어 질의와 겹치지 않는다. 실행 파일
      // 이름을 별칭으로 쓰기 위해 바로 가기가 가리키는 대상을 풀어 둔다.
      std::wstring target;
      Microsoft::WRL::ComPtr<IShellItem2> item2;
      if (SUCCEEDED(item.As(&item2)) && item2) {
        PWSTR value = nullptr;
        if (SUCCEEDED(item2->GetString(PKEY_Link_TargetParsingPath, &value)) && value != nullptr) {
          target = value;
          CoTaskMemFree(value);
        }
      }
      if (target.empty()) {
        PWSTR parsing = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &parsing)) && parsing != nullptr) {
          target = parsing;
          CoTaskMemFree(parsing);
        }
      }
      RawApp app;
      app.name = name;
      app.id = id;
      app.target = std::move(target);
      out.push_back(std::move(app));
    }
    CoTaskMemFree(id);
    CoTaskMemFree(name);
  }
  return true;
}

void WalkNamed(const std::wstring& dir, const std::wstring& needle, std::vector<Spotlight::FileHit>& out, int depth,
               int& remaining, bool folders_only, ULONGLONG deadline) {
  if (remaining <= 0 || dir.empty() || depth < 0 || GetTickCount64() >= deadline) {
    return;
  }
  WIN32_FIND_DATAW fd{};
  const HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (find == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    if (remaining <= 0 || GetTickCount64() >= deadline) {
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
      WalkNamed(full, needle, out, depth - 1, remaining, folders_only, deadline);
    }
    if (folders_only && !is_dir) {
      continue;
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
    const size_t before = out.size();
    AppendUniqueHit(out, std::move(hit));
    if (out.size() > before) {
      --remaining;
    }
  } while (FindNextFileW(find, &fd) != FALSE);
  FindClose(find);
}

void TryAddExistingPath(const std::wstring& full, std::vector<Spotlight::FileHit>& out) {
  if (full.empty()) {
    return;
  }
  const DWORD attr = GetFileAttributesW(full.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES) {
    return;
  }
  Spotlight::FileHit hit;
  hit.title = FileLeaf(full);
  hit.path = full;
  hit.folder = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
  hit.detail = ParentName(full);
  AppendUniqueHit(out, std::move(hit));
}

void ProbeExactLocations(const std::wstring& name, std::vector<Spotlight::FileHit>& out) {
  if (name.empty() || name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
    return;
  }
  const KNOWNFOLDERID ids[] = {FOLDERID_Desktop, FOLDERID_Documents, FOLDERID_Downloads, FOLDERID_Profile};
  for (const KNOWNFOLDERID& id : ids) {
    const std::wstring root = KnownFolder(id);
    if (!root.empty()) {
      TryAddExistingPath(root + L'\\' + name, out);
    }
  }
  for (const std::wstring& drive : FixedDriveRoots()) {
    TryAddExistingPath(drive + name, out);
    WIN32_FIND_DATAW fd{};
    const HANDLE find = FindFirstFileW((drive + L"*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
      continue;
    }
    do {
      if (fd.cFileName[0] == L'.' &&
          (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
        continue;
      }
      if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
          (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0 || SkipWalkDir(fd.cFileName)) {
        continue;
      }
      TryAddExistingPath(drive + fd.cFileName + L'\\' + name, out);
    } while (FindNextFileW(find, &fd) != FALSE);
    FindClose(find);
  }
}

std::vector<Spotlight::FileHit> RankFileHits(std::vector<Spotlight::FileHit> mixed, const std::wstring& needle) {
  std::vector<Spotlight::FileHit> folders;
  std::vector<Spotlight::FileHit> files;
  folders.reserve(mixed.size());
  files.reserve(mixed.size());
  for (auto& hit : mixed) {
    if (hit.folder) {
      folders.push_back(std::move(hit));
    } else {
      files.push_back(std::move(hit));
    }
  }

  auto by_name = [&](const Spotlight::FileHit& a, const Spotlight::FileHit& b) {
    const int sa = FileHitScore(a, needle);
    const int sb = FileHitScore(b, needle);
    if (sa != sb) {
      return sa < sb;
    }
    if (a.title.size() != b.title.size()) {
      return a.title.size() < b.title.size();
    }
    return a.path < b.path;
  };
  std::sort(folders.begin(), folders.end(), by_name);
  std::sort(files.begin(), files.end(), by_name);

  std::vector<Spotlight::FileHit> out;
  out.reserve(folders.size() + files.size());
  for (auto& hit : folders) {
    AppendUniqueHit(out, std::move(hit));
  }
  for (auto& hit : files) {
    AppendUniqueHit(out, std::move(hit));
  }
  return out;
}

std::vector<Spotlight::FileHit> CollectQuickHits(const std::wstring& needle) {
  const std::wstring query = TrimCopy(needle);
  if (query.empty()) {
    return {};
  }
  std::vector<Spotlight::FileHit> mixed;
  ProbeExactLocations(query, mixed);
  const std::wstring lower = LowerCopy(query);
  const ULONGLONG deadline = GetTickCount64() + 25;
  int remaining = 8;
  const KNOWNFOLDERID roots[] = {FOLDERID_Desktop, FOLDERID_Documents, FOLDERID_Downloads};
  for (const KNOWNFOLDERID& id : roots) {
    WalkNamed(KnownFolder(id), lower, mixed, 2, remaining, true, deadline);
    if (remaining <= 0 || GetTickCount64() >= deadline) {
      break;
    }
  }
  return RankFileHits(std::move(mixed), lower);
}

bool QueryIndexedHits(const std::wstring& aqs, const std::wstring& needle, bool filename_only, bool folders_only,
                      std::vector<Spotlight::FileHit>& out, int max_items);

std::vector<Spotlight::FileHit> CollectSlowHits(const std::wstring& needle, std::vector<Spotlight::FileHit> mixed) {
  const std::wstring query = TrimCopy(needle);
  if (query.empty()) {
    return RankFileHits(std::move(mixed), query);
  }
  const std::wstring lower = LowerCopy(query);
  QueryIndexedHits(IndexedSearchQuery(query), query, true, false, mixed, 12);
  const ULONGLONG deadline = GetTickCount64() + kWalkBudgetMs;
  int remaining = 8;
  for (const std::wstring& drive : FixedDriveRoots()) {
    if (GetTickCount64() >= deadline) {
      break;
    }
    WalkNamed(drive, lower, mixed, 1, remaining, false, deadline);
    if (remaining <= 0) {
      break;
    }
  }
  return RankFileHits(std::move(mixed), lower);
}

bool QueryIndexedHits(const std::wstring& aqs, const std::wstring& needle, bool filename_only, bool folders_only,
                      std::vector<Spotlight::FileHit>& out, int max_items) {
  if (aqs.empty() || max_items <= 0) {
    return false;
  }
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
  if (FAILED(parser->Parse(aqs.c_str(), nullptr, &solution)) || !solution) {
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
  if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&profile))) &&
      profile) {
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

  const size_t before = out.size();
  for (int n = 0; n < 32 && static_cast<int>(out.size()) < max_items; ++n) {
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
    if (filename_only && !FilenameContains(hit, needle)) {
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
    if (folders_only && !hit.folder) {
      continue;
    }
    hit.detail = ParentName(hit.path);
    AppendUniqueHit(out, std::move(hit));
  }
  return out.size() > before;
}

struct FileSearchPayload {
  uint64_t gen = 0;
  std::wstring needle;
  std::vector<Spotlight::FileHit> hits;
};

struct AppsPayload {
  uint64_t gen = 0;
  std::vector<RawApp> raw;
};

struct IconPayload {
  uint64_t gen = 0;
  std::wstring path;
  HICON icon = nullptr;
};

HICON ExtractShellIcon(const std::wstring& path, bool overlay) {
  if (path.empty()) {
    return nullptr;
  }
  const ULONGLONG started = GetTickCount64();
  SHFILEINFOW info{};
  UINT flags = SHGFI_ICON | SHGFI_LARGEICON;
  if (overlay) {
    flags |= SHGFI_ADDOVERLAYS;
  }
  HICON icon = nullptr;
  if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), flags) != 0) {
    icon = info.hIcon;
  }
  const unsigned ms = static_cast<unsigned>(GetTickCount64() - started);
  if (ms > 10) {
    Log(L"spotlight", L"icon %ums path=%s", ms, path.c_str());
  }
  return icon;
}

void DiscardHitIcons(std::vector<Spotlight::FileHit>& hits) {
  for (auto& hit : hits) {
    if (hit.icon != nullptr) {
      DestroyIcon(hit.icon);
      hit.icon = nullptr;
    }
  }
}

void ExtractHitIcons(std::vector<Spotlight::FileHit>& hits) {
  for (auto& hit : hits) {
    if (hit.icon == nullptr && !hit.path.empty()) {
      hit.icon = ExtractShellIcon(hit.path, true);
    }
  }
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
  WaitForFileSearches();
  Hide();
  DestroyAppIcons();
  DestroyFileIcons();
  DestroyIconCache();
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
  edit_font_dpi_ = 0;
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
  ClearIme();
  search_gen_.fetch_add(1, std::memory_order_acq_rel);
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
  search_gen_.fetch_add(1, std::memory_order_acq_rel);
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
  ClearIme();
  if (hwnd_ != nullptr) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void Spotlight::ClearIme() {
  ime_comp_.clear();
  ime_cursor_ = 0;
  swallow_ime_commit_ = false;
}

LRESULT Spotlight::HandleImeMessage(HWND edit, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_IME_SETCONTEXT:
      lparam &= ~ISC_SHOWUICOMPOSITIONWINDOW;
      return CallWindowProcW(edit_prev_, edit, msg, wparam, lparam);
    case WM_IME_STARTCOMPOSITION:
      ime_comp_.clear();
      ime_cursor_ = 0;
      Present();
      return 0;
    case WM_IME_ENDCOMPOSITION:
      if (!ime_comp_.empty()) {
        ime_comp_.clear();
        ime_cursor_ = 0;
        ApplyFilter();
      } else {
        Present();
      }
      return 0;
    case WM_IME_COMPOSITION:
      HandleImeComposition(lparam);
      return 0;
    case WM_IME_CHAR:
      return 0;
    case WM_IME_REQUEST:
      if (wparam == IMR_COMPOSITIONWINDOW && lparam != 0) {
        auto* form = reinterpret_cast<COMPOSITIONFORM*>(lparam);
        form->dwStyle = CFS_RECT;
        form->ptCurrentPos = {0, 0};
        form->rcArea = {0, 0, 0, 0};
        return 1;
      }
      break;
    case WM_IME_NOTIFY:
      if (wparam == IMN_SETCOMPOSITIONWINDOW) {
        return 0;
      }
      break;
    default:
      break;
  }
  return CallWindowProcW(edit_prev_, edit, msg, wparam, lparam);
}

void Spotlight::HandleImeComposition(LPARAM lparam) {
  if (edit_ == nullptr) {
    return;
  }
  const HIMC himc = ImmGetContext(edit_);
  if (himc == nullptr) {
    return;
  }
  if ((lparam & GCS_RESULTSTR) != 0) {
    const std::wstring result = CompositionString(himc, GCS_RESULTSTR);
    ime_comp_.clear();
    ime_cursor_ = 0;
    swallow_ime_commit_ = true;
    if (!result.empty()) {
      DWORD start = 0;
      DWORD end = 0;
      SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
      SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(result.c_str()));
      const DWORD pos = start + static_cast<DWORD>(result.size());
      SendMessageW(edit_, EM_SETSEL, pos, pos);
    }
  }
  if ((lparam & GCS_COMPSTR) != 0) {
    ime_comp_ = CompositionString(himc, GCS_COMPSTR);
    LONG cursor = ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0);
    if (cursor < 0 || static_cast<size_t>(cursor) > ime_comp_.size() ||
        (cursor == 0 && LooksLikeHangul(ime_comp_))) {
      cursor = static_cast<LONG>(ime_comp_.size());
    }
    ime_cursor_ = cursor;
  } else if ((lparam & GCS_RESULTSTR) != 0) {
    ime_comp_.clear();
    ime_cursor_ = 0;
  }
  ImmReleaseContext(edit_, himc);
  ApplyFilter();
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
  if (msg == WM_CHAR && self->swallow_ime_commit_) {
    self->swallow_ime_commit_ = false;
    if (wparam > 0x7F) {
      return 0;
    }
  }
  // WM_KEYDOWN에서 처리한 키라도 TranslateMessage가 만든 WM_CHAR는 따로 도착한다.
  // 편집 컨트롤은 ESC(0x1B)와 Enter(0x0D)를 입력할 수 없는 문자로 보고 MessageBeep을
  // 울리므로 여기서 삼킨다.
  if (msg == WM_CHAR && (wparam == 0x1B || wparam == 0x0D)) {
    return 0;
  }
  switch (msg) {
    case WM_IME_SETCONTEXT:
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_ENDCOMPOSITION:
    case WM_IME_COMPOSITION:
    case WM_IME_CHAR:
    case WM_IME_REQUEST:
    case WM_IME_NOTIFY:
      return self->HandleImeMessage(hwnd, msg, wparam, lparam);
    default:
      break;
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
      }
      return 0;
    case kFileSearchDoneMsg:
      NotePostedStorm(L"file-search", g_file_search_count, g_file_search_window);
      AcceptFileHits(reinterpret_cast<void*>(lparam));
      return 0;
    case kIconReadyMsg:
      NotePostedStorm(L"icon-ready", g_icon_ready_count, g_icon_ready_window);
      AcceptIcon(reinterpret_cast<void*>(lparam));
      return 0;
    case kAppsReadyMsg:
      AcceptApps(reinterpret_cast<void*>(lparam));
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
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        scroll_ -= delta;
        if (scroll_ < 0) {
          scroll_ = 0;
        }
        const int last = (std::max)(0, static_cast<int>(matches_.size()) - 1);
        if (scroll_ > last) {
          scroll_ = last;
        }
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
  if (!format_ || !search_format_ || !meta_format_ || !header_format_ || font_dpi_ != Dpi()) {
    font_dpi_ = Dpi();
    format_.Reset();
    search_format_.Reset();
    meta_format_.Reset();
    header_format_.Reset();
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) == 0) {
      wcscpy_s(locale, L"en-US");
    }
    const float row_size = 14.0f * static_cast<float>(font_dpi_) / 96.0f;
    const float search_size = 18.0f * static_cast<float>(font_dpi_) / 96.0f;
    const float meta_size = 11.0f * static_cast<float>(font_dpi_) / 96.0f;
    const wchar_t* families[] = {L"Segoe UI Variable", L"Segoe UI"};
    auto make_format = [&](float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out) -> HRESULT {
      HRESULT hr = E_FAIL;
      for (const wchar_t* family : families) {
        hr = dwrite_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, size, locale, out);
        if (SUCCEEDED(hr)) {
          break;
        }
      }
      return hr;
    };
    if (FAILED(make_format(row_size, DWRITE_FONT_WEIGHT_NORMAL, format_.ReleaseAndGetAddressOf())) || !format_) {
      return false;
    }
    if (FAILED(make_format(search_size, DWRITE_FONT_WEIGHT_NORMAL, search_format_.ReleaseAndGetAddressOf())) ||
        !search_format_) {
      return false;
    }
    if (FAILED(make_format(meta_size, DWRITE_FONT_WEIGHT_NORMAL, meta_format_.ReleaseAndGetAddressOf())) ||
        !meta_format_) {
      return false;
    }
    if (FAILED(make_format(meta_size, DWRITE_FONT_WEIGHT_SEMI_BOLD, header_format_.ReleaseAndGetAddressOf())) ||
        !header_format_) {
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
    header_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    header_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    header_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
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
  squircle_.Reset();
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

void Spotlight::DestroyIconCache() {
  for (auto& entry : icon_cache_) {
    if (entry.second != nullptr) {
      DestroyIcon(entry.second);
    }
  }
  icon_cache_.clear();
  icon_pending_.clear();
}

std::wstring Spotlight::PathForMatch(const Match& match) const {
  switch (match.kind) {
    case Kind::App:
      if (match.index >= 0 && match.index < static_cast<int>(apps_.size())) {
        return apps_[static_cast<size_t>(match.index)].path;
      }
      break;
    case Kind::File:
    case Kind::Folder:
      if (match.index >= 0 && match.index < static_cast<int>(files_.size())) {
        return files_[static_cast<size_t>(match.index)].path;
      }
      break;
    case Kind::Header:
    case Kind::Setting:
      break;
  }
  return {};
}

HICON Spotlight::LookupIcon(const Match& match) const {
  switch (match.kind) {
    case Kind::Setting:
      return settings_icon_;
    case Kind::Folder: {
      const std::wstring path = PathForMatch(match);
      if (!path.empty()) {
        const auto it = icon_cache_.find(path);
        if (it != icon_cache_.end() && it->second != nullptr) {
          return it->second;
        }
      }
      return folder_icon_;
    }
    case Kind::App:
    case Kind::File: {
      const std::wstring path = PathForMatch(match);
      if (path.empty()) {
        return nullptr;
      }
      const auto it = icon_cache_.find(path);
      return it != icon_cache_.end() ? it->second : nullptr;
    }
    case Kind::Header:
      break;
  }
  return nullptr;
}

void Spotlight::RequestIcon(const std::wstring& path, bool overlay) {
  if (path.empty() || hwnd_ == nullptr) {
    return;
  }
  if (icon_cache_.find(path) != icon_cache_.end() || icon_pending_.find(path) != icon_pending_.end()) {
    return;
  }
  icon_pending_.insert(path);
  const uint64_t gen = search_gen_.load(std::memory_order_acquire);
  const HWND hwnd = hwnd_;
  icon_inflight_.fetch_add(1, std::memory_order_acq_rel);
  std::thread([this, gen, path, overlay, hwnd]() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool com_ok = SUCCEEDED(com) || com == S_FALSE;
    HICON icon = LooksLikeFilesystemPath(path) ? ExtractShellIcon(path, overlay) : ExtractAppsFolderIcon(path);
    if (icon == nullptr && !LooksLikeFilesystemPath(path)) {
      icon = ExtractShellIcon(path, overlay);
    }
    auto* payload = new IconPayload;
    payload->gen = gen;
    payload->path = path;
    payload->icon = icon;
    if (!PostMessageW(hwnd, kIconReadyMsg, 0, reinterpret_cast<LPARAM>(payload))) {
      if (icon != nullptr) {
        DestroyIcon(icon);
      }
      delete payload;
    }
    if (com_ok) {
      CoUninitialize();
    }
    icon_inflight_.fetch_sub(1, std::memory_order_acq_rel);
  }).detach();
}

void Spotlight::AcceptIcon(void* payload) {
  std::unique_ptr<IconPayload> owned(static_cast<IconPayload*>(payload));
  if (!owned) {
    return;
  }
  icon_pending_.erase(owned->path);
  if (!visible_ || owned->gen != search_gen_.load(std::memory_order_acquire)) {
    if (owned->icon != nullptr) {
      DestroyIcon(owned->icon);
    }
    return;
  }
  if (icon_cache_.find(owned->path) != icon_cache_.end()) {
    if (owned->icon != nullptr) {
      DestroyIcon(owned->icon);
    }
    return;
  }
  icon_cache_.emplace(owned->path, owned->icon);
  const bool ready = owned->icon != nullptr;
  owned->icon = nullptr;
  if (ready) {
    Present();
  }
}

void Spotlight::StartAppsReload() {
  if (hwnd_ == nullptr) {
    return;
  }
  if (apps_inflight_.load(std::memory_order_acquire) != 0) {
    return;
  }
  const uint64_t gen = apps_gen_.load(std::memory_order_acquire);
  const HWND hwnd = hwnd_;
  apps_inflight_.fetch_add(1, std::memory_order_acq_rel);
  std::thread([this, gen, hwnd]() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool com_ok = SUCCEEDED(com) || com == S_FALSE;

    std::unordered_set<std::wstring> seen;
    std::vector<RawApp> raw;
    LARGE_INTEGER freq{};
    LARGE_INTEGER t0{};
    LARGE_INTEGER t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    const bool from_apps = CollectAppsFolder(raw, seen);
    QueryPerformanceCounter(&t1);
    if (from_apps) {
      Log(L"spotlight", L"AppsFolder enumerate %d apps in %.1f ms", static_cast<int>(raw.size()),
          SpotlightElapsedMs(freq, t0, t1));
    } else {
      static bool logged_fallback = false;
      if (!logged_fallback) {
        logged_fallback = true;
        Log(L"spotlight", L"AppsFolder enumerate failed; falling back to Start Menu shortcuts");
      }
      CollectFolder(KnownFolder(FOLDERID_StartMenu), raw, seen);
      CollectFolder(KnownFolder(FOLDERID_CommonStartMenu), raw, seen);
    }
    std::sort(raw.begin(), raw.end(), [](const auto& a, const auto& b) {
      return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE, a.name.c_str(),
                             static_cast<int>(a.name.size()), b.name.c_str(), static_cast<int>(b.name.size()),
                             nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });

    auto* payload = new AppsPayload;
    payload->gen = gen;
    payload->raw = std::move(raw);
    if (!PostMessageW(hwnd, kAppsReadyMsg, 0, reinterpret_cast<LPARAM>(payload))) {
      delete payload;
    }
    if (com_ok) {
      CoUninitialize();
    }
    apps_inflight_.fetch_sub(1, std::memory_order_acq_rel);
  }).detach();
}

void Spotlight::AcceptApps(void* payload) {
  std::unique_ptr<AppsPayload> owned(static_cast<AppsPayload*>(payload));
  if (!owned || owned->gen != apps_gen_.load(std::memory_order_acquire)) {
    return;
  }
  DestroyAppIcons();
  apps_.clear();
  apps_.reserve(owned->raw.size());
  for (auto& item : owned->raw) {
    AppEntry entry;
    entry.name = std::move(item.name);
    entry.path = std::move(item.id);
    entry.target = std::move(item.target);
    entry.filesystem = LooksLikeFilesystemPath(entry.path);
    CollectSearchKeys(entry.path, entry.target, entry.keys);
    apps_.push_back(std::move(entry));
  }
  apps_loaded_at_ = GetTickCount64();
  Log(L"spotlight", L"apps ready %d", static_cast<int>(apps_.size()));
  static bool dumped = false;
  if (!dumped) {
    dumped = true;
    for (const AppEntry& app : apps_) {
      Log(L"spotlight", L"app name=\"%s\" id=\"%s\" target=\"%s\"", app.name.c_str(), app.path.c_str(),
          app.target.c_str());
    }
  }
  if (visible_) {
    RebuildMatches();
    Present();
  }
}

void Spotlight::EnsureApps() {
  if (apps_.empty() || GetTickCount64() - apps_loaded_at_ > kAppReloadMs) {
    StartAppsReload();
  }
}

void Spotlight::QueryFiles(const std::wstring& needle) {
  if (needle.empty() || hwnd_ == nullptr) {
    return;
  }
  const uint64_t gen = search_gen_.load(std::memory_order_acquire);
  const HWND hwnd = hwnd_;
  search_inflight_.fetch_add(1, std::memory_order_acq_rel);
  std::thread([this, gen, needle, hwnd]() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool com_ok = SUCCEEDED(com) || com == S_FALSE;
    auto post = [&](std::vector<FileHit> hits) {
      if (search_gen_.load(std::memory_order_acquire) != gen) {
        DiscardHitIcons(hits);
        return false;
      }
      ExtractHitIcons(hits);
      auto* payload = new FileSearchPayload;
      payload->gen = gen;
      payload->needle = needle;
      payload->hits = std::move(hits);
      if (!PostMessageW(hwnd, kFileSearchDoneMsg, 0, reinterpret_cast<LPARAM>(payload))) {
        DiscardHitIcons(payload->hits);
        delete payload;
        return false;
      }
      return true;
    };
    auto hits = CollectQuickHits(needle);
    if (post(hits)) {
      if (com_ok) {
        post(CollectSlowHits(needle, std::move(hits)));
      }
    }
    if (com_ok) {
      CoUninitialize();
    }
    search_inflight_.fetch_sub(1, std::memory_order_acq_rel);
  }).detach();
}

void Spotlight::AcceptFileHits(void* payload) {
  std::unique_ptr<FileSearchPayload> owned(static_cast<FileSearchPayload*>(payload));
  if (!owned || !visible_ || owned->gen != search_gen_.load(std::memory_order_acquire) ||
      owned->needle != filter_) {
    if (owned) {
      DiscardHitIcons(owned->hits);
    }
    return;
  }
  DestroyFileIcons();
  for (auto& hit : owned->hits) {
    if (hit.path.empty()) {
      if (hit.icon != nullptr) {
        DestroyIcon(hit.icon);
        hit.icon = nullptr;
      }
      continue;
    }
    if (icon_cache_.find(hit.path) == icon_cache_.end()) {
      icon_cache_.emplace(hit.path, hit.icon);
    } else if (hit.icon != nullptr) {
      DestroyIcon(hit.icon);
    }
    hit.icon = nullptr;
  }
  files_ = std::move(owned->hits);
  LayoutWindow();
}

void Spotlight::WaitForFileSearches() {
  search_gen_.fetch_add(1, std::memory_order_acq_rel);
  apps_gen_.fetch_add(1, std::memory_order_acq_rel);
  while (search_inflight_.load(std::memory_order_acquire) != 0) {
    Sleep(10);
  }
  while (icon_inflight_.load(std::memory_order_acquire) != 0) {
    Sleep(10);
  }
  while (apps_inflight_.load(std::memory_order_acquire) != 0) {
    Sleep(10);
  }
  if (hwnd_ != nullptr) {
    MSG msg{};
    while (PeekMessageW(&msg, hwnd_, kFileSearchDoneMsg, kFileSearchDoneMsg, PM_REMOVE) != FALSE) {
      auto* payload = reinterpret_cast<FileSearchPayload*>(msg.lParam);
      if (payload != nullptr) {
        DiscardHitIcons(payload->hits);
        delete payload;
      }
    }
    while (PeekMessageW(&msg, hwnd_, kIconReadyMsg, kIconReadyMsg, PM_REMOVE) != FALSE) {
      auto* payload = reinterpret_cast<IconPayload*>(msg.lParam);
      if (payload != nullptr) {
        if (payload->icon != nullptr) {
          DestroyIcon(payload->icon);
        }
        delete payload;
      }
    }
    while (PeekMessageW(&msg, hwnd_, kAppsReadyMsg, kAppsReadyMsg, PM_REMOVE) != FALSE) {
      auto* payload = reinterpret_cast<AppsPayload*>(msg.lParam);
      delete payload;
    }
  }
  icon_pending_.clear();
}

void Spotlight::RebuildMatches() {
  matches_.clear();
  if (filter_.empty()) {
    return;
  }
  const std::wstring needle = LowerCopy(TrimCopy(filter_));
  if (needle.empty()) {
    return;
  }
  std::vector<Match> apps;
  std::vector<Match> settings;
  std::vector<Match> folders;
  std::vector<Match> documents;

  for (int i = 0; i < static_cast<int>(apps_.size()); ++i) {
    const int score = MatchApp(apps_[static_cast<size_t>(i)], needle);
    if (score >= 0) {
      apps.push_back(Match{Kind::App, i, score});
    }
  }
  for (int i = 0; i < static_cast<int>(sizeof(kSettings) / sizeof(kSettings[0])); ++i) {
    std::wstring hay = kSettings[i].title;
    hay.push_back(L' ');
    hay += kSettings[i].aliases;
    const int score = MatchQuery(std::move(hay), needle);
    if (score >= 0) {
      settings.push_back(Match{Kind::Setting, i, score});
    }
  }
  for (int i = 0; i < static_cast<int>(files_.size()); ++i) {
    const int score = FileHitScore(files_[static_cast<size_t>(i)], needle);
    if (score < 0) {
      continue;
    }
    if (files_[static_cast<size_t>(i)].folder) {
      folders.push_back(Match{Kind::Folder, i, score});
    } else {
      documents.push_back(Match{Kind::File, i, score});
    }
  }

  auto title_of = [&](const Match& match) -> std::wstring {
    switch (match.kind) {
      case Kind::App:
        if (match.index >= 0 && match.index < static_cast<int>(apps_.size())) {
          return apps_[static_cast<size_t>(match.index)].name;
        }
        break;
      case Kind::Setting:
        if (match.index >= 0 &&
            match.index < static_cast<int>(sizeof(kSettings) / sizeof(kSettings[0]))) {
          return kSettings[match.index].title;
        }
        break;
      case Kind::File:
      case Kind::Folder:
        if (match.index >= 0 && match.index < static_cast<int>(files_.size())) {
          return files_[static_cast<size_t>(match.index)].title;
        }
        break;
      default:
        break;
    }
    return {};
  };
  auto path_of = [&](const Match& match) -> std::wstring {
    switch (match.kind) {
      case Kind::App:
        if (match.index >= 0 && match.index < static_cast<int>(apps_.size())) {
          return apps_[static_cast<size_t>(match.index)].path;
        }
        break;
      case Kind::File:
      case Kind::Folder:
        if (match.index >= 0 && match.index < static_cast<int>(files_.size())) {
          return files_[static_cast<size_t>(match.index)].path;
        }
        break;
      default:
        break;
    }
    return {};
  };
  auto by_score = [&](const Match& a, const Match& b) {
    if (a.score != b.score) {
      return a.score < b.score;
    }
    const std::wstring ta = title_of(a);
    const std::wstring tb = title_of(b);
    if (ta.size() != tb.size()) {
      return ta.size() < tb.size();
    }
    return path_of(a) < path_of(b);
  };
  std::sort(apps.begin(), apps.end(), by_score);
  std::sort(settings.begin(), settings.end(), by_score);
  std::sort(folders.begin(), folders.end(), by_score);
  std::sort(documents.begin(), documents.end(), by_score);

  Match top{};
  bool has_top = false;
  auto consider = [&](const Match& match) {
    if (!has_top) {
      top = match;
      has_top = true;
      return;
    }
    if (match.score != top.score) {
      if (match.score < top.score) {
        top = match;
      }
      return;
    }
    if (KindRank(match.kind) != KindRank(top.kind)) {
      if (KindRank(match.kind) < KindRank(top.kind)) {
        top = match;
      }
      return;
    }
    const std::wstring pa = LowerCopy(path_of(match));
    const std::wstring pb = LowerCopy(path_of(top));
    const bool da = pa.find(L"\\desktop\\") != std::wstring::npos;
    const bool db = pb.find(L"\\desktop\\") != std::wstring::npos;
    if (da != db) {
      if (da) {
        top = match;
      }
      return;
    }
    if (title_of(match).size() != title_of(top).size()) {
      if (title_of(match).size() < title_of(top).size()) {
        top = match;
      }
      return;
    }
    if (pa < pb) {
      top = match;
    }
  };
  for (const auto& match : apps) {
    consider(match);
  }
  for (const auto& match : settings) {
    consider(match);
  }
  for (const auto& match : folders) {
    consider(match);
  }
  for (const auto& match : documents) {
    consider(match);
  }

  if (apps.size() > static_cast<size_t>(kMaxApps)) {
    apps.resize(static_cast<size_t>(kMaxApps));
  }
  if (settings.size() > static_cast<size_t>(kMaxSettings)) {
    settings.resize(static_cast<size_t>(kMaxSettings));
  }
  if (folders.size() > static_cast<size_t>(kMaxFolders)) {
    folders.resize(static_cast<size_t>(kMaxFolders));
  }
  if (documents.size() > static_cast<size_t>(kMaxDocuments)) {
    documents.resize(static_cast<size_t>(kMaxDocuments));
  }

  auto append_section = [&](int section, const std::vector<Match>& items) {
    if (items.empty()) {
      return;
    }
    matches_.push_back(Match{Kind::Header, section, 0});
    matches_.insert(matches_.end(), items.begin(), items.end());
  };
  if (has_top) {
    matches_.push_back(Match{Kind::Header, 0, 0});
    matches_.push_back(top);
  }
  append_section(1, apps);
  append_section(2, settings);
  append_section(3, folders);
  append_section(4, documents);
}

int Spotlight::RowHeight(const Match& match) const {
  return Dip(match.kind == Kind::Header ? kHeaderHeightDip : kRowHeightDip);
}

int Spotlight::VisibleCount() const {
  int list_h = 0;
  int count = 0;
  const int budget = Dip(kMaxListDip);
  for (int i = scroll_; i < static_cast<int>(matches_.size()); ++i) {
    const int h = RowHeight(matches_[static_cast<size_t>(i)]);
    if (count > 0 && list_h + h > budget) {
      break;
    }
    list_h += h;
    ++count;
  }
  return count;
}

void Spotlight::LayoutWindow() {
  if (hwnd_ == nullptr) {
    return;
  }
  RebuildMatches();
  if (scroll_ < 0 || scroll_ >= static_cast<int>(matches_.size())) {
    scroll_ = 0;
  }
  const UINT dpi = Dpi();
  const int shadow = DipToPx(kShadowDip, dpi);
  const int width = DipToPx(kWidthDip, dpi) + shadow * 2;
  const int pad = DipToPx(kPadDip, dpi);
  const int search_h = DipToPx(kSearchHeightDip, dpi);

  rows_visible_ = VisibleCount();
  int list_h = 0;
  for (int i = 0; i < rows_visible_ && scroll_ + i < static_cast<int>(matches_.size()); ++i) {
    list_h += RowHeight(matches_[static_cast<size_t>(scroll_ + i)]);
  }
  if (list_h > 0) {
    list_h += pad / 2;
  }
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

  DWORD sel_start = 0;
  DWORD sel_end = 0;
  if (edit_ != nullptr) {
    SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_start), reinterpret_cast<LPARAM>(&sel_end));
  }

  RECT current{};
  GetWindowRect(hwnd_, &current);
  if (current.left != x || current.top != y || current.right != x + width || current.bottom != y + height) {
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
  } else {
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }

  search_rect_ = {shadow + pad, shadow + pad, width - shadow - pad, shadow + pad + search_h};

  if (edit_font_ == nullptr || edit_font_dpi_ != dpi) {
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
    edit_font_dpi_ = dpi;
    if (edit_font_ != nullptr) {
      SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_), FALSE);
    }
  }

  const int glyph = DipToPx(kGlyphDip, dpi);
  const int inset_x = DipToPx(12, dpi) + glyph + DipToPx(10, dpi);
  const int inset_y = DipToPx(4, dpi);
  const int edit_x = search_rect_.left + inset_x;
  const int edit_y = search_rect_.top + inset_y;
  const int edit_w = search_rect_.right - search_rect_.left - inset_x - DipToPx(12, dpi);
  const int edit_h = search_h - inset_y * 2;
  RECT edit_now{};
  if (edit_ != nullptr) {
    GetWindowRect(edit_, &edit_now);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&edit_now), 2);
    if (edit_now.left != edit_x || edit_now.top != edit_y || edit_now.right != edit_x + edit_w ||
        edit_now.bottom != edit_y + edit_h) {
      SetWindowPos(edit_, nullptr, edit_x, edit_y, edit_w, edit_h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SendMessageW(edit_, EM_SETSEL, sel_start, sel_end);
  }

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
  const int icon = Dip(kIconDip);
  const int header_bottom = shadow + pad + search_h + pad / 2;

  if (scroll_ < 0 || scroll_ >= static_cast<int>(matches_.size())) {
    scroll_ = 0;
  }
  rows_visible_ = VisibleCount();
  int y = header_bottom;
  for (int i = 0; i < rows_visible_ && scroll_ + i < static_cast<int>(matches_.size()); ++i) {
    const Match& match = matches_[static_cast<size_t>(scroll_ + i)];
    const int row_h = RowHeight(match);
    Row row{};
    row.rect = {shadow + pad, y, client.right - shadow - pad, y + row_h};
    row.match = match;
    if (match.kind != Kind::Header) {
      const int icon_y = y + (row_h - icon) / 2;
      row.icon_rect = {shadow + pad + Dip(10), icon_y, shadow + pad + Dip(10) + icon, icon_y + icon};
    }
    rows_.push_back(row);
    y += row_h;
  }

  if (hot_ >= static_cast<int>(rows_.size())) {
    hot_ = rows_.empty() ? -1 : static_cast<int>(rows_.size()) - 1;
  }
  if (hot_ >= 0 && hot_ < static_cast<int>(rows_.size()) && !Selectable(rows_[static_cast<size_t>(hot_)])) {
    hot_ = FirstSelectable();
  }
  if (hot_ < 0 && !rows_.empty()) {
    hot_ = FirstSelectable();
  }
}

void Spotlight::Present() {
  WatchdogStage(L"spot.paint");
  if (hwnd_ == nullptr || !EnsureRenderer()) {
    return;
  }
  const ULONGLONG started = GetTickCount64();
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
    squircle_.Reset();
    return;
  }
  rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  rt_->BeginDraw();

  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), brush.GetAddressOf());
  const UINT dpi = Dpi();
  const float shadow = static_cast<float>(Dip(kShadowDip));
  const float radius = corner::ToPx(corner::kHeroDip, dpi);
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
    const D2D1_RECT_F card_rect = D2D1::RectF(card_l, card_t, card_r, card_b);
    const D2D1_ROUNDED_RECT card{card_rect, radius, radius};
    ID2D1PathGeometry* squircle = squircle_.Get(d2d_.Get(), card_rect, radius);
    if (squircle != nullptr) {
      rt_->FillGeometry(squircle, brush.Get());
    } else {
      rt_->FillRoundedRectangle(card, brush.Get());
    }
  }

  const float search_radius =
      corner::ConcentricPx(radius, static_cast<float>(Dip(kPadDip)), corner::ToPx(corner::kOverlayDip, dpi), dpi);
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
      const EditView typed = ReadEditView(edit_, ime_comp_, ime_cursor_);
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
          if (const HIMC himc = ImmGetContext(edit_)) {
            CANDIDATEFORM cand{};
            cand.dwIndex = 0;
            cand.dwStyle = CFS_EXCLUDE;
            cand.ptCurrentPos.x = static_cast<int>(cx + 0.5f);
            cand.ptCurrentPos.y = static_cast<int>(cy + 0.5f);
            cand.rcArea = text_rc;
            MapWindowPoints(hwnd_, edit_, reinterpret_cast<POINT*>(&cand.rcArea), 2);
            ImmSetCandidateWindow(himc, &cand);
            ImmReleaseContext(edit_, himc);
          }
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
    if (row.match.kind == Kind::Header) {
      if (header_format_ && brush) {
        const wchar_t* title = SectionTitle(row.match.index);
        const float text_l = static_cast<float>(row.rect.left + Dip(12));
        const float text_r = static_cast<float>(row.rect.right - Dip(12));
        const float h = static_cast<float>(row.rect.bottom - row.rect.top);
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (title != nullptr && title[0] != L'\0' &&
            SUCCEEDED(dwrite_->CreateTextLayout(title, static_cast<UINT32>(wcslen(title)), header_format_.Get(),
                                                (std::max)(8.0f, text_r - text_l), h, layout.GetAddressOf()))) {
          brush->SetColor(CueColor(dark_));
          rt_->DrawTextLayout(D2D1::Point2F(text_l, static_cast<float>(row.rect.top)), layout.Get(), brush.Get(),
                              D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
        }
      }
      continue;
    }
    if (static_cast<int>(i) == hot_ && Selectable(row) && brush) {
      brush->SetColor(MenuItemHoverFill(dark_, false));
      const float hover_r =
          corner::HoverPx(static_cast<float>(row.rect.bottom - row.rect.top), dpi);
      const D2D1_ROUNDED_RECT hover{
          D2D1::RectF(static_cast<float>(row.rect.left), static_cast<float>(row.rect.top),
                      static_cast<float>(row.rect.right), static_cast<float>(row.rect.bottom)),
          hover_r, hover_r};
      rt_->FillRoundedRectangle(hover, brush.Get());
    }

    std::wstring label;
    std::wstring detail;
    switch (row.match.kind) {
      case Kind::Header:
        break;
      case Kind::App:
        if (row.match.index >= 0 && row.match.index < static_cast<int>(apps_.size())) {
          label = apps_[static_cast<size_t>(row.match.index)].name;
        }
        detail = KindLabel(Kind::App);
        break;
      case Kind::Setting:
        if (row.match.index >= 0 &&
            row.match.index < static_cast<int>(sizeof(kSettings) / sizeof(kSettings[0]))) {
          label = kSettings[row.match.index].title;
        }
        detail = KindLabel(Kind::Setting);
        break;
      case Kind::File:
      case Kind::Folder:
        if (row.match.index >= 0 && row.match.index < static_cast<int>(files_.size())) {
          label = files_[static_cast<size_t>(row.match.index)].title;
          detail = files_[static_cast<size_t>(row.match.index)].detail;
        }
        if (detail.empty()) {
          detail = KindLabel(row.match.kind);
        }
        break;
    }
    if (!label.empty() && format_ && brush) {
      const float meta_w = static_cast<float>(Dip(128));
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
    squircle_.Reset();
    return;
  }

  for (const Row& row : rows_) {
    if (row.match.kind == Kind::Header) {
      continue;
    }
    const std::wstring path = PathForMatch(row.match);
    if (!path.empty() && icon_cache_.find(path) == icon_cache_.end()) {
      RequestIcon(path, row.match.kind != Kind::App);
    }
    if (HICON icon = LookupIcon(row.match)) {
      const int sz = row.icon_rect.bottom - row.icon_rect.top;
      DrawIconEx(layer_dc_, row.icon_rect.left, row.icon_rect.top, icon, sz, sz, 0, nullptr, DI_NORMAL);
    } else {
      const COLORREF fill = dark_ ? RGB(72, 72, 72) : RGB(180, 180, 180);
      const HBRUSH placeholder = CreateSolidBrush(fill);
      FillRect(layer_dc_, &row.icon_rect, placeholder);
      DeleteObject(placeholder);
    }
  }

  POINT src{0, 0};
  SIZE size{width, height};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, layer_dc_, &src, 0, &blend, ULW_ALPHA);
  const unsigned ms = static_cast<unsigned>(GetTickCount64() - started);
  if (ms > 16) {
    Log(L"spotlight", L"paint rows=%zu %ums", rows_.size(), ms);
  }
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
  WatchdogStage(L"spot.filter");
  const ULONGLONG started = GetTickCount64();
  filter_.clear();
  if (edit_ != nullptr) {
    filter_ = ReadEditView(edit_, ime_comp_, ime_cursor_).text;
  }
  search_gen_.fetch_add(1, std::memory_order_acq_rel);
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
  const unsigned ms = static_cast<unsigned>(GetTickCount64() - started);
  if (ms > 50) {
    Log(L"spotlight", L"query len=%zu results=%zu %ums", filter_.size(), matches_.size(), ms);
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
  if (matches_.empty() || delta == 0) {
    return;
  }
  int index = scroll_ + (hot_ < 0 ? 0 : hot_);
  const int n = static_cast<int>(matches_.size());
  for (int step = 0; step < n; ++step) {
    index += delta;
    if (index < 0) {
      index = n - 1;
    } else if (index >= n) {
      index = 0;
    }
    if (matches_[static_cast<size_t>(index)].kind != Kind::Header) {
      break;
    }
  }
  if (matches_[static_cast<size_t>(index)].kind == Kind::Header) {
    return;
  }
  if (index < scroll_) {
    scroll_ = index;
  } else {
    while (scroll_ < index) {
      const int shown = VisibleCount();
      if (index < scroll_ + shown) {
        break;
      }
      ++scroll_;
    }
  }
  RebuildRows();
  hot_ = index - scroll_;
  Present();
}

void Spotlight::ActivateMatch(const Match& match, bool reveal) {
  switch (match.kind) {
    case Kind::Header:
      return;
    case Kind::App:
      if (match.index >= 0 && match.index < static_cast<int>(apps_.size())) {
        const AppEntry& entry = apps_[static_cast<size_t>(match.index)];
        if (reveal) {
          if (entry.filesystem) {
            RevealPath(entry.path);
          }
        } else {
          LaunchApp(entry);
        }
      }
      break;
    case Kind::Setting:
      if (!reveal && match.index >= 0 &&
          match.index < static_cast<int>(sizeof(kSettings) / sizeof(kSettings[0]))) {
        LaunchPath(kSettings[match.index].uri);
      }
      break;
    case Kind::File:
    case Kind::Folder:
      if (match.index >= 0 && match.index < static_cast<int>(files_.size())) {
        if (reveal) {
          RevealPath(files_[static_cast<size_t>(match.index)].path);
        } else {
          LaunchPath(files_[static_cast<size_t>(match.index)].path);
        }
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
  if (index < 0 || index >= static_cast<int>(rows_.size()) || !Selectable(rows_[static_cast<size_t>(index)])) {
    index = FirstSelectable();
  }
  if (index < 0 || index >= static_cast<int>(rows_.size())) {
    return;
  }
  const bool reveal = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  ActivateMatch(rows_[static_cast<size_t>(index)].match, reveal);
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

void Spotlight::LaunchApp(const AppEntry& entry) {
  if (entry.path.empty()) {
    return;
  }
  if (entry.filesystem) {
    LaunchPath(entry.path);
    return;
  }
  const std::wstring target = L"shell:AppsFolder\\" + entry.path;
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"open";
  info.lpFile = target.c_str();
  info.nShow = SW_SHOWNORMAL;
  ShellExecuteExW(&info);
}

void Spotlight::RevealPath(const std::wstring& path) {
  if (path.empty()) {
    return;
  }
  std::wstring param = L"/select,\"";
  param += path;
  param += L'"';
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"open";
  info.lpFile = L"explorer.exe";
  info.lpParameters = param.c_str();
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

bool Spotlight::Selectable(const Row& row) const {
  return row.match.kind != Kind::Header;
}

int Spotlight::FirstSelectable() const {
  for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
    if (Selectable(rows_[static_cast<size_t>(i)])) {
      return i;
    }
  }
  return -1;
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
