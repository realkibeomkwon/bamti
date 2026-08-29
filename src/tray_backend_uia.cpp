#include "tray_backend.hpp"

#include "log.hpp"
#include "status_item.hpp"

#include <oleauto.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "oleaut32")

namespace bamti {
namespace {

constexpr wchar_t kTrayClass[] = L"Shell_TrayWnd";
constexpr wchar_t kBridgeClass[] = L"Windows.UI.Composition.DesktopWindowContentBridge";

std::wstring BstrTake(BSTR s) {
  if (s == nullptr) {
    return {};
  }
  std::wstring out(s);
  SysFreeString(s);
  return out;
}

uint64_t HashIntArray(const VARIANT& v) {
  if ((v.vt & VT_ARRAY) == 0 || v.parray == nullptr) {
    return 0;
  }
  const VARTYPE elem = static_cast<VARTYPE>(v.vt & VT_TYPEMASK);
  if (elem != VT_I4 && elem != VT_UI4 && elem != VT_INT && elem != VT_UINT) {
    return 0;
  }
  LONG lo = 0;
  LONG hi = -1;
  if (FAILED(SafeArrayGetLBound(v.parray, 1, &lo)) || FAILED(SafeArrayGetUBound(v.parray, 1, &hi)) || hi < lo) {
    return 0;
  }
  uint64_t hash = 14695981039346656037ull;
  for (LONG i = lo; i <= hi; ++i) {
    LONG value = 0;
    if (FAILED(SafeArrayGetElement(v.parray, &i, &value))) {
      return 0;
    }
    hash = Fnv1a64(reinterpret_cast<const uint8_t*>(&value), sizeof(value), hash);
  }
  return hash;
}

uint64_t FallbackKey(const std::wstring& automation_id, const std::wstring& class_name, int order) {
  const std::string id = WideToUtf8Bytes(automation_id);
  const std::string cls = WideToUtf8Bytes(class_name);
  uint64_t hash = Fnv1a64(reinterpret_cast<const uint8_t*>(id.data()), id.size());
  hash = Fnv1a64(reinterpret_cast<const uint8_t*>(cls.data()), cls.size(), hash);
  const int32_t ord = order;
  return Fnv1a64(reinterpret_cast<const uint8_t*>(&ord), sizeof(ord), hash);
}

HWND FindBridge() {
  HWND tray = FindWindowW(kTrayClass, nullptr);
  if (tray == nullptr) {
    return nullptr;
  }
  return FindWindowExW(tray, nullptr, kBridgeClass, nullptr);
}

class TrayBackendUia final : public TrayBackend {
 public:
  const char* Name() const override { return "uia"; }

  bool Probe() override {
    EnsureHwnd();
    return bridge_ != nullptr;
  }

  bool Enumerate(std::vector<TrayIconInfo>* out) override {
    if (out == nullptr) {
      return false;
    }
    out->clear();
    if (!EnsureUia() || !EnsureHwnd()) {
      return false;
    }
    Microsoft::WRL::ComPtr<IUIAutomationElement> root;
    if (FAILED(uia_->ElementFromHandle(bridge_, root.GetAddressOf())) || root == nullptr) {
      ResetHwnd();
      return false;
    }
    Microsoft::WRL::ComPtr<IUIAutomationElementArray> arr;
    if (FAILED(root->FindAllBuildCache(TreeScope_Descendants, cond_.Get(), cache_.Get(), arr.GetAddressOf())) ||
        arr == nullptr) {
      return false;
    }
    int n = 0;
    arr->get_Length(&n);
    out->reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      Microsoft::WRL::ComPtr<IUIAutomationElement> el;
      if (FAILED(arr->GetElement(i, el.GetAddressOf())) || el == nullptr) {
        continue;
      }
      TrayIconInfo info;
      FillCached(el.Get(), &info);
      out->push_back(std::move(info));
    }
    if (out->empty()) {
      arr.Reset();
      if (FAILED(root->FindAll(TreeScope_Descendants, cond_.Get(), arr.GetAddressOf())) || arr == nullptr) {
        Log(L"tray", L"uia FindAll empty cache_n=%d", n);
        return true;
      }
      int cur_n = 0;
      arr->get_Length(&cur_n);
      for (int i = 0; i < cur_n; ++i) {
        Microsoft::WRL::ComPtr<IUIAutomationElement> el;
        if (FAILED(arr->GetElement(i, el.GetAddressOf())) || el == nullptr) {
          continue;
        }
        TrayIconInfo info;
        FillCurrent(el.Get(), &info);
        out->push_back(std::move(info));
      }
      Log(L"tray", L"uia FindAll fallback cache_n=%d current_n=%d kept=%zu", n, cur_n, out->size());
    }
    FinishList(out);
    return true;
  }

