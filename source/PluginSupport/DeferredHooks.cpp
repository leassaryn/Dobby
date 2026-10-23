#include "dobby_internal.h"
#include "Interceptor.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace {
struct Deferred {
  std::string image, symbol;
  std::vector<uint8_t> build_id, expected;
  DobbyAddressSpec spec{};
  DobbyHookOptions options{};
  DobbyModuleHandle module = 0;
  void *replacement = nullptr, *target = nullptr, *original = nullptr;
  DobbyDeferredState state = DOBBY_DEFERRED_PENDING;
  DobbyError error = DOBBY_ERROR_NONE;
  bool cancelling = false;
};
auto &Entries() {
  static std::unordered_map<uint64_t, std::unique_ptr<Deferred>> values;
  return values;
}
uint64_t next_id = 1;
DobbyCallbackHandle monitor = 0;
bool polling = false;
bool repoll = false;
struct PollLease {
  bool active = true;
  ~PollLease() {
    if (active) {
      HookOperationGuard guard;
      polling = false;
    }
  }
};
void TryInstall(uint64_t id, Deferred &entry) {
  // Polling excludes registry mutation. Loader calls run without the operation lock.
  DobbyModuleHandle module = entry.module;
  if (!module && DobbyAcquireModule(entry.image.c_str(), &module) != RS_SUCCESS) {
    const auto error = DobbyGetLastError();
    HookOperationGuard guard;
    entry.error = error;
    entry.state = error == DOBBY_ERROR_NOT_FOUND ? DOBBY_DEFERRED_PENDING : DOBBY_DEFERRED_FAILED;
    return;
  }
  void *target = nullptr;
  const int resolved = DobbyResolveModuleAddress(module, &entry.spec, &target);
  const auto resolve_error = DobbyGetLastError();
  HookOperationGuard guard;
  entry.module = module;
  entry.target = target;
  entry.state = DOBBY_DEFERRED_FAILED;
  if (resolved != RS_SUCCESS) {
    entry.error = resolve_error;
  } else {
    DobbyHookInfo existing{sizeof(existing)};
    if (DobbyQueryHook(entry.target, &existing) == RS_SUCCESS) {
      entry.error = DOBBY_ERROR_CONFLICT;
    } else if (DobbyPrepareEx(entry.target, entry.replacement, &entry.original, &entry.options) != RS_SUCCESS) {
      entry.error = DobbyGetLastError();
    } else if (DobbyCommit(entry.target) == RS_SUCCESS) {
      entry.state = DOBBY_DEFERRED_INSTALLED;
      entry.error = DobbyGetLastError();
    } else {
      entry.error = DobbyGetLastError();
      if (DobbyDestroy(entry.target) == RS_SUCCESS)
        entry.original = nullptr;
    }
  }
  DobbyRecordEvent(DOBBY_EVENT_DEFERRED, entry.error, entry.target, 0, id);
}
#ifdef DOBBY_HAS_LINKER_CALLBACK
void Loaded(const char *, void *) {
  DobbyPollDeferredHooks();
}
#endif
} // namespace
PUBLIC int DobbyRegisterDeferredHook(const char *image, const DobbyAddressSpec *spec, void *replacement,
                                     const DobbyHookOptions *options, DobbyDeferredHandle *handle) {
  if (DobbyInPatchCoordinator())
    return DobbyFailure(DOBBY_ERROR_BUSY);
  if (!image || !*image || !replacement || !handle || !spec || spec->struct_size < sizeof(*spec) ||
      spec->api_version != DOBBY_API_VERSION || spec->build_id_size > 64 || spec->expected_size > 64 ||
      (spec->build_id_size && !spec->build_id) || (spec->expected_size && !spec->expected_bytes) ||
      (spec->symbol && !*spec->symbol) ||
      (options && (options->struct_size < sizeof(*options) || options->api_version != DOBBY_API_VERSION ||
                   options->near_policy < DOBBY_NEAR_AUTO || options->near_policy > DOBBY_NEAR_DISABLED)))
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  *handle = 0;
  {
    HookOperationGuard guard;
    if (!next_id || polling)
      return DobbyFailure(DOBBY_ERROR_BUSY);
    polling = true; // Exclude shutdown/cancel while obtaining the monitor outside the lock.
  }
  PollLease lease;
  DobbyCallbackHandle subscription = 0;
#ifdef DOBBY_HAS_LINKER_CALLBACK
  const int subscribed = DobbyRegisterImageLoadCallback(Loaded, &subscription);
  if (subscribed != RS_SUCCESS) {
    HookOperationGuard guard;
    polling = false;
    lease.active = false;
    return RS_FAILED;
  }
#endif
  auto entry = std::make_unique<Deferred>();
  entry->image = image;
  entry->spec = *spec;
  if (spec->symbol) {
    entry->symbol = spec->symbol;
    entry->spec.symbol = entry->symbol.c_str();
  }
  if (spec->build_id_size) {
    entry->build_id.assign(spec->build_id, spec->build_id + spec->build_id_size);
    entry->spec.build_id = entry->build_id.data();
  }
  if (spec->expected_size) {
    entry->expected.assign(spec->expected_bytes, spec->expected_bytes + spec->expected_size);
    entry->spec.expected_bytes = entry->expected.data();
  }
  entry->options =
      options ? *options : DobbyHookOptions{sizeof(DobbyHookOptions), DOBBY_API_VERSION, DOBBY_NEAR_AUTO, 0};
  entry->replacement = replacement;
  {
    HookOperationGuard guard;
    monitor = subscription;
    const auto id = next_id++;
    Entries().emplace(id, std::move(entry));
    *handle = id;
    polling = false;
    lease.active = false;
  }
  // Registration succeeds even when installation is deferred or fails: inspect the handle.
  DobbyPollDeferredHooks();
  return DobbySuccess();
}
PUBLIC int DobbyPollDeferredHooks(void) {
  if (DobbyInPatchCoordinator())
    return DobbyFailure(DOBBY_ERROR_BUSY);
  {
    HookOperationGuard guard;
    if (polling) {
      repoll = true;
      return DobbyFailure(DOBBY_ERROR_BUSY);
    }
    polling = true;
  }
  PollLease lease;
  for (;;) {
    std::vector<std::pair<uint64_t, Deferred *>> pending;
    {
      HookOperationGuard guard;
      repoll = false;
      for (auto &[id, entry] : Entries())
        if (!entry->original && !entry->cancelling)
          pending.emplace_back(id, entry.get());
    }
    for (const auto &[id, entry] : pending)
      TryInstall(id, *entry);
    {
      HookOperationGuard guard;
      if (repoll)
        continue;
      polling = false;
      lease.active = false;
      break;
    }
  }
  return DobbySuccess();
}
PUBLIC int DobbyQueryDeferredHook(DobbyDeferredHandle handle, DobbyDeferredInfo *info) {
  HookOperationGuard guard;
  if (!info || info->struct_size < sizeof(*info) || info->api_version != DOBBY_API_VERSION)
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  const auto it = Entries().find(handle);
  if (it == Entries().end())
    return DobbyFailure(DOBBY_ERROR_NOT_FOUND);
  auto &e = *it->second;
  *info = {sizeof(*info), DOBBY_API_VERSION, e.state, e.error, e.target, e.original};
  return DobbySuccess();
}
PUBLIC int DobbyCancelDeferredHook(DobbyDeferredHandle handle) {
  if (DobbyInPatchCoordinator())
    return DobbyFailure(DOBBY_ERROR_BUSY);
  DobbyModuleHandle module = 0;
  {
    HookOperationGuard guard;
    if (polling)
      return DobbyFailure(DOBBY_ERROR_BUSY);
    auto it = Entries().find(handle);
    if (it == Entries().end())
      return DobbyFailure(DOBBY_ERROR_NOT_FOUND);
    auto &e = *it->second;
    e.cancelling = true;
    if (e.original) {
      DobbyHookInfo info{sizeof(info)};
      if (DobbyQueryHook(e.target, &info) != RS_SUCCESS || info.replacement != e.replacement ||
          info.original != e.original)
        return DobbyFailure(DOBBY_ERROR_CONFLICT);
      if (DobbyDestroy(e.target) != RS_SUCCESS)
        return RS_FAILED;
      e.original = nullptr;
      e.state = DOBBY_DEFERRED_FAILED;
      e.error = DOBBY_ERROR_BUSY;
    }
    module = e.module;
    polling = true;
  }
  // Releasing the final dlopen pin can execute foreign destructors.
  PollLease lease;
  const int released = module ? DobbyReleaseModule(module) : RS_SUCCESS;
  const auto error = DobbyGetLastError();
  const auto system_error = DobbyGetLastSystemError();
  bool retry = false;
  {
    HookOperationGuard guard;
    polling = false;
    lease.active = false;
    retry = repoll;
    if (released == RS_SUCCESS)
      Entries().erase(handle);
  }
  if (retry)
    DobbyPollDeferredHooks();
  if (released != RS_SUCCESS) {
    DobbySetError(error, system_error);
    return RS_FAILED;
  }
  return DobbySuccess();
}
PUBLIC int DobbyQueryPluginState(DobbyPluginState *state) {
  HookOperationGuard guard;
  if (!state || state->struct_size < sizeof(*state) || state->api_version != DOBBY_API_VERSION)
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  *state = {sizeof(*state), DOBBY_API_VERSION};
  for (const auto &[id, e] : Entries()) {
    if (e->state == DOBBY_DEFERRED_INSTALLED)
      ++state->deferred_installed;
    else if (e->state == DOBBY_DEFERRED_PENDING)
      ++state->deferred_pending;
    else
      ++state->deferred_failed;
  }
#ifdef DOBBY_HAS_LINKER_CALLBACK
  extern void DobbyGetImageMonitorState(size_t &, int &);
  DobbyGetImageMonitorState(state->image_callbacks, state->image_monitor_active);
#endif
  extern void DobbyGetModulePluginState(size_t &, size_t &);
  DobbyGetModulePluginState(state->module_handles, state->import_hooks);
  return DobbySuccess();
}
PUBLIC int DobbyShutdownPlugins(void) {
  if (DobbyInPatchCoordinator())
    return DobbyFailure(DOBBY_ERROR_BUSY);
  HookOperationGuard guard;
  size_t modules = 0, imports = 0;
  extern void DobbyGetModulePluginState(size_t &, size_t &);
  DobbyGetModulePluginState(modules, imports);
  if (!Entries().empty() || polling || modules || imports)
    return DobbyFailure(DOBBY_ERROR_BUSY);
#ifdef DOBBY_HAS_LINKER_CALLBACK
  if (monitor) {
    if (DobbyUnregisterImageLoadCallback(monitor) != RS_SUCCESS)
      return RS_FAILED;
    monitor = 0;
  }
  if (DobbyStopImageLoadMonitor() != RS_SUCCESS)
    return RS_FAILED;
#endif
  return DobbySetLinkerNamespaceBypass(0);
}
bool DobbyDeferredUsesRange(uintptr_t start, size_t size) {
  for (const auto &[id, e] : Entries()) {
    const auto address = reinterpret_cast<uintptr_t>(e->replacement) & ~uintptr_t{1};
    if (!e->cancelling && address >= start && address - start < size)
      return true;
  }
  return false;
}

#ifndef DOBBY_HAS_LINKER_BYPASS
PUBLIC int DobbySetLinkerNamespaceBypass(int enabled) {
  return enabled ? DobbyFailure(DOBBY_ERROR_UNSUPPORTED_FORMAT) : DobbySuccess();
}
#endif
