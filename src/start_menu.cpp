#include "start_menu.hpp"

#include "dwm.hpp"
#include "theme.hpp"

#include <commctrl.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <imm.h>
#include <knownfolders.h>
#include <powrprof.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bamti {
namespace {

constexpr int kWidthDip = 280;
constexpr int kPadDip = 8;
constexpr int kSearchHeightDip = 32;
constexpr int kSearchRadiusDip = 8;
constexpr int kRowHeightDip = 32;
constexpr int kSepHeightDip = 8;
constexpr int kMaxHeightDip = 520;
constexpr int kGapBelowStartDip = 4;
constexpr COLORREF kSearchFillRef = RGB(255, 255, 255);
constexpr COLORREF kSearchTextRef = RGB(24, 24, 24);
constexpr UINT kReloadAppsMsg = WM_APP + 40;

int DipToPx(int dip, UINT dpi) {
  return MulDiv(dip, static_cast<int>(dpi), 96);
}

bool EndsWithIgnoreCase(const std::wstring& text, const wchar_t* suffix) {
  const size_t n = wcslen(suffix);
  if (text.size() < n) {
    return false;
  }
  return lstrcmpiW(text.c_str() + (text.size() - n), suffix) == 0;
}

bool SkipShortcut(const std::wstring& name) {
  std::wstring lower = name;
  if (!lower.empty()) {
    CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
  }
  return lower.find(L"uninstall") != std::wstring::npos || lower.find(L"제거") != std::wstring::npos;
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

std::wstring EditVisibleText(HWND edit) {
  std::wstring text;
  if (edit == nullptr) {
    return text;
  }
  const int n = GetWindowTextLengthW(edit);
  if (n > 0) {
    text.resize(static_cast<size_t>(n));
    GetWindowTextW(edit, text.data(), n + 1);
  }
  const HIMC himc = ImmGetContext(edit);
  if (himc == nullptr) {
    return text;
  }
  const LONG bytes = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
  if (bytes > 0) {
    std::wstring comp(static_cast<size_t>(bytes / sizeof(wchar_t)), L'\0');
    ImmGetCompositionStringW(himc, GCS_COMPSTR, comp.data(), bytes);
    DWORD start = 0;
    DWORD end = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    if (start > text.size()) {
      start = static_cast<DWORD>(text.size());
    }
    if (end > text.size()) {
      end = static_cast<DWORD>(text.size());
    }
    if (end < start) {
      end = start;
    }
    text.replace(static_cast<size_t>(start), static_cast<size_t>(end - start), comp);
  }
  ImmReleaseContext(edit, himc);
  return text;
}

UINT32 EditCaretIndex(HWND edit, const std::wstring& visible) {
  DWORD start = 0;
  DWORD end = 0;
  SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
  LONG cursor = 0;
  LONG bytes = 0;
  const HIMC himc = ImmGetContext(edit);
  if (himc != nullptr) {
    bytes = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
    cursor = ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0);
    ImmReleaseContext(edit, himc);
  }
  UINT32 index = end;
  if (bytes > 0 && cursor >= 0) {
    index = start + static_cast<UINT32>(cursor);
  }
  if (index > visible.size()) {
    index = static_cast<UINT32>(visible.size());
  }
  return index;
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
    std::wstring key = name;
    CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
    if (!seen.insert(key).second) {
      continue;
    }
    out.push_back({std::move(name), full});
  } while (FindNextFileW(find, &fd) != FALSE);
  FindClose(find);
}

}  // namespace

