#include "autostart.hpp"

#include "log.hpp"

#define SECURITY_WIN32
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <security.h>
#include <taskschd.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace bamti {
namespace {

constexpr wchar_t kRunSubkey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedSubkey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kValueName[] = L"bamti";
constexpr wchar_t kTaskName[] = L"bamti";

class Bstr {
 public:
  explicit Bstr(const wchar_t* text) : p_(SysAllocString(text)) {}
  ~Bstr() { SysFreeString(p_); }
  Bstr(const Bstr&) = delete;
  Bstr& operator=(const Bstr&) = delete;
  operator BSTR() const { return p_; }

 private:
  BSTR p_ = nullptr;
};

struct ComScope {
  bool ok = false;
  bool uninit = false;

  ComScope() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (hr == S_OK) {
      ok = true;
      uninit = true;
    } else if (hr == S_FALSE || hr == RPC_E_CHANGED_MODE) {
      ok = true;
    }
  }

  ~ComScope() {
    if (uninit) {
      CoUninitialize();
    }
  }

  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;
};

std::wstring ModulePath() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (n == 0) {
      return {};
    }
    if (n < path.size()) {
      path.resize(n);
      break;
    }
    path.assign(path.size() * 2, L'\0');
  }
  return path;
}

std::wstring ModuleCommand() {
  const std::wstring path = ModulePath();
  if (path.empty()) {
    return {};
  }
  return L"\"" + path + L"\"";
}

std::wstring CurrentUserId() {
  DWORD n = 256;
  std::wstring s(n, L'\0');
  if (GetUserNameExW(NameSamCompatible, s.data(), &n) != FALSE) {
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
  }
  if (GetLastError() != ERROR_MORE_DATA || n == 0) {
    return {};
  }
  s.assign(n, L'\0');
  if (GetUserNameExW(NameSamCompatible, s.data(), &n) == FALSE) {
    return {};
  }
  s.resize(wcsnlen(s.c_str(), s.size()));
  return s;
}

bool RunValueExists() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunSubkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  DWORD type = 0;
  DWORD size = 0;
  const LONG st = RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &size);
  RegCloseKey(key);
  return st == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
}

bool StartupApprovedAllows() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedSubkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return true;
  }
  DWORD type = 0;
  DWORD size = 0;
  LONG st = RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &size);
  if (st != ERROR_SUCCESS || type != REG_BINARY || size == 0) {
    RegCloseKey(key);
    return true;
  }
  std::vector<BYTE> data(size);
  st = RegQueryValueExW(key, kValueName, nullptr, &type, data.data(), &size);
  RegCloseKey(key);
  if (st != ERROR_SUCCESS || size == 0) {
    return true;
  }
  return (data[0] & 1u) == 0;
}

bool IsBamtiRunName(const wchar_t* name) {
  return name != nullptr && wcscmp(name, kValueName) == 0;
}

void DeleteApprovedValue(const wchar_t* name) {
  if (!IsBamtiRunName(name)) {
    return;
  }
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedSubkey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
    return;
  }
  const LONG st = RegDeleteValueW(key, name);
  RegCloseKey(key);
  if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
    Log(L"host", L"autostart approved delete failed err=%lu", static_cast<unsigned long>(st));
  }
}

bool DeleteRunValue() {
  HKEY key = nullptr;
  LONG st = RegOpenKeyExW(HKEY_CURRENT_USER, kRunSubkey, 0, KEY_SET_VALUE, &key);
  if (st == ERROR_FILE_NOT_FOUND) {
    return true;
  }
  if (st != ERROR_SUCCESS) {
    Log(L"host", L"autostart registry open failed err=%lu", static_cast<unsigned long>(st));
    return false;
  }
  st = RegDeleteValueW(key, kValueName);
  RegCloseKey(key);
  if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
    Log(L"host", L"autostart delete failed err=%lu", static_cast<unsigned long>(st));
    return false;
  }
  return true;
}

bool SetRunAutostart(bool on) {
  HKEY key = nullptr;
  LONG st = RegCreateKeyExW(HKEY_CURRENT_USER, kRunSubkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
  if (st != ERROR_SUCCESS) {
    Log(L"host", L"autostart registry open failed err=%lu", static_cast<unsigned long>(st));
    return false;
  }
  if (on) {
    const std::wstring command = ModuleCommand();
    if (command.empty()) {
      RegCloseKey(key);
      Log(L"host", L"autostart module path failed err=%lu", GetLastError());
      return false;
    }
    const DWORD bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    st = RegSetValueExW(key, kValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), bytes);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS) {
      Log(L"host", L"autostart set failed err=%lu", static_cast<unsigned long>(st));
      return false;
    }
    Log(L"host", L"autostart added cmd=%s", command.c_str());
    DeleteApprovedValue(kValueName);
    return true;
  }
  st = RegDeleteValueW(key, kValueName);
  RegCloseKey(key);
  if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
    Log(L"host", L"autostart delete failed err=%lu", static_cast<unsigned long>(st));
    return false;
  }
  Log(L"host", L"autostart removed");
  return true;
}

