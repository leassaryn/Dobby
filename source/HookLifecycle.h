#pragma once
#include "dobby_internal.h"

void RetireHook(HookEntry *entry);
class InstrumentCallbackScope {
public:
  InstrumentCallbackScope();
  ~InstrumentCallbackScope();
};

// Suppress user loader notifications for internal module reference management.
extern thread_local unsigned dobby_internal_loader_depth;
class InternalLoaderScope {
public:
  InternalLoaderScope() {
    ++dobby_internal_loader_depth;
  }
  ~InternalLoaderScope() {
    --dobby_internal_loader_depth;
  }
};
