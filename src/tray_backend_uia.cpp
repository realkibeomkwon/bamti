#include "tray_backend.hpp"

#include "log.hpp"
#include "status_item.hpp"

#include <oleauto.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <iterator>
#include <new>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "oleaut32")

namespace bamti {
namespace {

constexpr wchar_t kTrayClass[] = L"Shell_TrayWnd";
constexpr wchar_t kBridgeClass[] = L"Windows.UI.Composition.DesktopWindowContentBridge";
constexpr wchar_t kOverflowIslandClass[] = L"TopLevelWindowForOverflowXamlIsland";

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

HWND FindOverflowBridge() {
  HWND island = FindWindowW(kOverflowIslandClass, nullptr);
  if (island == nullptr) {
    return nullptr;
  }
  return FindWindowExW(island, nullptr, kBridgeClass, nullptr);
}

void SignalWake(HANDLE wake, volatile LONG* fired) {
  if (fired != nullptr) {
    InterlockedIncrement(fired);
  }
  if (wake != nullptr) {
    SetEvent(wake);
  }
}

HANDLE g_hook_wake = nullptr;
volatile LONG* g_hook_fired = nullptr;

void CALLBACK TrayWinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG id_object, LONG id_child, DWORD, DWORD) {
  if (id_object != 0 || id_child != 0) {
    return;
  }
  SignalWake(g_hook_wake, g_hook_fired);
}

class StructureHandler final : public IUIAutomationStructureChangedEventHandler {
 public:
  StructureHandler(HANDLE wake, volatile LONG* fired) : refs_(1), wake_(wake), fired_(fired) {}
  StructureHandler(const StructureHandler&) = delete;
  StructureHandler& operator=(const StructureHandler&) = delete;

  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }

  ULONG STDMETHODCALLTYPE Release() override {
    const LONG n = InterlockedDecrement(&refs_);
    if (n == 0) {
      delete this;
    }
    return static_cast<ULONG>(n);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
    if (pp == nullptr) {
      return E_POINTER;
    }
    if (riid == IID_IUnknown || riid == __uuidof(IUIAutomationStructureChangedEventHandler)) {
      *pp = static_cast<IUIAutomationStructureChangedEventHandler*>(this);
      AddRef();
      return S_OK;
    }
    *pp = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE HandleStructureChangedEvent(IUIAutomationElement*, StructureChangeType,
                                                        SAFEARRAY*) override {
    SignalWake(wake_, fired_);
    return S_OK;
  }

 private:
  ~StructureHandler() = default;
  volatile LONG refs_;
  HANDLE wake_;
  volatile LONG* fired_;
};

class PropertyHandler final : public IUIAutomationPropertyChangedEventHandler {
 public:
  PropertyHandler(HANDLE wake, volatile LONG* fired) : refs_(1), wake_(wake), fired_(fired) {}
  PropertyHandler(const PropertyHandler&) = delete;
  PropertyHandler& operator=(const PropertyHandler&) = delete;

  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }

