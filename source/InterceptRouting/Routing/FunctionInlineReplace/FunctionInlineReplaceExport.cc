#include "dobby_internal.h"

#include "Interceptor.h"
#include "InterceptRouting/InterceptRouting.h"
#include "InterceptRouting/Routing/FunctionInlineReplace/function-inline-replace.h"

PUBLIC int DobbyHook(void *address, void *replace_call, void **origin_call) {
  int result = DobbyPrepare(address, replace_call, origin_call);
  if (result != RS_SUCCESS)
    return result;
  return DobbyCommit(address);
}

PUBLIC int DobbyPrepare(void *address, void *replace_call, void **origin_call) {
  if (!address) {
    ERROR_LOG("function address is 0x0");
    return RS_FAILED;
  }

  DLOG(0, "[DobbyPrepare] Initialize at %p", address);

  // check if already hooked
  HookEntry *entry = Interceptor::SharedInstance()->FindHookEntry(address);
  if (entry) {
    // reject conflicts: this address is already hooked with a different mechanism
    if (entry->type != kFunctionInlineHook) {
      ERROR_LOG("address %p is already instrumented; destroy it before hooking", address);
      return RS_FAILED;
    }
    FunctionInlineReplaceRouting *route = (FunctionInlineReplaceRouting *)entry->route;
    if (route->GetTrampolineTarget() == replace_call) {
      // idempotent: already hooked with the same replacement; succeed as a no-op
      if (origin_call)
        *origin_call = entry->relocated_origin_function;
      return RS_SUCCESS;
    }
  }

  // check if was previously hooked and rehook if needed
  entry = Interceptor::SharedInstanceOriginal()->FindHookEntry(address);
  if (entry) {
    if (entry->type != kFunctionInlineHook) {
      ERROR_LOG("address %p is already instrumented; destroy it before hooking", address);
      return RS_FAILED;
    }
    FunctionInlineReplaceRouting *route = (FunctionInlineReplaceRouting *)entry->route;
    if (route->GetTrampolineTarget() != replace_call) {
      // no need to regenerate relocated buffer because its size and contents are the same
      // TODO: Check ARM
      route->SetReplaceCall(replace_call);
    }
    Interceptor::SharedInstance()->AddHookEntry(entry);
    if (origin_call)
      *origin_call = entry->relocated_origin_function;
    return RS_SUCCESS;
  }

  entry = new HookEntry();
  entry->id = Interceptor::SharedInstance()->GetHookEntryCount();
  entry->type = kFunctionInlineHook;
  entry->function_address = address;

  FunctionInlineReplaceRouting *route = new FunctionInlineReplaceRouting(entry, replace_call);
  route->Prepare();
  route->DispatchRouting();

  // abort if relocation/trampoline generation failed
  if (!route->IsReady()) {
    ERROR_LOG("failed to prepare hook for %p", address);
    route->Destroy();
    delete route;
    delete entry;
    return RS_FAILED;
  }

  Interceptor::SharedInstance()->AddHookEntry(entry);

  // set origin call with relocated function
  if (origin_call)
    *origin_call = entry->relocated_origin_function;

  return RS_SUCCESS;
}

PUBLIC int DobbyCommit(void *address) {
  if (!address) {
    ERROR_LOG("function address is 0x0");
    return RS_FAILED;
  }

  // check if already hooked
  HookEntry *entry = Interceptor::SharedInstance()->FindHookEntry(address);
  if (!entry || !entry->route || entry->type != kFunctionInlineHook) {
    ERROR_LOG("function %p is not prepared/hooked.", address);
    return RS_FAILED;
  }
  FunctionInlineReplaceRouting *route = (FunctionInlineReplaceRouting *)entry->route;
  if (entry->is_committed) {
    // idempotent: already committed; succeed as a no-op
    return RS_SUCCESS;
  }

  // code patch & hijack original control flow entry
  route->Commit();
  return RS_SUCCESS;
}