  bool Invoke(const TrayIconInfo& icon) override {
    last_hr_ = E_FAIL;
    last_pattern_ = "none";
    if (!EnsureUia() || !EnsureHwnd()) {
      return false;
    }
    Microsoft::WRL::ComPtr<IUIAutomationElement> root;
    if (FAILED(uia_->ElementFromHandle(bridge_, root.GetAddressOf())) || root == nullptr) {
      ResetHwnd();
      return false;
    }
    Microsoft::WRL::ComPtr<IUIAutomationElementArray> arr;
    if (FAILED(root->FindAll(TreeScope_Descendants, cond_.Get(), arr.GetAddressOf())) || arr == nullptr) {
      return false;
    }
    int n = 0;
    arr->get_Length(&n);
    struct Found {
      TrayIconInfo info;
      Microsoft::WRL::ComPtr<IUIAutomationElement> el;
    };
    std::vector<Found> found;
    found.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      Microsoft::WRL::ComPtr<IUIAutomationElement> el;
      if (FAILED(arr->GetElement(i, el.GetAddressOf())) || el == nullptr) {
        continue;
      }
      Found one;
      if (!FillCurrent(el.Get(), &one.info)) {
        continue;
      }
      one.el = el;
      found.push_back(std::move(one));
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
      return a.info.screen.left < b.info.screen.left;
    });
    for (int i = 0; i < static_cast<int>(found.size()); ++i) {
      found[i].info.order = i;
      if (found[i].info.key == 0) {
        found[i].info.key = FallbackKey(found[i].info.automation_id, found[i].info.class_name, i);
      }
    }
    Microsoft::WRL::ComPtr<IUIAutomationElement> match;
    for (const Found& one : found) {
      if (one.info.key == icon.key) {
        match = one.el;
        break;
      }
    }
    if (match == nullptr) {
      last_hr_ = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
      last_pattern_ = "find";
      return false;
    }

