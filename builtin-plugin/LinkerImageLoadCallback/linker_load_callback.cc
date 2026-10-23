#include "dobby.h"
#include "common_header.h"
#include "Interceptor.h"
#include "DobbyError.h"
#include "HookLifecycle.h"

#include <dlfcn.h>

#include <vector>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <memory>

namespace {
std::mutex callback_mutex;
struct Callback {
  DobbyCallbackHandle id;
  linker_load_callback_t function;
  size_t active = 0;
  bool enabled = true;
};
std::vector<std::shared_ptr<Callback>> callbacks;
uint64_t next_callback = 1;
std::atomic<size_t> active_loads{0};
struct LoadScope {
  InstrumentCallbackScope retention;
  LoadScope() {
    ++active_loads;
  }
  ~LoadScope() {
    --active_loads;
  }
};
void *loader_target = nullptr;
#if defined(__ANDROID__)
using Loader = void *(*)(const char *, int, const void *);
#else
using Loader = void *(*)(const char *, int);
#endif
std::atomic<Loader> original_loader{nullptr};

void Notify(const char *filename, void *result) {
  if (!result || !filename || dobby_internal_loader_depth)
    return;
  std::vector<std::shared_ptr<Callback>> snapshot;
  {
    std::lock_guard<std::mutex> lock(callback_mutex);
    snapshot = callbacks;
  }
  for (auto &callback : snapshot) {
    {
      std::lock_guard<std::mutex> lock(callback_mutex);
      if (!callback->enabled)
        continue;
      ++callback->active;
    }
    callback->function(filename, result);
    std::lock_guard<std::mutex> lock(callback_mutex);
    --callback->active;
  }
}

#if defined(__ANDROID__)
void *Load(const char *filename, int flags, const void *caller) {
  LoadScope scope;
  void *result = original_loader.load()(filename, flags, caller);
#else
void *Load(const char *filename, int flags) {
  LoadScope scope;
  void *result = original_loader.load()(filename, flags);
#endif
  Notify(filename, result);
  return result;
}
} // namespace

PUBLIC int DobbyRegisterImageLoadCallback(linker_load_callback_t callback, DobbyCallbackHandle *handle) {
  DobbySetError(DOBBY_ERROR_NONE);
  HookOperationGuard operation;
  DobbySetError(DOBBY_ERROR_NONE);
  if (!callback || !handle)
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  *handle = 0;
  if (!loader_target) {
#if defined(__ANDROID__)
    loader_target = DobbySymbolResolver(nullptr, "__loader_dlopen");
#else
    loader_target = DobbySymbolResolver(nullptr, "dlopen");
#endif
    if (!loader_target) {
      DobbySetError(DOBBY_ERROR_NOT_FOUND);
      return RS_FAILED;
    }
  }
  void *original = nullptr;
  if (DobbyPrepare(loader_target, reinterpret_cast<void *>(Load), &original) != RS_SUCCESS)
    return RS_FAILED;
  original_loader = reinterpret_cast<Loader>(original);
  if (DobbyCommit(loader_target) != RS_SUCCESS)
    return RS_FAILED;
  std::lock_guard<std::mutex> lock(callback_mutex);
  for (const auto &existing : callbacks) {
    if (existing->function == callback) {
      *handle = existing->id;
      return DobbySuccess();
    }
  }
  if (!next_callback)
    return DobbyFailure(DOBBY_ERROR_BUSY);
  auto entry = std::make_shared<Callback>();
  entry->id = next_callback++;
  entry->function = callback;
  callbacks.push_back(entry);
  *handle = entry->id;
  return DobbySuccess();
}
PUBLIC void dobby_register_image_load_callback(linker_load_callback_t callback) {
  DobbyCallbackHandle handle;
  DobbyRegisterImageLoadCallback(callback, &handle);
}
PUBLIC int DobbyUnregisterImageLoadCallback(DobbyCallbackHandle handle) {
  DobbySetError(DOBBY_ERROR_NONE);
  std::lock_guard<std::mutex> lock(callback_mutex);
  auto it = std::find_if(callbacks.begin(), callbacks.end(), [&](const auto &entry) { return entry->id == handle; });
  if (it == callbacks.end())
    return DobbyFailure(DOBBY_ERROR_NOT_FOUND);
  if ((*it)->active)
    return DobbyFailure(DOBBY_ERROR_BUSY);
  (*it)->enabled = false;
  callbacks.erase(it);
  return DobbySuccess();
}
PUBLIC int DobbyStopImageLoadMonitor(void) {
  DobbySetError(DOBBY_ERROR_NONE);
  HookOperationGuard operation;
  {
    std::lock_guard<std::mutex> lock(callback_mutex);
    if (!callbacks.empty() || active_loads.load())
      return DobbyFailure(DOBBY_ERROR_BUSY);
  }
  if (!loader_target)
    return DobbySuccess();
  DobbyHookInfo info{};
  info.struct_size = sizeof(info);
  if (DobbyQueryHook(loader_target, &info) == RS_SUCCESS) {
    if (info.replacement != reinterpret_cast<void *>(Load))
      return DobbyFailure(DOBBY_ERROR_CONFLICT);
    if (DobbyDestroy(loader_target) != RS_SUCCESS)
      return RS_FAILED;
  }
  loader_target = nullptr;
  return DobbySuccess();
}

bool DobbyCallbacksUseRange(uintptr_t start, size_t size) {
  std::lock_guard<std::mutex> lock(callback_mutex);
  for (const auto &entry : callbacks) {
    uintptr_t address = reinterpret_cast<uintptr_t>(entry->function) & ~uintptr_t{1};
    if (address >= start && address - start < size)
      return true;
  }
  return false;
}