StartMenu::~StartMenu() {
  Hide();
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

void StartMenu::Warmup(HWND owner, bool dark) {
  owner_ = owner;
  dark_ = dark;
  if (!EnsureWindow(owner)) {
    return;
  }
  if (apps_.empty()) {
    ReloadApps();
  }
  ApplyChrome();
}

void StartMenu::Toggle(HWND owner, const RECT& start_screen, bool dark, bool from_keyboard) {
  if (visible_) {
    Hide();
    return;
  }
  if (!from_keyboard && closed_at_ != 0 && GetTickCount64() - closed_at_ < 250) {
    return;
  }
  owner_ = owner;
  dark_ = dark;
  anchor_ = start_screen;
  if (!EnsureWindow(owner)) {
    return;
  }
  if (apps_.empty()) {
    ReloadApps();
  }
  filter_.clear();
  scroll_ = 0;
  hot_ = -1;
  if (edit_ != nullptr) {
    SetWindowTextW(edit_, L"");
  }
  ApplyChrome();
  LayoutWindow(anchor_);
  visible_ = true;
  ShowWindow(hwnd_, SW_SHOW);
  SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
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

void StartMenu::Hide() {
  if (!visible_ && !closing_) {
    if (hwnd_ != nullptr) {
      ShowWindow(hwnd_, SW_HIDE);
    }
    return;
  }
  visible_ = false;
  closing_ = false;
  hot_ = -1;
  closed_at_ = GetTickCount64();
  if (hwnd_ != nullptr) {
    ShowWindow(hwnd_, SW_HIDE);
  }
  if (owner_ != nullptr) {
    InvalidateRect(owner_, nullptr, FALSE);
  }
}

LRESULT CALLBACK StartMenu::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  StartMenu* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<StartMenu*>(create->lpCreateParams);
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else {
    self = reinterpret_cast<StartMenu*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (self != nullptr) {
    return self->HandleMessage(msg, wparam, lparam);
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK StartMenu::EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* self = reinterpret_cast<StartMenu*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
    CreateCaret(hwnd, nullptr, (std::max)(1, self->Dip(1)), self->Dip(16));
    HideCaret(hwnd);
    InvalidateRect(self->hwnd_, nullptr, FALSE);
    return result;
  }
  if (msg == WM_KILLFOCUS) {
    DestroyCaret();
    InvalidateRect(self->hwnd_, nullptr, FALSE);
    return CallWindowProcW(self->edit_prev_, hwnd, msg, wparam, lparam);
  }
  if (msg == WM_KEYDOWN) {
    if (wparam == VK_ESCAPE) {
      self->Hide();
      return 0;
    }
    if (wparam == VK_RETURN) {
      const auto apps = self->VisibleApps();
      if (!apps.empty()) {
        Row row{};
        row.action = Action::App;
        row.app_index = apps.front();
        self->ActivateRow(row);
      }
      return 0;
    }
    if (wparam == VK_DOWN && !self->rows_.empty()) {
      self->hot_ = 0;
      InvalidateRect(self->hwnd_, nullptr, FALSE);
      return 0;
    }
  }
  const LRESULT result = CallWindowProcW(self->edit_prev_, hwnd, msg, wparam, lparam);
  if (ShouldRefreshSearchVisual(msg, wparam)) {
    InvalidateRect(self->hwnd_, nullptr, FALSE);
  }
  return result;
}

LRESULT StartMenu::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      Paint();
      return 0;
    case WM_MOUSEMOVE: {
      const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      UpdateHot(pt);
      return 0;
    }
    case WM_MOUSELEAVE:
      if (hot_ >= 0) {
        hot_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
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
        ActivateRow(*row);
      }
      return 0;
    }
    case WM_MOUSEWHEEL:
      if (!VisibleApps().empty()) {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        scroll_ -= (delta / WHEEL_DELTA) * Dip(kRowHeightDip);
        RebuildRows();
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        if (HitTest(pt) != nullptr) {
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
      SetTextColor(hdc, kSearchTextRef);
      SetBkColor(hdc, kSearchFillRef);
      SetBkMode(hdc, OPAQUE);
      if (search_brush_ == nullptr) {
        search_brush_ = CreateSolidBrush(kSearchFillRef);
      }
      return reinterpret_cast<LRESULT>(search_brush_);
    }
    case kReloadAppsMsg:
      ReloadApps();
      if (visible_) {
        LayoutWindow(anchor_);
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;
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
      if (wparam == VK_RETURN && hot_ >= 0 && hot_ < static_cast<int>(rows_.size())) {
        ActivateRow(rows_[static_cast<size_t>(hot_)]);
        return 0;
      }
      break;
    case WM_ACTIVATE:
      if (LOWORD(wparam) == WA_INACTIVE && visible_) {
        closing_ = true;
        Hide();
      }
      return 0;
    case WM_DESTROY:
      hwnd_ = nullptr;
      edit_ = nullptr;
      visible_ = false;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

bool StartMenu::EnsureWindow(HWND owner) {
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
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
  wc.lpszClassName = kStartMenuClass;
  if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kStartMenuClass, L"", WS_POPUP, 0, 0, 0, 0, owner, nullptr,
                          instance, this);
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

bool StartMenu::EnsureRenderer() {
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
  if (!format_ || font_dpi_ != Dpi()) {
    font_dpi_ = Dpi();
    format_.Reset();
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) == 0) {
      wcscpy_s(locale, L"en-US");
    }
    const float size = 13.0f * static_cast<float>(font_dpi_) / 96.0f;
    const wchar_t* families[] = {L"Segoe UI Variable", L"Segoe UI"};
    HRESULT hr = E_FAIL;
    for (const wchar_t* family : families) {
      hr = dwrite_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, size, locale, format_.ReleaseAndGetAddressOf());
      if (SUCCEEDED(hr)) {
        break;
      }
    }
    if (FAILED(hr) || !format_) {
      return false;
    }
    format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  }
  return true;
}

