#include "platform_macro.h"
#if defined(TARGET_ARCH_IA32)

#include "dobby_internal.h"

#include "core/modules/assembler/assembler-ia32.h"

#include "TrampolineBridge/ClosureTrampolineBridge/AssemblyClosureTrampoline.h"

using namespace zz;
using namespace zz::x86;

ClosureTrampolineEntry *ClosureTrampoline::CreateClosureTrampoline(void *carry_data, void *carry_handler) {
  ClosureTrampolineEntry *entry = nullptr;
  entry = new ClosureTrampolineEntry;
  void *bridge = get_closure_bridge();
  if (!bridge) {
    delete entry;
    return nullptr;
  }

  AssemblyCodeChunk *cchunk = MemoryArena::AllocateCodeChunk(32);
  if (cchunk == nullptr) {
    delete entry;
    return NULL;
  }

#define _ turbo_assembler_.
#define __ turbo_assembler_.GetCodeBuffer()->
  TurboAssembler turbo_assembler_(cchunk->address);

  int32_t offset = (int32_t)bridge - ((int32_t)cchunk->address + 18);

  _ sub(esp, Immediate(4, 32));
  _ mov(Address(esp, 4 * 0), Immediate((int32_t)entry, 32));
  _ jmp(Immediate(offset, 32));

  entry->address = (void *)cchunk->raw_instruction_start();
  entry->size = cchunk->raw_instruction_size();
  entry->carry_data = carry_data;
  entry->carry_handler = carry_handler;

  CodeBufferBase *buffer = reinterpret_cast<CodeBufferBase *>(turbo_assembler_.GetCodeBuffer());
  const bool patched =
      CodePatch(cchunk->address, (uint8_t *)buffer->getRawBuffer(), buffer->getSize()) == kMemoryOperationSuccess;
  delete cchunk;
  if (!patched) {
    delete entry;
    return nullptr;
  }

  return entry;
}

#endif