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
  if (DobbyInPatchCoordinator())
    return DobbyFailure(DOBBY_ERROR_BUSY);
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
  if (DobbyInPatchCoordinator())
    return DobbyFailure(DOBBY_ERROR_BUSY);
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

namespace {
DobbyPatchCoordinator patch_coordinator{};
thread_local bool in_patch_coordinator = false;
} // namespace
PUBLIC int DobbySetPatchCoordinator(const DobbyPatchCoordinator *coordinator) {
  HookOperationGuard guard;
  if (in_patch_coordinator)
    return DobbyFailure(DOBBY_ERROR_BUSY);
  if (coordinator && (coordinator->struct_size < sizeof(*coordinator) ||
                      coordinator->api_version != DOBBY_API_VERSION || !coordinator->enter || !coordinator->leave))
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  patch_coordinator = coordinator ? *coordinator : DobbyPatchCoordinator{};
  DobbySetError(DOBBY_ERROR_NONE);
  return RS_SUCCESS;
}
PatchScope::PatchScope(void *address, size_t size, DobbyPatchOperation operation)
    : address_(address), size_(size), operation_(operation), coordinator_(patch_coordinator) {
  if (in_patch_coordinator) {
    DobbySetError(DOBBY_ERROR_BUSY);
    return;
  }
  if (!coordinator_.enter) {
    ready_ = true;
    return;
  }
  in_patch_coordinator = true;
  entered_ = coordinator_.enter(address_, size_, operation_, coordinator_.userdata) == RS_SUCCESS;
  ready_ = entered_;
  if (!entered_) {
    in_patch_coordinator = false;
    DobbySetError(DOBBY_ERROR_BUSY);
  }
}
PatchScope::~PatchScope() {
  DobbyRecordEvent(DOBBY_EVENT_PATCH, DobbyGetLastError(), address_, size_, (uint64_t(operation_) << 1) | applied_,
                   DobbyGetLastSystemError());
  if (!entered_)
    return;
  const auto error = DobbyGetLastError();
  const auto system_error = DobbyGetLastSystemError();
  coordinator_.leave(address_, size_, operation_, applied_, coordinator_.userdata);
  in_patch_coordinator = false;
  DobbySetError(error, system_error);
}

bool DobbyInPatchCoordinator() {
  return in_patch_coordinator;
}
bool DobbyCoordinatorUsesRange(uintptr_t start, size_t size) {
  const uintptr_t enter = reinterpret_cast<uintptr_t>(patch_coordinator.enter) & ~uintptr_t{1};
  const uintptr_t leave = reinterpret_cast<uintptr_t>(patch_coordinator.leave) & ~uintptr_t{1};
  return (enter >= start && enter - start < size) || (leave >= start && leave - start < size);
}