void StartMenu::ReloadApps() {
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
}

void StartMenu::LayoutWindow(const RECT& start_screen) {
  if (hwnd_ == nullptr) {
    return;
  }
  const UINT dpi = Dpi();
  const int width = DipToPx(kWidthDip, dpi);
  const int pad = DipToPx(kPadDip, dpi);
  const int search_h = DipToPx(kSearchHeightDip, dpi);
  const int row_h = DipToPx(kRowHeightDip, dpi);
  const int sep_h = DipToPx(kSepHeightDip, dpi);
  const int header_h = pad + search_h + pad;
  const int footer_h = row_h * 6 + sep_h * 2;
  const int max_h = DipToPx(kMaxHeightDip, dpi);
  const int max_app_h = (std::max)(0, max_h - header_h - footer_h - pad);
  const int max_app_rows = max_app_h / row_h;

  const auto apps = VisibleApps();
  app_rows_visible_ = (std::min)(static_cast<int>(apps.size()), max_app_rows);
  const int app_h = app_rows_visible_ * row_h;
  const int height = header_h + app_h + footer_h + pad;

  HMONITOR monitor = MonitorFromWindow(owner_ != nullptr ? owner_ : hwnd_, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  GetMonitorInfoW(monitor, &info);

  int x = start_screen.left;
  int y = start_screen.bottom + DipToPx(kGapBelowStartDip, dpi);
  if (x + width > info.rcWork.right) {
    x = info.rcWork.right - width;
  }
  if (x < info.rcWork.left) {
    x = info.rcWork.left;
  }
  if (y + height > info.rcWork.bottom) {
    y = start_screen.top - height - DipToPx(kGapBelowStartDip, dpi);
  }
  if (y < info.rcWork.top) {
    y = info.rcWork.top;
  }

  SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);

  search_rect_ = {pad, pad, width - pad, pad + search_h};

  if (edit_font_ != nullptr) {
    DeleteObject(edit_font_);
    edit_font_ = nullptr;
  }
  LOGFONTW lf{};
  lf.lfHeight = -DipToPx(13, dpi);
  lf.lfWeight = FW_NORMAL;
  lf.lfQuality = ANTIALIASED_QUALITY;
  lf.lfCharSet = DEFAULT_CHARSET;
  wcscpy_s(lf.lfFaceName, L"Segoe UI Variable");
  edit_font_ = CreateFontIndirectW(&lf);
  if (edit_font_ != nullptr) {
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_), TRUE);
  }

  const int inset_x = DipToPx(10, dpi);
  const int inset_y = DipToPx(1, dpi);
  SetWindowPos(edit_, nullptr, search_rect_.left + inset_x, search_rect_.top + inset_y,
               search_rect_.right - search_rect_.left - inset_x * 2, search_h - inset_y * 2,
               SWP_NOZORDER | SWP_NOACTIVATE);

  RebuildRows();
}

void StartMenu::RebuildRows() {
  rows_.clear();
  if (hwnd_ == nullptr) {
    return;
  }
  RECT client{};
  GetClientRect(hwnd_, &client);
  const int pad = Dip(kPadDip);
  const int search_h = Dip(kSearchHeightDip);
  const int row_h = Dip(kRowHeightDip);
  const int sep_h = Dip(kSepHeightDip);
  const int header_bottom = pad + search_h + pad;

  const auto apps = VisibleApps();
  const int max_rows = (std::max)(0, app_rows_visible_);
  const int max_scroll = (std::max)(0, static_cast<int>(apps.size()) - max_rows) * row_h;
  if (scroll_ > max_scroll) {
    scroll_ = max_scroll;
  }
  if (scroll_ < 0) {
    scroll_ = 0;
  }
  scroll_ = row_h > 0 ? (scroll_ / row_h) * row_h : 0;

  const int first = row_h > 0 ? scroll_ / row_h : 0;
  int y = header_bottom;
  for (int i = 0; i < max_rows && first + i < static_cast<int>(apps.size()); ++i) {
    Row row{};
    row.rect = {pad, y, client.right - pad, y + row_h};
    row.action = Action::App;
    row.app_index = apps[static_cast<size_t>(first + i)];
    rows_.push_back(row);
    y += row_h;
  }

  y += sep_h;
  auto add_footer = [&](Action action) {
    Row row{};
    row.rect = {pad, y, client.right - pad, y + row_h};
    row.action = action;
    rows_.push_back(row);
    y += row_h;
  };
  add_footer(Action::Explorer);
  add_footer(Action::Settings);
  add_footer(Action::Run);
  y += sep_h;
  add_footer(Action::Sleep);
  add_footer(Action::Restart);
  add_footer(Action::Shutdown);

  if (hot_ >= static_cast<int>(rows_.size())) {
    hot_ = rows_.empty() ? -1 : static_cast<int>(rows_.size()) - 1;
  }
}

