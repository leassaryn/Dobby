#include "dobby_internal.h"

#include "Interceptor.h"
#include "InterceptRouting/InterceptRouting.h"
#include "InterceptRouting/Routing/FunctionInlineReplace/function-inline-replace.h"

PUBLIC int DobbyHook(void *address, void *replace_call, void **origin_call) {
  HookOperationGuard operation;
  DobbySetError(DOBBY_ERROR_NONE);
  int result = DobbyPrepare(address, replace_call, origin_call);
  if (result != RS_SUCCESS)
    return result;
  return DobbyCommit(address);
}

static int Prepare(void *address, void *replace_call, void **origin_call, int near_policy, size_t max_patch_bytes) {
  if (DobbyInPatchCoordinator())
    return DobbyFailure(DOBBY_ERROR_BUSY);
  HookOperationGuard operation;
  DobbySetError(DOBBY_ERROR_NONE);
  if (!address || !replace_call) {
    ERROR_LOG("function address or replacement is null");
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  }

#if defined(TARGET_ARCH_ARM64)
  if ((reinterpret_cast<uintptr_t>(address) & 3) != 0)
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
#endif

  DLOG(0, "[DobbyPrepare] Initialize at %p", address);

  // check if already hooked
  HookEntry *entry = Interceptor::SharedInstance()->FindHookEntry(address);
  if (entry) {
    // reject conflicts: this address is already hooked with a different mechanism
    if (entry->type != kFunctionInlineHook) {
      ERROR_LOG("address %p is already instrumented; destroy it before hooking", address);
      return DobbyFailure(DOBBY_ERROR_CONFLICT);
    }
    FunctionInlineReplaceRouting *route = (FunctionInlineReplaceRouting *)entry->route;
    if (route->GetTrampolineTarget() == replace_call) {
      if (near_policy != -1 && (entry->near_policy != near_policy || entry->max_patch_bytes != max_patch_bytes))
        return DobbyFailure(DOBBY_ERROR_CONFLICT);
      // idempotent: already hooked with the same replacement; succeed as a no-op
      if (origin_call)
        *origin_call = entry->relocated_origin_function;
      return DobbySuccess();
    }
    ERROR_LOG("address %p already has a different replacement; destroy it before hooking", address);
    return DobbyFailure(DOBBY_ERROR_CONFLICT);
  }

  entry = new HookEntry();
  entry->id = Interceptor::SharedInstance()->GetHookEntryCount();
  entry->type = kFunctionInlineHook;
  entry->function_address = address;
  entry->near_policy = near_policy;
  entry->max_patch_bytes = max_patch_bytes;

  FunctionInlineReplaceRouting *route = new FunctionInlineReplaceRouting(entry, replace_call);
  route->Prepare();
  route->DispatchRouting();

  // abort if relocation/trampoline generation failed
  if (!route->IsReady()) {
    ERROR_LOG("failed to prepare hook for %p", address);
    route->Destroy();
    delete route;
    delete entry;
    if (DobbyGetLastError() == DOBBY_ERROR_NONE)
      DobbySetError(DOBBY_ERROR_RELOCATION_FAILED);
    return RS_FAILED;
  }

  Interceptor::SharedInstance()->AddHookEntry(entry);
  DobbyHookInfo prepared{sizeof(prepared)};
  if (DobbyQueryHook(address, &prepared) == RS_SUCCESS)
    DobbyRecordEvent(DOBBY_EVENT_PREPARE, DOBBY_ERROR_NONE, address, prepared.patch_size, prepared.relay ? 1 : 0);

  // set origin call with relocated function
  if (origin_call)
    *origin_call = entry->relocated_origin_function;

  return DobbySuccess();
}

PUBLIC int DobbyCommit(void *address) {
  if (DobbyInPatchCoordinator())
    return DobbyFailure(DOBBY_ERROR_BUSY);
  HookOperationGuard operation;
  DobbySetError(DOBBY_ERROR_NONE);
  if (!address) {
    ERROR_LOG("function address is 0x0");
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  }

  // check if already hooked
  HookEntry *entry = Interceptor::SharedInstance()->FindHookEntry(address);
  if (!entry || !entry->route || entry->type != kFunctionInlineHook) {
    ERROR_LOG("function %p is not prepared/hooked.", address);
    return DobbyFailure(entry ? DOBBY_ERROR_CONFLICT : DOBBY_ERROR_NOT_FOUND);
  }
  FunctionInlineReplaceRouting *route = (FunctionInlineReplaceRouting *)entry->route;
  if (entry->is_committed) {
    // idempotent: already committed; succeed as a no-op
    return DobbySuccess();
  }

  // code patch & hijack original control flow entry
  return route->Commit() ? RS_SUCCESS : RS_FAILED;
}

PUBLIC int DobbyPrepare(void *address, void *replacement, void **original) {
  return Prepare(address, replacement, original, -1, 0);
}
PUBLIC int DobbyPrepareEx(void *address, void *replacement, void **original, const DobbyHookOptions *options) {
  if (options && (options->struct_size < sizeof(*options) || options->api_version != DOBBY_API_VERSION ||
                  options->near_policy < DOBBY_NEAR_AUTO || options->near_policy > DOBBY_NEAR_DISABLED))
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
#if !defined(TARGET_ARCH_ARM64) || !defined(DOBBY_HAS_NEAR_BRANCH)
  if (options && options->near_policy == DOBBY_NEAR_REQUIRED)
    return DobbyFailure(DOBBY_ERROR_UNSUPPORTED_FORMAT);
#endif
  return Prepare(address, replacement, original, options ? options->near_policy : DOBBY_NEAR_AUTO,
                 options ? options->max_patch_bytes : 0);
}
PUBLIC int DobbyHookEx(void *address, void *replacement, void **original, const DobbyHookOptions *options) {
  HookOperationGuard operation;
  if (DobbyPrepareEx(address, replacement, original, options) != RS_SUCCESS)
    return RS_FAILED;
  return DobbyCommit(address);
}
