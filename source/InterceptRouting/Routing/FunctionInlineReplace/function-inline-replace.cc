#include "InterceptRouting/Routing/FunctionInlineReplace/function-inline-replace.h"

#include "dobby_internal.h"

void FunctionInlineReplaceRouting::DispatchRouting() {
  BuildReplaceRouting();

  // guard against a failed trampoline generation (e.g. no near code cave)
  if (trampoline_buffer_ == NULL || trampoline_buffer_->getSize() == 0) {
    ERROR_LOG("failed to generate trampoline buffer for %p", entry_->target_address);
    return;
  }

  // generate relocated code which size == trampoline size
  GenerateRelocatedCode(trampoline_buffer_->getSize());
}

void FunctionInlineReplaceRouting::BuildReplaceRouting() {
  this->SetTrampolineTarget(this->replace_call);
  DLOG(0, "[inline] Set trampoline target => %p", GetTrampolineTarget());

  // generate trampoline buffer, run before `GenerateRelocatedCode`
  GenerateTrampolineBuffer(entry_->patch_address, GetTrampolineTarget());
}

#if 0
void *FunctionInlineReplaceRouting::GetTrampolineTarget() {
  return this->replace_call;
}
#endif
