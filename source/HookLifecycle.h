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

// Called only while the hook-operation mutex is held. No automatic thread suspension.
class PatchScope {
public:
  PatchScope(void *address, size_t size, DobbyPatchOperation operation);
  ~PatchScope();
  bool ready() const {
    return ready_;
  }
  void Applied() {
    applied_ = true;
  }

private:
  void *address_;
  size_t size_;
  DobbyPatchOperation operation_;
  DobbyPatchCoordinator coordinator_{};
  bool ready_ = false;
  bool entered_ = false;
  bool applied_ = false;
};
