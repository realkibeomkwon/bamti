#pragma once

#include "corner.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace bamti {

inline constexpr wchar_t kSpotlightClass[] = L"bamti.Spotlight";

class Spotlight {
 public:
  Spotlight() = default;
  Spotlight(const Spotlight&) = delete;
  Spotlight& operator=(const Spotlight&) = delete;
  ~Spotlight();

  void Warmup(HWND owner, bool dark);
  void Toggle(HWND owner, bool dark);
  void Hide();
  bool visible() const { return visible_; }

  enum class Kind {
    Header,
    App,
    Setting,
    File,
    Folder,
  };

  struct FileHit {
    std::wstring title;
    std::wstring detail;
    std::wstring path;
    bool folder = false;
    HICON icon = nullptr;
  };

 private:
  struct AppEntry {
    std::wstring name;
    std::wstring path;
    HICON icon = nullptr;
    bool filesystem = false;
  };

  struct Match {
    Kind kind = Kind::App;
    int index = -1;
    int score = 0;
  };

  struct Row {
    RECT rect{};
    RECT icon_rect{};
    Match match{};
  };

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
  static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  LRESULT HandleImeMessage(HWND edit, UINT msg, WPARAM wparam, LPARAM lparam);
  void HandleImeComposition(LPARAM lparam);
  void ClearIme();
  bool EnsureWindow(HWND owner);
  bool EnsureRenderer();
  bool EnsureLayer(int width, int height);
  void ReleaseLayer();
  void StartAppsReload();
  void AcceptApps(void* payload);
  void EnsureApps();
  void DestroyAppIcons();
  void DestroyFileIcons();
  void DestroyIconCache();
  std::wstring PathForMatch(const Match& match) const;
  HICON LookupIcon(const Match& match) const;
  void RequestIcon(const std::wstring& path, bool overlay);
  void AcceptIcon(void* payload);
  void QueryFiles(const std::wstring& needle);
  void AcceptFileHits(void* payload);
  void WaitForFileSearches();
  void RebuildMatches();
  void LayoutWindow();
  void RebuildRows();
  void Present();
  void PaintEdit(HWND edit);
  void ApplyChrome();
  void ApplyFilter();
  void UpdateHot(POINT client);
  void MoveHot(int delta);
  void ActivateMatch(const Match& match, bool reveal = false);
  void ActivateHot();
  void LaunchPath(const std::wstring& path);
  void LaunchApp(const AppEntry& entry);
  void RevealPath(const std::wstring& path);
  int RowHeight(const Match& match) const;
  int VisibleCount() const;
  const Row* HitTest(POINT client) const;
  bool Selectable(const Row& row) const;
  int FirstSelectable() const;
  UINT Dpi() const;
  int Dip(int value) const;

  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;
  HWND edit_ = nullptr;
  WNDPROC edit_prev_ = nullptr;
  HFONT edit_font_ = nullptr;
  HBRUSH search_brush_ = nullptr;
  HDC layer_dc_ = nullptr;
  HBITMAP layer_bmp_ = nullptr;
  HGDIOBJ layer_old_ = nullptr;
  void* layer_bits_ = nullptr;
  int layer_w_ = 0;
  int layer_h_ = 0;
  RECT search_rect_{};
  ULONGLONG apps_loaded_at_ = 0;
  std::atomic<uint64_t> apps_gen_{0};
  std::atomic<int> apps_inflight_{0};
  UINT font_dpi_ = 0;
  UINT edit_font_dpi_ = 0;
  bool visible_ = false;
  bool dark_ = true;
  bool closing_ = false;
  int scroll_ = 0;
  int hot_ = -1;
  int rows_visible_ = 0;
  std::wstring filter_;
  std::wstring ime_comp_;
  LONG ime_cursor_ = 0;
  bool swallow_ime_commit_ = false;
  std::atomic<uint64_t> search_gen_{0};
  std::atomic<uint32_t> search_inflight_{0};
  std::atomic<uint32_t> icon_inflight_{0};
  std::map<std::wstring, HICON> icon_cache_;
  std::unordered_set<std::wstring> icon_pending_;
  std::vector<AppEntry> apps_;
  std::vector<FileHit> files_;
  std::vector<Match> matches_;
  std::vector<Row> rows_;
  HICON settings_icon_ = nullptr;
  HICON folder_icon_ = nullptr;
  Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> format_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> search_format_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> meta_format_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> header_format_;
  Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> rt_;
  corner::SquircleCache squircle_;
};

}  // namespace bamti
