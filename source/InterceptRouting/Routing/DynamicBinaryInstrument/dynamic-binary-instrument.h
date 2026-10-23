#ifndef DYNAMIC_BINARY_INSTRUMENT_H
#define DYNAMIC_BINARY_INSTRUMENT_H

#include "dobby_internal.h"

#include "Interceptor.h"

#include "InterceptRouting/InterceptRouting.h"

#include "TrampolineBridge/ClosureTrampolineBridge/AssemblyClosureTrampoline.h"

#define X64InterceptRouting InterceptRouting
#define ARM64InterceptRouting InterceptRouting

class DynamicBinaryInstrumentRouting : public X64InterceptRouting {
public:
  DynamicBinaryInstrumentRouting(HookEntry *entry, void *handler) : X64InterceptRouting(entry) {
    this->handler = handler;
  }

  void DispatchRouting() override;
  void Destroy() override;
  void *GetUserReplacement() override {
    return handler;
  }

public:
  void *handler;

private:
  virtual void BuildDynamicBinaryInstrumentRouting();

private:
  ClosureTrampolineEntry *closure_trampoline_ = nullptr;

  ;
};

#endif
