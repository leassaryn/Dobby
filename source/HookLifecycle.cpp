#include "HookLifecycle.h"
#include "Interceptor.h"
#include "InterceptRouting/InterceptRouting.h"
#include "MemoryAllocator/NearMemoryArena.h"
#include <atomic>
#include <errno.h>

thread_local unsigned dobby_internal_loader_depth = 0;

namespace {
HookEntry *retired_hooks = nullptr;
size_t retired_count = 0;
thread_local unsigned callback_depth = 0;
std::atomic<size_t> active_callbacks{0};
} // namespace

InstrumentCallbackScope::InstrumentCallbackScope() {
  ++callback_depth;
  ++active_callbacks;
}
InstrumentCallbackScope::~InstrumentCallbackScope() {
  --active_callbacks;
  --callback_depth;
}

void RetireHook(HookEntry *entry) {
  entry->retired_next = retired_hooks;
  retired_hooks = entry;
  ++retired_count;
}

PUBLIC int DobbyCollectRetiredHooks(size_t *released_count) {
  HookOperationGuard operation;
  DobbySetError(DOBBY_ERROR_NONE);
  if (released_count)
    *released_count = 0;
  if (callback_depth != 0 || active_callbacks.load() != 0)
    return DobbyFailure(DOBBY_ERROR_BUSY);
  const size_t count = retired_count;
  while (retired_hooks) {
    HookEntry *entry = retired_hooks;
    retired_hooks = entry->retired_next;
    auto *route = static_cast<InterceptRouting *>(entry->route);
    route->Destroy();
    delete route;
    delete entry;
  }
  retired_count = 0;
  if (released_count)
    *released_count = count;
  return DobbySuccess();
}

PUBLIC int DobbyGetResourceStats(DobbyResourceStats *stats) {
  HookOperationGuard operation;
  DobbySetError(DOBBY_ERROR_NONE);
  if (!stats || stats->struct_size < sizeof(DobbyResourceStats))
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  DobbyResourceStats result{};
  result.struct_size = sizeof(result);
  result.registered_hooks = Interceptor::SharedInstance()->GetHookEntryCount();
  result.retired_hooks = retired_count;
  result.retained_code_bytes = MemoryArena::RetainedBytes();
#ifdef DOBBY_HAS_NEAR_BRANCH
  result.retained_code_bytes += NearMemoryArena::RetainedBytes();
#endif
  *stats = result;
  return DobbySuccess();
}

PUBLIC int DobbyReleaseCodeMemory(size_t *released_bytes) {
  HookOperationGuard operation;
  DobbySetError(DOBBY_ERROR_NONE);
  if (released_bytes)
    *released_bytes = 0;
  if (active_callbacks.load() != 0 || Interceptor::SharedInstance()->GetHookEntryCount() != 0)
    return DobbyFailure(DOBBY_ERROR_BUSY);
  if (DobbyCollectRetiredHooks(nullptr) != RS_SUCCESS)
    return RS_FAILED;
#ifdef DOBBY_HAS_DBI
  extern void reset_closure_bridge();
  reset_closure_bridge();
#endif
#ifdef TARGET_ARCH_X64
  extern void ResetIndirectStubs();
  ResetIndirectStubs();
#endif
  size_t released = 0;
  bool success = MemoryArena::ReleaseAll(released);
#ifdef DOBBY_HAS_NEAR_BRANCH
  success = NearMemoryArena::ReleaseAll(released) && success;
#endif
  if (released_bytes)
    *released_bytes = released;
  if (!success)
    return DobbyFailure(DOBBY_ERROR_MEMORY_RELEASE_FAILED, errno);
  return DobbySuccess();
}
