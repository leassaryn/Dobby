#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#include <syslog.h>
#endif

#include "dobby_internal.h"
#include "Interceptor.h"
#include "HookLifecycle.h"
#include "InterceptRouting/InterceptRouting.h"

#if defined(__linux__) || defined(__APPLE__)
__attribute__((constructor)) static void ctor() {
  DLOG(-1, "================================");
  DLOG(-1, "Dobby");
  DLOG(-1, "================================");

  DLOG(-1, "dobby in debug log mode, disable with cmake flag \"-DDOBBY_DEBUG=OFF\"");
}
#endif

PUBLIC const char *DobbyBuildVersion() {
  return __DOBBY_BUILD_VERSION__;
}

PUBLIC int DobbyEnumerateHooks(dobby_hook_iterator_t cb, void *userdata) {
  DobbySetError(DOBBY_ERROR_NONE);
  if (cb == NULL)
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);

  Interceptor::SharedInstance()->Iterate(cb, userdata);
  return DobbySuccess();
}

PUBLIC int DobbyDestroy(void *address) {
  if (DobbyInPatchCoordinator())
    return DobbyFailure(DOBBY_ERROR_BUSY);
  HookOperationGuard operation;
  DobbySetError(DOBBY_ERROR_NONE);
  if (!address)
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  // check if we already hook
  HookEntry *entry = Interceptor::SharedInstance()->FindHookEntry(address);
  if (entry) {
    if (entry->is_committed) {
      uint8_t *buffer = entry->origin_chunk_.chunk_buffer;
      uint32_t buffer_size = entry->origin_chunk_.chunk.length;
      void *patch_address = entry->origin_chunk_.chunk.address;
      PatchScope patch(patch_address, buffer_size, DOBBY_PATCH_REMOVE);
      if (!patch.ready())
        return RS_FAILED;
      auto *route = static_cast<InterceptRouting *>(entry->route);
      auto *installed = route->GetTrampolineBuffer();
      if (memcmp(patch_address, installed->getRawBuffer(), installed->getSize()) != 0 ||
          (installed->getSize() < buffer_size &&
           memcmp(static_cast<uint8_t *>(patch_address) + installed->getSize(), buffer + installed->getSize(),
                  buffer_size - installed->getSize()) != 0))
        return DobbyFailure(DOBBY_ERROR_CONFLICT);
      if (CodePatch(patch_address, buffer, buffer_size) != kMemoryOperationSuccess)
        return RT_FAILED;
      patch.Applied();
    }

    Interceptor::SharedInstance()->RemoveHookEntry(address);
    RetireHook(entry);
    return RT_SUCCESS;
  }

  return DobbyFailure(DOBBY_ERROR_NOT_FOUND);
}

PUBLIC int DobbyQueryHook(void *address, DobbyHookInfo *info) {
  HookOperationGuard operation;
  DobbySetError(DOBBY_ERROR_NONE);
  if (!address || !info || info->struct_size < sizeof(DobbyHookInfo))
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  HookEntry *entry = Interceptor::SharedInstance()->FindHookEntry(address);
  if (!entry)
    return DobbyFailure(DOBBY_ERROR_NOT_FOUND);
  auto *route = static_cast<InterceptRouting *>(entry->route);
  DobbyHookInfo result{};
  result.struct_size = sizeof(result);
  result.kind = entry->type == kFunctionInlineHook ? DOBBY_HOOK_INLINE : DOBBY_HOOK_INSTRUMENT;
  result.state = entry->is_committed ? DOBBY_HOOK_COMMITTED : DOBBY_HOOK_PREPARED;
  result.target = entry->target_address;
  result.patch_address = entry->origin_chunk_.chunk.address;
  result.original = entry->relocated_origin_instructions;
  result.replacement = route->GetUserReplacement();
  result.patch_size = route->GetTrampolineBuffer()->getSize();
  result.relay = route->GetRelayAddress();
  *info = result;
  return DobbySuccess();
}
