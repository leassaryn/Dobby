#ifndef INTERCEPT_ROUTING_H
#define INTERCEPT_ROUTING_H

#include "Interceptor.h"
#include "MemoryAllocator/AssemblyCodeBuilder.h"
#include "InstructionRelocation/InstructionRelocation.h"
#include "TrampolineBridge/Trampoline/Trampoline.h"

class InterceptRouting {
public:
  InterceptRouting(HookEntry *entry) : entry_(entry) {
    entry->route = this;

    origin_ = NULL;
    relocated_ = NULL;
    trampoline_ = NULL;
    trampoline_buffer_ = NULL;
    trampoline_target_ = NULL;
    is_ready_ = false;
  }

  virtual ~InterceptRouting() {
  }

  virtual void DispatchRouting() = 0;

  virtual void Prepare();

  virtual void *GetUserReplacement() {
    return trampoline_target_;
  }
  void *GetRelayAddress() const {
    return trampoline_ ? trampoline_->address : nullptr;
  }
  void SetRelay(AssemblyCodeChunk *chunk) {
    trampoline_ = chunk;
  }

  virtual bool Active();

  virtual void Destroy();

  bool Commit();

  HookEntry *GetHookEntry();

  void SetTrampolineBuffer(CodeBufferBase *buffer) {
    trampoline_buffer_ = buffer;
  }

  CodeBufferBase *GetTrampolineBuffer() {
    return trampoline_buffer_;
  }

  void SetTrampolineTarget(void *address) {
    trampoline_target_ = address;
  }

  void *GetTrampolineTarget() {
    return trampoline_target_;
  }

  // True once relocation + trampoline buffers were built successfully and the
  // hook is safe to commit. Checked by the public API entrypoints.
  bool IsReady() {
    return is_ready_;
  }

protected:
  void SetReady(bool ready) {
    is_ready_ = ready;
  }

  bool GenerateRelocatedCode(int tramp_size);

  bool GenerateTrampolineBuffer(void *src, void *dst);

protected:
  HookEntry *entry_;

  AssemblyCodeChunk *origin_;

  AssemblyCodeChunk *relocated_;

  AssemblyCodeChunk *trampoline_;

  // trampoline buffer before active
  CodeBufferBase *trampoline_buffer_;

  void *trampoline_target_;

  // set true by GenerateRelocatedCode once relocation succeeds
  bool is_ready_;
};
#endif