    Microsoft::WRL::ComPtr<IUIAutomationInvokePattern> invoke;
    last_hr_ = match->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(invoke.GetAddressOf()));
    last_pattern_ = "Invoke";
    if (SUCCEEDED(last_hr_) && invoke != nullptr) {
      last_hr_ = invoke->Invoke();
      if (SUCCEEDED(last_hr_)) {
        return true;
      }
    }

    Microsoft::WRL::ComPtr<IUIAutomationLegacyIAccessiblePattern> acc;
    last_hr_ = match->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(acc.GetAddressOf()));
    last_pattern_ = "LegacyIAccessible";
    if (SUCCEEDED(last_hr_) && acc != nullptr) {
      last_hr_ = acc->DoDefaultAction();
      return SUCCEEDED(last_hr_);
    }
    return false;
  }

  void Reset() override { ResetHwnd(); }

  void LastInvokeError(HRESULT* hr, const char** pattern) const override {
    if (hr != nullptr) {
      *hr = last_hr_;
    }
    if (pattern != nullptr) {
      *pattern = last_pattern_;
    }
  }

 private:
  bool EnsureHwnd() {
    if (bridge_ != nullptr && IsWindow(bridge_)) {
      return true;
    }
    bridge_ = FindBridge();
    return bridge_ != nullptr;
  }

  void ResetHwnd() { bridge_ = nullptr; }

  bool EnsureUia() {
    if (uia_ != nullptr && cond_ != nullptr && cache_ != nullptr) {
      return true;
    }
    uia_.Reset();
    cond_.Reset();
    cache_.Reset();
    const HRESULT created =
        CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia_.GetAddressOf()));
    if (FAILED(created) || uia_ == nullptr) {
      Log(L"tray", L"CoCreateInstance UIA hr=0x%08X", static_cast<unsigned>(created));
      return false;
    }

    VARIANT vn;
    VariantInit(&vn);
    vn.vt = VT_BSTR;
    vn.bstrVal = SysAllocString(L"NotifyItemIcon");
    Microsoft::WRL::ComPtr<IUIAutomationCondition> id_notify;
    HRESULT hr = uia_->CreatePropertyCondition(UIA_AutomationIdPropertyId, vn, id_notify.GetAddressOf());
    VariantClear(&vn);
    if (FAILED(hr) || id_notify == nullptr) {
      return false;
    }

    VARIANT vs;
    VariantInit(&vs);
    vs.vt = VT_BSTR;
    vs.bstrVal = SysAllocString(L"SystemTrayIcon");
    Microsoft::WRL::ComPtr<IUIAutomationCondition> id_system;
    hr = uia_->CreatePropertyCondition(UIA_AutomationIdPropertyId, vs, id_system.GetAddressOf());
    VariantClear(&vs);
    if (FAILED(hr) || id_system == nullptr) {
      return false;
    }

    VARIANT vt;
    VariantInit(&vt);
    vt.vt = VT_I4;
    vt.lVal = UIA_ButtonControlTypeId;
    Microsoft::WRL::ComPtr<IUIAutomationCondition> type_btn;
    hr = uia_->CreatePropertyCondition(UIA_ControlTypePropertyId, vt, type_btn.GetAddressOf());
    VariantClear(&vt);
    if (FAILED(hr) || type_btn == nullptr) {
      return false;
    }

    Microsoft::WRL::ComPtr<IUIAutomationCondition> id_or;
    if (FAILED(uia_->CreateOrCondition(id_notify.Get(), id_system.Get(), id_or.GetAddressOf())) || id_or == nullptr) {
      return false;
    }
    if (FAILED(uia_->CreateAndCondition(id_or.Get(), type_btn.Get(), cond_.GetAddressOf())) || cond_ == nullptr) {
      return false;
    }

    VARIANT vi;
    VariantInit(&vi);
    vi.vt = VT_I4;
    vi.lVal = UIA_ImageControlTypeId;
    Microsoft::WRL::ComPtr<IUIAutomationCondition> type_img;
    hr = uia_->CreatePropertyCondition(UIA_ControlTypePropertyId, vi, type_img.GetAddressOf());
    VariantClear(&vi);
    if (FAILED(hr) || type_img == nullptr) {
      return false;
    }

    if (FAILED(uia_->CreateCacheRequest(cache_.GetAddressOf())) || cache_ == nullptr) {
      return false;
    }
    cache_->AddProperty(UIA_NamePropertyId);
    cache_->AddProperty(UIA_AutomationIdPropertyId);
    cache_->AddProperty(UIA_ClassNamePropertyId);
    cache_->AddProperty(UIA_BoundingRectanglePropertyId);
    cache_->AddProperty(UIA_IsOffscreenPropertyId);
    cache_->AddProperty(UIA_RuntimeIdPropertyId);
    cache_->AddPattern(UIA_InvokePatternId);
    cache_->put_TreeScope(static_cast<TreeScope>(TreeScope_Element | TreeScope_Children));
    cache_->put_TreeFilter(type_img.Get());
    return true;
  }

  void FillCached(IUIAutomationElement* el, TrayIconInfo* out) {
    BSTR name = nullptr;
    BSTR autoid = nullptr;
    BSTR cls = nullptr;
    el->get_CachedName(&name);
    el->get_CachedAutomationId(&autoid);
    el->get_CachedClassName(&cls);
    out->tip = BstrTake(name);
    out->automation_id = BstrTake(autoid);
    out->class_name = BstrTake(cls);
    el->get_CachedBoundingRectangle(&out->screen);
    el->get_CachedIsOffscreen(&out->offscreen);
    out->system_icon = out->automation_id == L"SystemTrayIcon";
    out->has_image_child = true;
    Microsoft::WRL::ComPtr<IUIAutomationElementArray> kids;
    if (SUCCEEDED(el->GetCachedChildren(kids.GetAddressOf())) && kids != nullptr) {
      int kn = 0;
      kids->get_Length(&kn);
      out->has_image_child = kn > 0;
    }
    VARIANT rid;
    VariantInit(&rid);
    if (SUCCEEDED(el->GetCachedPropertyValue(UIA_RuntimeIdPropertyId, &rid))) {
      out->key = HashIntArray(rid);
    }
    VariantClear(&rid);
  }

  void FinishList(std::vector<TrayIconInfo>* out) {
    std::sort(out->begin(), out->end(), [](const TrayIconInfo& a, const TrayIconInfo& b) {
      return a.screen.left < b.screen.left;
    });
    for (int i = 0; i < static_cast<int>(out->size()); ++i) {
      (*out)[i].order = i;
      if ((*out)[i].key == 0) {
        (*out)[i].key = FallbackKey((*out)[i].automation_id, (*out)[i].class_name, i);
      }
    }
  }

  bool FillCurrent(IUIAutomationElement* el, TrayIconInfo* out) {
    BSTR name = nullptr;
    BSTR autoid = nullptr;
    BSTR cls = nullptr;
    el->get_CurrentName(&name);
    el->get_CurrentAutomationId(&autoid);
    el->get_CurrentClassName(&cls);
    out->tip = BstrTake(name);
    out->automation_id = BstrTake(autoid);
    out->class_name = BstrTake(cls);
    el->get_CurrentBoundingRectangle(&out->screen);
    el->get_CurrentIsOffscreen(&out->offscreen);
    out->system_icon = out->automation_id == L"SystemTrayIcon";
    out->has_image_child = true;
    VARIANT rid;
    VariantInit(&rid);
    if (SUCCEEDED(el->GetCurrentPropertyValue(UIA_RuntimeIdPropertyId, &rid))) {
      out->key = HashIntArray(rid);
    }
    VariantClear(&rid);
    return out->automation_id == L"NotifyItemIcon" || out->system_icon;
  }

  HWND bridge_ = nullptr;
  Microsoft::WRL::ComPtr<IUIAutomation> uia_;
  Microsoft::WRL::ComPtr<IUIAutomationCondition> cond_;
  Microsoft::WRL::ComPtr<IUIAutomationCacheRequest> cache_;
  HRESULT last_hr_ = S_OK;
  const char* last_pattern_ = "";
};

}  // namespace

std::unique_ptr<TrayBackend> MakeUiaTrayBackend() {
  return std::make_unique<TrayBackendUia>();
}

}  // namespace bamti
