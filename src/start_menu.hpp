#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace bamti {

inline constexpr wchar_t kStartMenuClass[] = L"bamti.StartMenu";

class StartMenu {
 public:
  StartMenu() = default;
  StartMenu(const StartMenu&) = delete;
  StartMenu& operator=(const StartMenu&) = delete;
  ~StartMenu();

  void Warmup(HWND owner, bool dark);
  void Toggle(HWND owner, const RECT& start_screen, bool dark, bool from_keyboard = false);
  void Hide();
  bool visible() const { return visible_; }

 private:
  enum class Action {
    App,
    Explorer,
    Settings,
    Run,
    Sleep,
    Restart,
    Shutdown,
  };

  struct AppEntry {
    std::wstring name;
    std::wstring path;
  };

  struct Row {
    RECT rect{};
    Action action = Action::App;
    int app_index = -1;
  };

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
  static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  bool EnsureWindow(HWND owner);
  bool EnsureRenderer();
  void ReloadApps();
  void LayoutWindow(const RECT& start_screen);
  void RebuildRows();
  void Paint();
  void PaintEdit(HWND edit);
  void ApplyChrome();
  void ApplyFilter();
  void UpdateHot(POINT client);
  void ActivateRow(const Row& row);
  void LaunchPath(const std::wstring& path);
  void SendWinChord(WORD vk);
  bool EnableShutdownPrivilege();
  const Row* HitTest(POINT client) const;
  std::vector<int> VisibleApps() const;
  UINT Dpi() const;
  int Dip(int value) const;

  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;
  HWND edit_ = nullptr;
  WNDPROC edit_prev_ = nullptr;
  HFONT edit_font_ = nullptr;
  HBRUSH search_brush_ = nullptr;
  RECT anchor_{};
  RECT search_rect_{};
  ULONGLONG closed_at_ = 0;
  UINT font_dpi_ = 0;
  bool visible_ = false;
  bool dark_ = true;
  bool closing_ = false;
  int scroll_ = 0;
  int hot_ = -1;
  int app_rows_visible_ = 0;
  std::wstring filter_;
  std::vector<AppEntry> apps_;
  std::vector<Row> rows_;
  Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> format_;
  Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> rt_;
};

}  // namespace bamti
