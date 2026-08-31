#include "dobby_internal.h"

#include "InterceptRouting/InterceptRouting.h"
#include "InterceptRouting/Routing/DynamicBinaryInstrument/dynamic-binary-instrument.h"

PUBLIC int DobbyInstrument(void *address, DBICallTy handler) {
  if (!address) {
    ERROR_LOG("the function address is 0x0.\n");
    return RS_FAILED;
  }

  RAW_LOG(1, "\n\n");
  DLOG(0, "[DobbyInstrument] Initialize at %p", address);

  // check if we already instrument
  HookEntry *entry = Interceptor::SharedInstance()->FindHookEntry(address);
  if (entry) {
    // reject conflicts: this address is already hooked with a different mechanism
    if (entry->type != kDynamicBinaryInstrument) {
      ERROR_LOG("address %p is already hooked; destroy it before instrumenting", address);
      return RS_FAILED;
    }
    DynamicBinaryInstrumentRouting *route = (DynamicBinaryInstrumentRouting *)entry->route;
    if (route->handler == handler) {
      // idempotent: already instrumented with the same handler; succeed as a no-op
      return RS_SUCCESS;
    }
  }

  entry = new HookEntry();
  entry->id = Interceptor::SharedInstance()->GetHookEntryCount();
  entry->type = kDynamicBinaryInstrument;
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
    return RS_FAILED;
  }

  Interceptor::SharedInstance()->AddHookEntry(entry);
  route->Commit();

  return RS_SUCCESS;
}