void StartMenu::Paint() {
  PAINTSTRUCT ps{};
  const HDC hdc = BeginPaint(hwnd_, &ps);
  RECT client{};
  GetClientRect(hwnd_, &client);

  BP_PAINTPARAMS params{};
  params.cbSize = sizeof(params);
  params.dwFlags = BPPF_ERASE;
  HDC buffer_dc = nullptr;
  const HPAINTBUFFER buffer = BeginBufferedPaint(hdc, &client, BPBF_TOPDOWNDIB, &params, &buffer_dc);
  if (buffer != nullptr && buffer_dc != nullptr && EnsureRenderer()) {
    BufferedPaintClear(buffer, &client);
    if (!rt_) {
      const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
          D2D1_RENDER_TARGET_TYPE_DEFAULT,
          D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
      d2d_->CreateDCRenderTarget(&props, rt_.ReleaseAndGetAddressOf());
    }
    if (rt_ && SUCCEEDED(rt_->BindDC(buffer_dc, &client))) {
      rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
      rt_->BeginDraw();
      Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
      rt_->CreateSolidColorBrush(D2D1::ColorF(0.94f, 0.94f, 0.94f, 1.0f), brush.GetAddressOf());
      if (brush) {
        rt_->FillRectangle(
            D2D1::RectF(0.0f, 0.0f, static_cast<float>(client.right), static_cast<float>(client.bottom)), brush.Get());
      }

      const float radius = static_cast<float>(Dip(kSearchRadiusDip));
      if (brush) {
        brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        const D2D1_ROUNDED_RECT search{
            D2D1::RectF(static_cast<float>(search_rect_.left), static_cast<float>(search_rect_.top),
                        static_cast<float>(search_rect_.right), static_cast<float>(search_rect_.bottom)),
            radius, radius};
        rt_->FillRoundedRectangle(search, brush.Get());
      }

      if (brush && format_ && dwrite_ && edit_ != nullptr) {
        RECT text_rc{};
        GetClientRect(edit_, &text_rc);
        MapWindowPoints(edit_, hwnd_, reinterpret_cast<POINT*>(&text_rc), 2);
        const float tw = static_cast<float>(text_rc.right - text_rc.left);
        const float th = static_cast<float>(text_rc.bottom - text_rc.top);
        if (tw > 1.0f && th > 1.0f) {
          const std::wstring typed = EditVisibleText(edit_);
          const bool cue = typed.empty();
          const wchar_t* label = cue ? L"검색" : typed.c_str();
          const UINT32 label_len = static_cast<UINT32>(cue ? wcslen(label) : typed.size());
          Microsoft::WRL::ComPtr<IDWriteTextLayout> search_layout;
          if (SUCCEEDED(dwrite_->CreateTextLayout(label, label_len, format_.Get(), tw, th,
                                                  search_layout.GetAddressOf()))) {
            brush->SetColor(cue ? D2D1::ColorF(0.55f, 0.55f, 0.55f, 1.0f)
                                : D2D1::ColorF(0.09f, 0.09f, 0.09f, 1.0f));
            rt_->DrawTextLayout(D2D1::Point2F(static_cast<float>(text_rc.left), static_cast<float>(text_rc.top)),
                                search_layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            if (GetFocus() == edit_) {
              const UINT32 caret_index = cue ? 0 : EditCaretIndex(edit_, typed);
              FLOAT cx = 0.0f;
              FLOAT cy = 0.0f;
              DWRITE_HIT_TEST_METRICS hit{};
              search_layout->HitTestTextPosition(caret_index, FALSE, &cx, &cy, &hit);
              const float caret_h = hit.height > 1.0f ? hit.height : format_->GetFontSize();
              const float x0 = static_cast<float>(text_rc.left) + cx;
              const float y0 = static_cast<float>(text_rc.top) + cy;
              brush->SetColor(D2D1::ColorF(0.09f, 0.09f, 0.09f, 1.0f));
              rt_->FillRectangle(D2D1::RectF(x0, y0, x0 + 1.5f, y0 + caret_h), brush.Get());
              SetCaretPos(static_cast<int>(cx + 0.5f), static_cast<int>(cy + 0.5f));
            }
          }
        }
      }

      const int pad = Dip(kPadDip);
      const int row_h = Dip(kRowHeightDip);
      const int sep_h = Dip(kSepHeightDip);
      if (brush) {
        brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f));
        const int header_bottom = pad + Dip(kSearchHeightDip) + pad;
        const int app_bottom = header_bottom + app_rows_visible_ * row_h;
        const float x1 = static_cast<float>(pad);
        const float x2 = static_cast<float>(client.right - pad);
        const float y1 = static_cast<float>(app_bottom + sep_h / 2);
        const float y2 = static_cast<float>(app_bottom + sep_h + row_h * 3 + sep_h / 2);
        rt_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y1), brush.Get(), 1.0f);
        rt_->DrawLine(D2D1::Point2F(x1, y2), D2D1::Point2F(x2, y2), brush.Get(), 1.0f);
      }

      for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        if (static_cast<int>(i) == hot_ && brush) {
          brush->SetColor(MenuItemHoverFill(false, false));
          const D2D1_ROUNDED_RECT hover{
              D2D1::RectF(static_cast<float>(row.rect.left), static_cast<float>(row.rect.top),
                          static_cast<float>(row.rect.right), static_cast<float>(row.rect.bottom)),
              static_cast<float>(Dip(6)), static_cast<float>(Dip(6))};
          rt_->FillRoundedRectangle(hover, brush.Get());
        }

        std::wstring label;
        switch (row.action) {
          case Action::App:
            if (row.app_index >= 0 && row.app_index < static_cast<int>(apps_.size())) {
              label = apps_[static_cast<size_t>(row.app_index)].name;
            }
            break;
          case Action::Explorer:
            label = L"파일 탐색기";
            break;
          case Action::Settings:
            label = L"설정";
            break;
          case Action::Run:
            label = L"실행";
            break;
          case Action::Sleep:
            label = L"절전";
            break;
          case Action::Restart:
            label = L"다시 시작";
            break;
          case Action::Shutdown:
            label = L"시스템 종료";
            break;
        }
        if (!label.empty() && format_ && brush) {
          Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
          const float w = static_cast<float>(row.rect.right - row.rect.left - Dip(16));
          const float h = static_cast<float>(row.rect.bottom - row.rect.top);
          if (SUCCEEDED(dwrite_->CreateTextLayout(label.c_str(), static_cast<UINT32>(label.size()), format_.Get(), w, h,
                                                  layout.GetAddressOf()))) {
            brush->SetColor(D2D1::ColorF(0.09f, 0.09f, 0.09f, 1.0f));
            rt_->DrawTextLayout(D2D1::Point2F(static_cast<float>(row.rect.left + Dip(8)), static_cast<float>(row.rect.top)),
                                layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
          }
        }
      }

      const HRESULT hr = rt_->EndDraw();
      if (hr == D2DERR_RECREATE_TARGET) {
        rt_.Reset();
      }
    }
    BufferedPaintSetAlpha(buffer, &client, 255);
    EndBufferedPaint(buffer, TRUE);
  }
  EndPaint(hwnd_, &ps);
}

