#pragma once

#include "popup_surface.hpp"
#include "status_item.hpp"
#include "status_source.hpp"

#include <functional>
#include <string>
#include <vector>

namespace bamti {

struct StatusPanelHost {
  bool dark = false;
  std::function<void(const StatusEvent&)> dispatch;
  std::function<void(std::string id, std::string row_id, uint64_t revision, bool on)> arm_toggle;
  std::function<void(std::string id, std::string row_id, uint64_t revision, float value)> arm_slider;
};

class StatusPanelContent : public PopupContent {
 public:
  void Reset(StatusItem item, StatusPanelHost host);

  int RowCount() const override;
  SIZE Measure(UINT dpi) override;
  void Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) override;
  int HitTest(POINT client, UINT dpi) const override;
  void Invoke(int index) override;
  bool StickyRow(int index) const override;
  void StickyInvoke(int index) override;
  bool DragRow(int index) const override;
  void DragTo(int index, POINT client, UINT dpi) override;
  void DragEnd(int index) override;

 private:
  struct Hit {
    RECT rc{};
    int row = -1;
  };

  int HitRow(int index) const;
  void Activate(int row);

  StatusItem item_{};
  StatusPanelHost host_{};
  std::vector<Hit> hits_;
  int drag_row_ = -1;
  float drag_value_ = 0.0f;
};

struct OverflowHost {
  bool dark = false;
  RECT overflow_rect{};
  std::function<void(const std::string& id)> open_panel;
  std::function<void(const StatusEvent&)> dispatch;
};

class OverflowContent : public PopupContent {
 public:
  void Reset(std::vector<StatusItem> items, OverflowHost host);

  int RowCount() const override;
  SIZE Measure(UINT dpi) override;
  void Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) override;
  int HitTest(POINT client, UINT dpi) const override;
  void Invoke(int index) override;

 private:
  OverflowHost host_{};
  std::vector<StatusItem> items_;
  std::vector<RECT> row_hits_;
};

}  // namespace bamti
