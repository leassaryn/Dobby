#include "dobby_internal.h"

#include "InterceptRouting/InterceptRouting.h"
#include "InterceptRouting/Routing/DynamicBinaryInstrument/dynamic-binary-instrument.h"

PUBLIC int DobbyInstrument(void *address, DBICallTy handler) {
  HookOperationGuard operation;
  DobbySetError(DOBBY_ERROR_NONE);
  if (!address || !handler) {
    ERROR_LOG("null instruction address or handler");
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  }

#if defined(TARGET_ARCH_ARM64)
  if ((reinterpret_cast<uintptr_t>(address) & 3) != 0)
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
#endif

  RAW_LOG(1, "\n\n");
  DLOG(0, "[DobbyInstrument] Initialize at %p", address);

  // check if we already instrument
  HookEntry *entry = Interceptor::SharedInstance()->FindHookEntry(address);
  if (entry) {
    // reject conflicts: this address is already hooked with a different mechanism
    if (entry->type != kDynamicBinaryInstrument) {
      ERROR_LOG("address %p is already hooked; destroy it before instrumenting", address);
      return DobbyFailure(DOBBY_ERROR_CONFLICT);
    }
    DynamicBinaryInstrumentRouting *route = (DynamicBinaryInstrumentRouting *)entry->route;
    if (route->handler == handler) {
      // idempotent: already instrumented with the same handler; succeed as a no-op
      return DobbySuccess();
    }
    ERROR_LOG("address %p already has a different handler; destroy it before instrumenting", address);
    return DobbyFailure(DOBBY_ERROR_CONFLICT);
  }

  entry = new HookEntry();
  entry->id = Interceptor::SharedInstance()->GetHookEntryCount();
  entry->type = kDynamicBinaryInstrument;
  entry->near_policy = -1;
  entry->instruction_address = address;

  DynamicBinaryInstrumentRouting *route = new DynamicBinaryInstrumentRouting(entry, (void *)handler);
  route->Prepare();
  route->DispatchRouting();

  // abort if relocation/trampoline generation failed
  if (!route->IsReady()) {
    ERROR_LOG("failed to prepare instrument for %p", address);
    route->Destroy();
    delete route;
    delete entry;
    if (DobbyGetLastError() == DOBBY_ERROR_NONE)
      DobbySetError(DOBBY_ERROR_RELOCATION_FAILED);
    return RS_FAILED;
  }

  if (!route->Commit()) {
    route->Destroy();
    delete route;
    delete entry;
    if (DobbyGetLastError() == DOBBY_ERROR_NONE)
      DobbySetError(DOBBY_ERROR_RELOCATION_FAILED);
    return RS_FAILED;
  }
  Interceptor::SharedInstance()->AddHookEntry(entry);

  return RS_SUCCESS;
}