void StartMenu::PaintEdit(HWND edit) {
  PAINTSTRUCT ps{};
  BeginPaint(edit, &ps);
  EndPaint(edit, &ps);
}

void StartMenu::ApplyChrome() {
  if (hwnd_ == nullptr) {
    return;
  }
  const BOOL dark = FALSE;
  DwmSetWindowAttribute(hwnd_, dwm::kUseImmersiveDarkMode, &dark, sizeof(dark));
  const int backdrop = dwm::kBackdropNone;
  DwmSetWindowAttribute(hwnd_, dwm::kSystemBackdropType, &backdrop, sizeof(backdrop));
  const int corner = dwm::kCornerRound;
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
  if (search_brush_ == nullptr) {
    search_brush_ = CreateSolidBrush(kSearchFillRef);
  }
}

void StartMenu::ApplyFilter() {
  filter_.clear();
  if (edit_ != nullptr) {
    const int n = GetWindowTextLengthW(edit_);
    if (n > 0) {
      filter_.assign(static_cast<size_t>(n) + 1, L'\0');
      GetWindowTextW(edit_, filter_.data(), n + 1);
      filter_.resize(static_cast<size_t>(n));
    }
  }
  scroll_ = 0;
  hot_ = -1;
  LayoutWindow(anchor_);
  InvalidateRect(hwnd_, nullptr, FALSE);
  if (edit_ != nullptr) {
    InvalidateRect(edit_, nullptr, FALSE);
  }
}