bool ConnectService(Microsoft::WRL::ComPtr<ITaskService>& service, Microsoft::WRL::ComPtr<ITaskFolder>& folder) {
  HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service));
  if (FAILED(hr) || !service) {
    Log(L"host", L"autostart task service create failed hr=0x%08lX", static_cast<unsigned long>(hr));
    return false;
  }
  VARIANT empty;
  VariantInit(&empty);
  hr = service->Connect(empty, empty, empty, empty);
  if (FAILED(hr)) {
    Log(L"host", L"autostart task service connect failed hr=0x%08lX", static_cast<unsigned long>(hr));
    return false;
  }
  hr = service->GetFolder(Bstr(L"\\"), &folder);
  if (FAILED(hr) || !folder) {
    Log(L"host", L"autostart task folder failed hr=0x%08lX", static_cast<unsigned long>(hr));
    return false;
  }
  return true;
}

Microsoft::WRL::ComPtr<IRegisteredTask> GetBamtiTask(ITaskFolder* folder) {
  Microsoft::WRL::ComPtr<IRegisteredTask> task;
  if (folder == nullptr) {
    return task;
  }
  folder->GetTask(Bstr(kTaskName), &task);
  return task;
}

bool TaskIsEnabled(IRegisteredTask* task) {
  if (task == nullptr) {
    return false;
  }
  VARIANT_BOOL enabled = VARIANT_FALSE;
  if (FAILED(task->get_Enabled(&enabled))) {
    return false;
  }
  return enabled == VARIANT_TRUE;
}

std::wstring TaskExecPath(IRegisteredTask* task) {
  if (task == nullptr) {
    return {};
  }
  Microsoft::WRL::ComPtr<ITaskDefinition> def;
  if (FAILED(task->get_Definition(&def)) || !def) {
    return {};
  }
  Microsoft::WRL::ComPtr<IActionCollection> actions;
  if (FAILED(def->get_Actions(&actions)) || !actions) {
    return {};
  }
  Microsoft::WRL::ComPtr<IAction> action;
  if (FAILED(actions->get_Item(1, &action)) || !action) {
    return {};
  }
  Microsoft::WRL::ComPtr<IExecAction> exec;
  if (FAILED(action.As(&exec)) || !exec) {
    return {};
  }
  BSTR path = nullptr;
  if (FAILED(exec->get_Path(&path)) || path == nullptr) {
    return {};
  }
  std::wstring out = path;
  SysFreeString(path);
  return out;
}

bool RegisterBamtiTask(ITaskService* service, ITaskFolder* folder) {
  if (service == nullptr || folder == nullptr) {
    return false;
  }
  const std::wstring user = CurrentUserId();
  const std::wstring path = ModulePath();
  if (user.empty() || path.empty()) {
    Log(L"host", L"autostart task identity failed user_empty=%d path_empty=%d", user.empty() ? 1 : 0,
        path.empty() ? 1 : 0);
    return false;
  }

  Microsoft::WRL::ComPtr<ITaskDefinition> def;
  HRESULT hr = service->NewTask(0, &def);
  if (FAILED(hr) || !def) {
    Log(L"host", L"autostart task NewTask failed hr=0x%08lX", static_cast<unsigned long>(hr));
    return false;
  }

  Microsoft::WRL::ComPtr<IPrincipal> principal;
  hr = def->get_Principal(&principal);
  if (FAILED(hr) || !principal) {
    Log(L"host", L"autostart task principal failed hr=0x%08lX", static_cast<unsigned long>(hr));
    return false;
  }
  principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
  principal->put_RunLevel(TASK_RUNLEVEL_LUA);
  principal->put_UserId(Bstr(user.c_str()));

  Microsoft::WRL::ComPtr<ITaskSettings> settings;
  hr = def->get_Settings(&settings);
  if (FAILED(hr) || !settings) {
    Log(L"host", L"autostart task settings failed hr=0x%08lX", static_cast<unsigned long>(hr));
    return false;
  }
  settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
  settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
  settings->put_ExecutionTimeLimit(Bstr(L"PT0S"));
  settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
  settings->put_StartWhenAvailable(VARIANT_TRUE);

  Microsoft::WRL::ComPtr<IIdleSettings> idle;
  hr = settings->get_IdleSettings(&idle);
  if (SUCCEEDED(hr) && idle) {
    idle->put_StopOnIdleEnd(VARIANT_FALSE);
  }

  Microsoft::WRL::ComPtr<ITriggerCollection> triggers;
  hr = def->get_Triggers(&triggers);
  if (FAILED(hr) || !triggers) {
    Log(L"host", L"autostart task triggers failed hr=0x%08lX", static_cast<unsigned long>(hr));
    return false;
  }
  Microsoft::WRL::ComPtr<ITrigger> trigger;
  hr = triggers->Create(TASK_TRIGGER_LOGON, &trigger);
  if (FAILED(hr) || !trigger) {
    Log(L"host", L"autostart task logon trigger failed hr=0x%08lX", static_cast<unsigned long>(hr));
    return false;
  }
  Microsoft::WRL::ComPtr<ILogonTrigger> logon;
  hr = trigger.As(&logon);
  if (FAILED(hr) || !logon) {
    Log(L"host", L"autostart task logon qi failed hr=0x%08lX", static_cast<unsigned long>(hr));
    return false;
  }
  logon->put_Delay(Bstr(L"PT0S"));
  logon->put_UserId(Bstr(user.c_str()));

  Microsoft::WRL::ComPtr<IActionCollection> actions;
  hr = def->get_Actions(&actions);
  if (FAILED(hr) || !actions) {
    Log(L"host", L"autostart task actions failed hr=0x%08lX", static_cast<unsigned long>(hr));
    return false;
  }
  Microsoft::WRL::ComPtr<IAction> action;
  hr = actions->Create(TASK_ACTION_EXEC, &action);
  if (FAILED(hr) || !action) {
    Log(L"host", L"autostart task exec action failed hr=0x%08lX", static_cast<unsigned long>(hr));
    return false;
  }
  Microsoft::WRL::ComPtr<IExecAction> exec;
  hr = action.As(&exec);
  if (FAILED(hr) || !exec) {
    Log(L"host", L"autostart task exec qi failed hr=0x%08lX", static_cast<unsigned long>(hr));
    return false;
  }
  exec->put_Path(Bstr(path.c_str()));

  VARIANT empty;
  VariantInit(&empty);
  Microsoft::WRL::ComPtr<IRegisteredTask> registered;
  hr = folder->RegisterTaskDefinition(Bstr(kTaskName), def.Get(), TASK_CREATE_OR_UPDATE, empty, empty,
                                      TASK_LOGON_INTERACTIVE_TOKEN, empty, &registered);
  if (FAILED(hr)) {
    Log(L"host", L"autostart task register failed hr=0x%08lX", static_cast<unsigned long>(hr));
    return false;
  }
  return true;
}