  ULONG STDMETHODCALLTYPE Release() override {
    const LONG n = InterlockedDecrement(&refs_);
    if (n == 0) {
      delete this;
    }
    return static_cast<ULONG>(n);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
    if (pp == nullptr) {
      return E_POINTER;
    }
    if (riid == IID_IUnknown || riid == __uuidof(IUIAutomationPropertyChangedEventHandler)) {
      *pp = static_cast<IUIAutomationPropertyChangedEventHandler*>(this);
      AddRef();
      return S_OK;
    }
    *pp = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE HandlePropertyChangedEvent(IUIAutomationElement*, PROPERTYID, VARIANT) override {
    SignalWake(wake_, fired_);
    return S_OK;
  }

 private:
  ~PropertyHandler() = default;
  volatile LONG refs_;
  HANDLE wake_;
  volatile LONG* fired_;
};

struct FoundEl {
  TrayIconInfo info;
  Microsoft::WRL::ComPtr<IUIAutomationElement> el;
};

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
    std::vector<TrayIconInfo> tray;
    if (!CollectFrom(bridge_, false, &tray)) {
      ResetHwnd();
      return false;
    }
    std::sort(tray.begin(), tray.end(), [](const TrayIconInfo& a, const TrayIconInfo& b) {
      return a.screen.left < b.screen.left;
    });
    std::vector<TrayIconInfo> hidden;
    if (EnsureOverflowHwnd()) {
      if (!CollectFrom(overflow_bridge_, true, &hidden)) {
        overflow_bridge_ = nullptr;
      }
    }
    out->reserve(tray.size() + hidden.size());
    out->insert(out->end(), tray.begin(), tray.end());
    out->insert(out->end(), hidden.begin(), hidden.end());
    AssignOrdersAndKeys(out);
    return true;
  }

  bool Invoke(const TrayIconInfo& icon) override {
    last_hr_ = E_FAIL;
    last_pattern_ = "none";
    if (!EnsureUia() || !EnsureHwnd()) {
      return false;
    }
    std::vector<FoundEl> tray;
    if (!CollectFound(bridge_, false, &tray)) {
      ResetHwnd();
      return false;
    }
    std::sort(tray.begin(), tray.end(), [](const FoundEl& a, const FoundEl& b) {
      return a.info.screen.left < b.info.screen.left;
    });
    std::vector<FoundEl> hidden;
    if (EnsureOverflowHwnd()) {
      CollectFound(overflow_bridge_, true, &hidden);
    }
    std::vector<FoundEl> found;
    found.reserve(tray.size() + hidden.size());
    found.insert(found.end(), std::make_move_iterator(tray.begin()), std::make_move_iterator(tray.end()));
    found.insert(found.end(), std::make_move_iterator(hidden.begin()), std::make_move_iterator(hidden.end()));
    for (int i = 0; i < static_cast<int>(found.size()); ++i) {
      found[i].info.order = i;
      if (found[i].info.key == 0) {
        found[i].info.key = FallbackKey(found[i].info.automation_id, found[i].info.class_name, i);
      }
    }
    Microsoft::WRL::ComPtr<IUIAutomationElement> match;
    bool match_overflow = false;
    for (const FoundEl& one : found) {
      if (one.info.key == icon.key) {
        match = one.el;
        match_overflow = one.info.from_overflow;
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
    HRESULT invoke_hr = last_hr_;
    if (SUCCEEDED(last_hr_) && invoke != nullptr) {
      last_hr_ = invoke->Invoke();
      invoke_hr = last_hr_;
      if (SUCCEEDED(last_hr_)) {
        if (match_overflow) {
          Log(L"tray", L"overflow invoke hr=0x%08X fallback_hr=n/a ok=1", static_cast<unsigned>(invoke_hr));
        }
        return true;
      }
    }

    Microsoft::WRL::ComPtr<IUIAutomationLegacyIAccessiblePattern> acc;
    last_hr_ = match->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(acc.GetAddressOf()));
    last_pattern_ = "LegacyIAccessible";
    HRESULT fallback_hr = last_hr_;
    if (SUCCEEDED(last_hr_) && acc != nullptr) {
      last_hr_ = acc->DoDefaultAction();
      fallback_hr = last_hr_;
      if (SUCCEEDED(last_hr_)) {
        if (match_overflow) {
          Log(L"tray", L"overflow invoke hr=0x%08X fallback_hr=0x%08X ok=1", static_cast<unsigned>(invoke_hr),
              static_cast<unsigned>(fallback_hr));
        }
        return true;
      }
    }
    Log(L"tray", L"invoke hr=0x%08X fallback_hr=0x%08X overflow=%d", static_cast<unsigned>(invoke_hr),
        static_cast<unsigned>(fallback_hr), match_overflow ? 1 : 0);
    return false;
  }

  void Reset() override {
    UnsubscribeStructureChanged();
    ResetHwnd();
  }

  bool SubscribeStructureChanged(HANDLE wake) override {
    if (abandoned_ || wake == nullptr) {
      return false;
    }
    if (subscribed_ && handler_ != nullptr && props_ != nullptr && bridge_ != nullptr && IsWindow(bridge_)) {
      return true;
    }
    UnsubscribeStructureChanged();
    if (!EnsureUia() || !EnsureHwnd()) {
      return false;
    }
    Microsoft::WRL::ComPtr<IUIAutomationElement> root;
    if (FAILED(uia_->ElementFromHandle(bridge_, root.GetAddressOf())) || root == nullptr) {
      ResetHwnd();
      return false;
    }
    auto* raw = new (std::nothrow) StructureHandler(wake, &fired_);
    if (raw == nullptr) {
      return false;
    }
    handler_.Attach(raw);
    auto* praw = new (std::nothrow) PropertyHandler(wake, &fired_);
    if (praw == nullptr) {
      handler_.Reset();
      return false;
    }
    props_.Attach(praw);
    // ChildRemoved는 보내는 요소가 이미 사라져 nullptr 캐시로는 이벤트가 떨어지지 않는다.
    // RuntimeId만 캐시하고, 값은 읽지 않은 채 신호로만 쓴다.
    Microsoft::WRL::ComPtr<IUIAutomationCacheRequest> ev_cache;
    if (FAILED(uia_->CreateCacheRequest(ev_cache.GetAddressOf())) || ev_cache == nullptr) {
      handler_.Reset();
      props_.Reset();
      return false;
    }
    ev_cache->AddProperty(UIA_RuntimeIdPropertyId);
    HRESULT hr =
        uia_->AddStructureChangedEventHandler(root.Get(), TreeScope_Subtree, ev_cache.Get(), handler_.Get());
    if (FAILED(hr)) {
      handler_.Reset();
      props_.Reset();
      Log(L"tray", L"structure_changed subscribe hr=0x%08X", static_cast<unsigned>(hr));
      return false;
    }
    PROPERTYID changed[] = {UIA_IsOffscreenPropertyId};
    hr = uia_->AddPropertyChangedEventHandlerNativeArray(root.Get(), TreeScope_Subtree, ev_cache.Get(), props_.Get(),
                                                         changed, ARRAYSIZE(changed));
    if (FAILED(hr)) {
      Log(L"tray", L"property_changed subscribe hr=0x%08X", static_cast<unsigned>(hr));
      uia_->RemoveAllEventHandlers();
      handler_.Reset();
      props_.Reset();
      return false;
    }
    // HWND 래퍼는 XAML ChildRemoved를 놓친다. 아이콘 패널 부모에도 같은 핸들러를 건다.
    Microsoft::WRL::ComPtr<IUIAutomationElementArray> arr;
    if (SUCCEEDED(root->FindAll(TreeScope_Descendants, cond_.Get(), arr.GetAddressOf())) && arr != nullptr) {
      int n = 0;
      arr->get_Length(&n);
      Microsoft::WRL::ComPtr<IUIAutomationTreeWalker> walker;
      if (n > 0 && SUCCEEDED(uia_->get_ControlViewWalker(walker.GetAddressOf())) && walker != nullptr) {
        Microsoft::WRL::ComPtr<IUIAutomationElement> first;
        if (SUCCEEDED(arr->GetElement(0, first.GetAddressOf())) && first != nullptr) {
          Microsoft::WRL::ComPtr<IUIAutomationElement> parent;
          if (SUCCEEDED(walker->GetParentElement(first.Get(), parent.GetAddressOf())) && parent != nullptr) {
            uia_->AddStructureChangedEventHandler(parent.Get(), TreeScope_Subtree, ev_cache.Get(), handler_.Get());
            uia_->AddPropertyChangedEventHandlerNativeArray(parent.Get(), TreeScope_Subtree, ev_cache.Get(),
                                                            props_.Get(), changed, ARRAYSIZE(changed));
          }
        }
      }
    }
    if (EnsureOverflowHwnd()) {
      Microsoft::WRL::ComPtr<IUIAutomationElement> overflow_root;
      if (SUCCEEDED(uia_->ElementFromHandle(overflow_bridge_, overflow_root.GetAddressOf())) &&
          overflow_root != nullptr) {
        uia_->AddStructureChangedEventHandler(overflow_root.Get(), TreeScope_Subtree, ev_cache.Get(), handler_.Get());
        uia_->AddPropertyChangedEventHandlerNativeArray(overflow_root.Get(), TreeScope_Subtree, ev_cache.Get(),
                                                        props_.Get(), changed, ARRAYSIZE(changed));
      }
    }
    UnhookWinEvent(hook_);
    hook_ = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, nullptr, TrayWinEventProc, 0, 0,
                            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_hook_wake = wake;
    g_hook_fired = &fired_;
    subscribed_ = true;
    return true;
  }

  void UnsubscribeStructureChanged() override {
    if (hook_ != nullptr) {
      UnhookWinEvent(hook_);
      hook_ = nullptr;
    }
    g_hook_wake = nullptr;
    g_hook_fired = nullptr;
    if (uia_ != nullptr) {
      uia_->RemoveAllEventHandlers();
    }
    handler_.Reset();
    props_.Reset();
    subscribed_ = false;
    InterlockedExchange(&fired_, 0);
  }

  bool StructureChangedLive() const override { return subscribed_ && !abandoned_; }

  long TakeStructureChangedCount() override { return InterlockedExchange(&fired_, 0); }

  void AbandonStructureChanged() override {
    UnsubscribeStructureChanged();
    abandoned_ = true;
  }

  ~TrayBackendUia() { UnsubscribeStructureChanged(); }

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

  bool EnsureOverflowHwnd() {
    if (overflow_bridge_ != nullptr && IsWindow(overflow_bridge_)) {
      return true;
    }
    overflow_bridge_ = FindOverflowBridge();
    return overflow_bridge_ != nullptr;
  }

  void ResetHwnd() {
    bridge_ = nullptr;
    overflow_bridge_ = nullptr;
  }

  bool CollectFrom(HWND hwnd, bool from_overflow, std::vector<TrayIconInfo>* out) {
    if (hwnd == nullptr || out == nullptr) {
      return false;
    }
    Microsoft::WRL::ComPtr<IUIAutomationElement> root;
    if (FAILED(uia_->ElementFromHandle(hwnd, root.GetAddressOf())) || root == nullptr) {
      return false;
    }
    Microsoft::WRL::ComPtr<IUIAutomationElementArray> arr;
    if (FAILED(root->FindAllBuildCache(TreeScope_Descendants, cond_.Get(), cache_.Get(), arr.GetAddressOf())) ||
        arr == nullptr) {
      return false;
    }
    int n = 0;
    arr->get_Length(&n);
    const size_t before = out->size();
    out->reserve(before + static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      Microsoft::WRL::ComPtr<IUIAutomationElement> el;
      if (FAILED(arr->GetElement(i, el.GetAddressOf())) || el == nullptr) {
        continue;
      }
      TrayIconInfo info;
      FillCached(el.Get(), &info);
      info.from_overflow = from_overflow;
      out->push_back(std::move(info));
    }
    if (out->size() == before) {
      arr.Reset();
      if (FAILED(root->FindAll(TreeScope_Descendants, cond_.Get(), arr.GetAddressOf())) || arr == nullptr) {
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
        info.from_overflow = from_overflow;
        out->push_back(std::move(info));
      }
    }
    return true;
  }

  bool CollectFound(HWND hwnd, bool from_overflow, std::vector<FoundEl>* out) {
    if (hwnd == nullptr || out == nullptr) {
      return false;
    }
    Microsoft::WRL::ComPtr<IUIAutomationElement> root;
    if (FAILED(uia_->ElementFromHandle(hwnd, root.GetAddressOf())) || root == nullptr) {
      return false;
    }
    Microsoft::WRL::ComPtr<IUIAutomationElementArray> arr;
    if (FAILED(root->FindAll(TreeScope_Descendants, cond_.Get(), arr.GetAddressOf())) || arr == nullptr) {
      return false;
    }
    int n = 0;
    arr->get_Length(&n);
    out->reserve(out->size() + static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      Microsoft::WRL::ComPtr<IUIAutomationElement> el;
      if (FAILED(arr->GetElement(i, el.GetAddressOf())) || el == nullptr) {
        continue;
      }
      FoundEl one;
      if (!FillCurrent(el.Get(), &one.info)) {
        continue;
      }
      one.info.from_overflow = from_overflow;
      one.el = el;
      out->push_back(std::move(one));
    }
    return true;
  }

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

    if (FAILED(uia_->CreateCacheRequest(cache_.GetAddressOf())) || cache_ == nullptr) {
      return false;
    }
    cache_->AddProperty(UIA_NamePropertyId);
    cache_->AddProperty(UIA_AutomationIdPropertyId);
    cache_->AddProperty(UIA_ClassNamePropertyId);
    cache_->AddProperty(UIA_BoundingRectanglePropertyId);
    cache_->AddProperty(UIA_IsOffscreenPropertyId);
    cache_->AddProperty(UIA_RuntimeIdPropertyId);
    cache_->AddProperty(UIA_ControlTypePropertyId);
    cache_->AddPattern(UIA_InvokePatternId);
    // Image-only TreeFilter는 FindAllBuildCache 결과에서 버튼 자체를 빼 캐시가 비었다.
    cache_->put_TreeScope(static_cast<TreeScope>(TreeScope_Element | TreeScope_Children));
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
    const HRESULT kids_hr = el->GetCachedChildren(kids.GetAddressOf());
    if (SUCCEEDED(kids_hr)) {
      out->has_image_child = false;
      int kn = 0;
      if (kids != nullptr) {
        kids->get_Length(&kn);
      }
      for (int i = 0; i < kn; ++i) {
        Microsoft::WRL::ComPtr<IUIAutomationElement> kid;
        if (FAILED(kids->GetElement(i, kid.GetAddressOf())) || kid == nullptr) {
          continue;
        }
        CONTROLTYPEID type = 0;
        kid->get_CachedControlType(&type);
        if (type == UIA_ImageControlTypeId) {
          out->has_image_child = true;
          break;
        }
      }
    }
    VARIANT rid;
    VariantInit(&rid);
    if (SUCCEEDED(el->GetCachedPropertyValue(UIA_RuntimeIdPropertyId, &rid))) {
      out->key = HashIntArray(rid);
    }
    VariantClear(&rid);
  }

  void AssignOrdersAndKeys(std::vector<TrayIconInfo>* out) {
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
  HWND overflow_bridge_ = nullptr;
  Microsoft::WRL::ComPtr<IUIAutomation> uia_;
  Microsoft::WRL::ComPtr<IUIAutomationCondition> cond_;
  Microsoft::WRL::ComPtr<IUIAutomationCacheRequest> cache_;
  Microsoft::WRL::ComPtr<IUIAutomationStructureChangedEventHandler> handler_;
  Microsoft::WRL::ComPtr<IUIAutomationPropertyChangedEventHandler> props_;
  HWINEVENTHOOK hook_ = nullptr;
  volatile LONG fired_ = 0;
  bool subscribed_ = false;
  bool abandoned_ = false;
  HRESULT last_hr_ = S_OK;
  const char* last_pattern_ = "";
};

}  // namespace

std::unique_ptr<TrayBackend> MakeUiaTrayBackend() {
  return std::make_unique<TrayBackendUia>();
}

}  // namespace bamti