void StartMenu::UpdateHot(POINT client) {
  TRACKMOUSEEVENT track{};
  track.cbSize = sizeof(track);
  track.dwFlags = TME_LEAVE;
  track.hwndTrack = hwnd_;
  TrackMouseEvent(&track);

  int next = -1;
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (PtInRect(&rows_[i].rect, client)) {
      next = static_cast<int>(i);
      break;
    }
  }
  if (next == hot_) {
    return;
  }
  hot_ = next;
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void StartMenu::ActivateRow(const Row& row) {
  switch (row.action) {
    case Action::App:
      if (row.app_index >= 0 && row.app_index < static_cast<int>(apps_.size())) {
        LaunchPath(apps_[static_cast<size_t>(row.app_index)].path);
      }
      break;
    case Action::Explorer:
      LaunchPath(L"explorer.exe");
      break;
    case Action::Settings:
      LaunchPath(L"ms-settings:");
      break;
    case Action::Run:
      Hide();
      SendWinChord(L'R');
      return;
    case Action::Sleep:
      Hide();
      SetSuspendState(FALSE, TRUE, FALSE);
      return;
    case Action::Restart:
      Hide();
      if (EnableShutdownPrivilege()) {
        ExitWindowsEx(EWX_REBOOT, 0);
      }
      return;
    case Action::Shutdown:
      Hide();
      if (EnableShutdownPrivilege()) {
        ExitWindowsEx(EWX_SHUTDOWN, 0);
      }
      return;
  }
  Hide();
}

void StartMenu::LaunchPath(const std::wstring& path) {
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

void StartMenu::SendWinChord(WORD vk) {
  INPUT keys[4]{};
  keys[0].type = INPUT_KEYBOARD;
  keys[0].ki.wVk = VK_LWIN;
  keys[1].type = INPUT_KEYBOARD;
  keys[1].ki.wVk = vk;
  keys[2].type = INPUT_KEYBOARD;
  keys[2].ki.wVk = vk;
  keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
  keys[3].type = INPUT_KEYBOARD;
  keys[3].ki.wVk = VK_LWIN;
  keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(4, keys, sizeof(INPUT));
}

bool StartMenu::EnableShutdownPrivilege() {
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) == FALSE) {
    return false;
  }
  TOKEN_PRIVILEGES tp{};
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid) == FALSE) {
    CloseHandle(token);
    return false;
  }
  AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
  const bool ok = GetLastError() == ERROR_SUCCESS;
  CloseHandle(token);
  return ok;
}

const StartMenu::Row* StartMenu::HitTest(POINT client) const {
  for (const auto& row : rows_) {
    if (PtInRect(&row.rect, client)) {
      return &row;
    }
  }
  return nullptr;
}

std::vector<int> StartMenu::VisibleApps() const {
  std::vector<int> out;
  out.reserve(apps_.size());
  for (int i = 0; i < static_cast<int>(apps_.size()); ++i) {
    if (!filter_.empty()) {
      std::wstring hay = apps_[static_cast<size_t>(i)].name;
      std::wstring needle = filter_;
      CharLowerBuffW(hay.data(), static_cast<DWORD>(hay.size()));
      CharLowerBuffW(needle.data(), static_cast<DWORD>(needle.size()));
      if (hay.find(needle) == std::wstring::npos) {
        continue;
      }
    }
    out.push_back(i);
  }
  return out;
}

UINT StartMenu::Dpi() const {
  HWND source = hwnd_ != nullptr ? hwnd_ : owner_;
  if (source == nullptr) {
    return 96;
  }
  const UINT dpi = GetDpiForWindow(source);
  return dpi == 0 ? 96 : dpi;
}

int StartMenu::Dip(int value) const {
  return DipToPx(value, Dpi());
}

}  // namespace bamti
