#include "platform_macro.h"
#if defined(TARGET_ARCH_ARM64)

#include "dobby_internal.h"

#include "core/modules/assembler/assembler-arm64.h"
#include "core/modules/codegen/codegen-arm64.h"

#include "InstructionRelocation/arm64/ARM64InstructionRelocation.h"

#include "MemoryAllocator/NearMemoryArena.h"
#include "InterceptRouting/RoutingPlugin/RoutingPlugin.h"

using namespace zz::arm64;

CodeBufferBase *GenerateNormalTrampolineBuffer(addr_t from, addr_t to) {
  TurboAssembler turbo_assembler_((void *)from);
#define _ turbo_assembler_.

  const int64_t page_distance = static_cast<int64_t>(to >> 12) - static_cast<int64_t>(from >> 12);
  const int64_t branch_offset = static_cast<int64_t>(to) - static_cast<int64_t>(from);
  if ((branch_offset & 3) == 0 && branch_offset >= -(int64_t{1} << 27) && branch_offset < (int64_t{1} << 27)) {
    _ b(branch_offset);
  } else if (page_distance >= -(int64_t{1} << 20) && page_distance < (int64_t{1} << 20)) {
    // adrp, add, br
    _ AdrpAdd(TMP_REG_0, from, to);
    _ br(TMP_REG_0);
    DLOG(0, "Trampoline use [Adrp, Add, Br] combine");
  } else {
    // ldr, br, branch-address
    CodeGen codegen(&turbo_assembler_);
    codegen.LiteralLdrBranch((uint64_t)to);
    DLOG(0, "Trampoline use [Ldr, Br, Label] combine");
  }

  CodeBufferBase *result = NULL;
  result = turbo_assembler_.GetCodeBuffer()->Copy();
  return result;
}

#ifdef DOBBY_HAS_NEAR_BRANCH
CodeBufferBase *GenerateNearTrampolineBuffer(InterceptRouting *routing, addr_t src, addr_t dst) {
  constexpr int64_t range = int64_t{1} << 27;
  const int64_t distance = static_cast<int64_t>(dst) - static_cast<int64_t>(src);
  TurboAssembler patch(reinterpret_cast<void *>(src));
  if ((distance & 3) == 0 && distance >= -range && distance < range) {
    patch.b(distance);
  } else {
    TurboAssembler relay(nullptr);
    relay.Mov(TMP_REG_0, dst);
    relay.br(TMP_REG_0);
    auto *buffer = relay.GetCodeBuffer();
    auto *chunk = NearMemoryArena::AllocateCodeChunk(src, range, buffer->getSize());
    if (!chunk) {
      DobbySetError(DOBBY_ERROR_NEAR_ALLOCATION_FAILED);
      return nullptr;
    }
    const int64_t offset = static_cast<int64_t>(chunk->raw_instruction_start()) - static_cast<int64_t>(src);
    if ((offset & 3) != 0 || offset < -range || offset >= range) {
      delete chunk;
      DobbySetError(DOBBY_ERROR_NEAR_ALLOCATION_FAILED);
      return nullptr;
    }
    if (CodePatch(chunk->address, static_cast<uint8_t *>(buffer->getRawBuffer()), buffer->getSize()) !=
        kMemoryOperationSuccess) {
      delete chunk;
      return nullptr;
    }
    routing->SetRelay(chunk);
    patch.b(offset);
  }
  return patch.GetCodeBuffer()->Copy();
}
#endif
#endif