bool DeleteBamtiTask(ITaskFolder* folder) {
  if (folder == nullptr) {
    return false;
  }
  if (!GetBamtiTask(folder)) {
    return true;
  }
  const HRESULT hr = folder->DeleteTask(Bstr(kTaskName), 0);
  if (SUCCEEDED(hr)) {
    return true;
  }
  Log(L"host", L"autostart task delete failed hr=0x%08lX", static_cast<unsigned long>(hr));
  return false;
}

}  // namespace

bool AutostartEnabled() {
  ComScope com;
  if (com.ok) {
    Microsoft::WRL::ComPtr<ITaskService> service;
    Microsoft::WRL::ComPtr<ITaskFolder> folder;
    if (ConnectService(service, folder)) {
      Microsoft::WRL::ComPtr<IRegisteredTask> task = GetBamtiTask(folder.Get());
      if (task) {
        return TaskIsEnabled(task.Get());
      }
    }
  }
  return RunValueExists() && StartupApprovedAllows();
}

bool SetAutostart(bool on) {
  if (on) {
    ComScope com;
    if (com.ok) {
      Microsoft::WRL::ComPtr<ITaskService> service;
      Microsoft::WRL::ComPtr<ITaskFolder> folder;
      if (ConnectService(service, folder) && RegisterBamtiTask(service.Get(), folder.Get())) {
        DeleteRunValue();
        Log(L"host", L"autostart task registered path=%s", ModulePath().c_str());
        return true;
      }
    }
    Log(L"host", L"autostart task register failed; falling back to Run key");
    return SetRunAutostart(true);
  }

  bool task_ok = false;
  ComScope com;
  if (com.ok) {
    Microsoft::WRL::ComPtr<ITaskService> service;
    Microsoft::WRL::ComPtr<ITaskFolder> folder;
    if (ConnectService(service, folder)) {
      task_ok = DeleteBamtiTask(folder.Get());
    }
  }
  const bool run_ok = SetRunAutostart(false);
  return task_ok && run_ok;
}

void AutostartMigrate() {
  ComScope com;
  if (!com.ok) {
    Log(L"host", L"autostart migrate task=0 run_removed=0");
    return;
  }
  Microsoft::WRL::ComPtr<ITaskService> service;
  Microsoft::WRL::ComPtr<ITaskFolder> folder;
  if (!ConnectService(service, folder)) {
    Log(L"host", L"autostart migrate task=0 run_removed=0");
    return;
  }

  Microsoft::WRL::ComPtr<IRegisteredTask> task = GetBamtiTask(folder.Get());
  if (task) {
    const std::wstring have = TaskExecPath(task.Get());
    const std::wstring want = ModulePath();
    if (!have.empty() && !want.empty() && lstrcmpiW(have.c_str(), want.c_str()) != 0) {
      RegisterBamtiTask(service.Get(), folder.Get());
    }
    Log(L"host", L"autostart migrate task=1 run_removed=0");
    return;
  }
  if (!RunValueExists()) {
    Log(L"host", L"autostart migrate task=0 run_removed=0");
    return;
  }
  if (!RegisterBamtiTask(service.Get(), folder.Get())) {
    Log(L"host", L"autostart migrate task=0 run_removed=0");
    return;
  }
  const bool removed = DeleteRunValue();
  Log(L"host", L"autostart migrate task=1 run_removed=%d", removed ? 1 : 0);
}

}  // namespace bamti
