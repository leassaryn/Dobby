#include "bionic_linker_restriction.h"
#include "dobby_internal.h"
#include "Interceptor.h"
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
const char *LinkerPath() {
  char sdk[PROP_VALUE_MAX]{};
  if (__system_property_get("ro.build.version.sdk", sdk) <= 0)
    return nullptr;
  char *end = nullptr;
  const auto api = strtol(sdk, &end, 10);
  if (!end || *end || api < 21)
    return nullptr;
#if __LP64__
  return api >= 29 ? "/apex/com.android.runtime/bin/linker64" : "/system/bin/linker64";
#else
  return api >= 29 ? "/apex/com.android.runtime/bin/linker" : "/system/bin/linker";
#endif
}
void *InternalSymbol(const char *name) {
  const char *path = LinkerPath();
  void *symbol = path ? DobbySymbolResolver(path, name) : nullptr;
  if (!symbol)
    DobbySetError(DOBBY_ERROR_UNSUPPORTED_FORMAT);
  return symbol;
}
void *bypass_target = nullptr, *bypass_original = nullptr;
bool Accessible(void *, const std::string &) {
  return true;
}
} // namespace
PUBLIC void *linker_dlopen(const char *filename, int flags) {
  using Loader = void *(*)(const char *, int, const void *);
  auto loader = reinterpret_cast<Loader>(DobbySymbolResolver(nullptr, "__loader_dlopen"));
  const auto caller = dlsym(RTLD_DEFAULT, "open");
  if (!loader || !caller) {
    DobbySetError(DOBBY_ERROR_UNSUPPORTED_FORMAT);
    return nullptr;
  }
  auto result = loader(filename, flags, caller);
  DobbySetError(result ? DOBBY_ERROR_NONE : DOBBY_ERROR_NOT_FOUND);
  return result;
}
// Private soinfo lists have no stable layout or public locking contract.
// Legacy entry points remain link-compatible, but never guess object offsets.
std::vector<soinfo_t> linker_get_solist() {
  DobbySetError(DOBBY_ERROR_UNSUPPORTED_FORMAT);
  return {};
}
PUBLIC char *linker_soinfo_get_realpath(soinfo_t info) {
  if (!info) {
    DobbySetError(DOBBY_ERROR_INVALID_ARGUMENT);
    return nullptr;
  }
  using Function = char *(*)(soinfo_t);
  auto function = reinterpret_cast<Function>(InternalSymbol("__dl__ZNK6soinfo12get_realpathEv"));
  if (!function)
    return nullptr;
  DobbySetError(DOBBY_ERROR_NONE);
  return function(info);
}
PUBLIC uintptr_t linker_soinfo_to_handle(soinfo_t info) {
  if (!info) {
    DobbySetError(DOBBY_ERROR_INVALID_ARGUMENT);
    return 0;
  }
  using Function = uintptr_t (*)(soinfo_t);
  auto function = reinterpret_cast<Function>(InternalSymbol("__dl__ZN6soinfo9to_handleEv"));
  if (!function)
    return 0;
  DobbySetError(DOBBY_ERROR_NONE);
  return function(info);
}
PUBLIC void linker_iterate_soinfo(int (*callback)(soinfo_t)) {
  DobbySetError(callback ? DOBBY_ERROR_UNSUPPORTED_FORMAT : DOBBY_ERROR_INVALID_ARGUMENT);
}
PUBLIC int DobbySetLinkerNamespaceBypass(int enabled) {
  if (DobbyInPatchCoordinator())
    return DobbyFailure(DOBBY_ERROR_BUSY);
  void *resolved = enabled ? InternalSymbol("__dl__ZN19android_namespace_t13is_accessibleERKNSt3__112basic_stringIcNS0_"
                                            "11char_traitsIcEENS0_9allocatorIcEEEE")
                           : nullptr;
  HookOperationGuard guard;
  if (bypass_target) {
    DobbyHookInfo info{sizeof(info)};
    if (DobbyQueryHook(bypass_target, &info) != RS_SUCCESS ||
        info.replacement != reinterpret_cast<void *>(Accessible) || info.original != bypass_original)
      return DobbyFailure(DOBBY_ERROR_CONFLICT);
    if (enabled)
      return DobbyCommit(bypass_target);
    if (DobbyDestroy(bypass_target) != RS_SUCCESS)
      return RS_FAILED;
    bypass_target = bypass_original = nullptr;
    return DobbySuccess();
  }
  if (!enabled)
    return DobbySuccess();
  void *target = resolved;
  if (!target)
    return RS_FAILED;
  DobbyHookInfo info{sizeof(info)};
  if (DobbyQueryHook(target, &info) == RS_SUCCESS)
    return DobbyFailure(DOBBY_ERROR_CONFLICT);
  if (DobbyPrepare(target, reinterpret_cast<void *>(Accessible), &bypass_original) != RS_SUCCESS)
    return RS_FAILED;
  bypass_target = target; // Preserve ownership on commit failure, allowing explicit disable/retry.
  return DobbyCommit(target);
}
PUBLIC void linker_disable_namespace_restriction() {
  DobbySetLinkerNamespaceBypass(1);
}
