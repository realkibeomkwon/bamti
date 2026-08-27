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
    Explorer,
    Settings,
    Run,
    Sleep,
    Restart,
    Shutdown,
  };

  struct Row {
    RECT rect{};
    Action action = Action::Explorer;
  };

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  bool EnsureWindow(HWND owner);
  bool EnsureRenderer();
  void LayoutWindow(const RECT& start_screen);
  void RebuildRows();
  void Paint();
  void ApplyChrome();
  void UpdateHot(POINT client);
  void MoveHot(int delta);
  void ActivateRow(const Row& row);
  void LaunchPath(const std::wstring& path);
  void SendWinChord(WORD vk);
  bool EnableShutdownPrivilege();
  const Row* HitTest(POINT client) const;
  UINT Dpi() const;
  int Dip(int value) const;

  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;
  RECT anchor_{};
  ULONGLONG closed_at_ = 0;
  UINT font_dpi_ = 0;
  bool visible_ = false;
  bool dark_ = true;
  bool closing_ = false;
  int hot_ = -1;
  std::vector<Row> rows_;
  Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> format_;
  Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> rt_;
};

}  // namespace bamti
