#include "dobby_internal.h"

#include "InterceptRouting/InterceptRouting.h"
#include "InterceptRouting/RoutingPlugin/RoutingPlugin.h"

using namespace zz;

void InterceptRouting::Prepare() {
  entry_->patch_address = entry_->target_address;
#if defined(TARGET_ARCH_ARM64)
  uint32_t first;
  memcpy(&first, entry_->target_address, sizeof(first));
  // Keep an explicit BTI landing pad at the public entry. PACIASP/PACIBSP are
  // implicit landing pads: replace them with BTI C and relocate their signing.
  if (first == 0xd503245fU || first == 0xd503249fU || first == 0xd50324dfU)
    entry_->patch_address = static_cast<uint8_t *>(entry_->target_address) + 4;
  else if (first == 0xd503233fU || first == 0xd503237fU)
    entry_->needs_bti_prefix = true;
#endif
}

// Generate relocated code
bool InterceptRouting::GenerateRelocatedCode(int tramp_size) {
  const size_t prefix =
      reinterpret_cast<uintptr_t>(entry_->patch_address) - reinterpret_cast<uintptr_t>(entry_->target_address);
  if (entry_->max_patch_bytes &&
      (prefix > entry_->max_patch_bytes || static_cast<size_t>(tramp_size) > entry_->max_patch_bytes - prefix)) {
    DobbySetError(DOBBY_ERROR_PATCH_LIMIT);
    return false;
  }
  // prologue verification: the target must be set, the trampoline size sane,
  // and (on fixed-width ISAs) a whole number of instructions.
  if (entry_->patch_address == NULL || tramp_size <= 0) {
    ERROR_LOG("invalid hook target %p / trampoline size %d", entry_->patch_address, tramp_size);
    DobbySetError(DOBBY_ERROR_INVALID_ARGUMENT);
    return false;
  }
#if defined(TARGET_ARCH_ARM64) || defined(TARGET_ARCH_ARM)
  if (tramp_size % 4 != 0) {
    ERROR_LOG("trampoline size %d is not a multiple of the instruction size", tramp_size);
    DobbySetError(DOBBY_ERROR_RELOCATION_FAILED);
    return false;
  }
#endif

#if defined(TARGET_ARCH_ARM64)
  if ((reinterpret_cast<uintptr_t>(entry_->patch_address) & 3) != 0) {
    DobbySetError(DOBBY_ERROR_INVALID_ARGUMENT);
    return false;
  }
  // A terminal instruction can mark the end of a tiny IL2CPP method or loader
  // thunk. Never consume the following function to make room for a long patch.
  // This is conservative: it is not a complete function-boundary decoder.
  for (int offset = 0; offset < tramp_size - 4; offset += 4) {
    uint32_t instruction;
    memcpy(&instruction, static_cast<uint8_t *>(entry_->patch_address) + offset, sizeof(instruction));
    if ((instruction & 0xfffffc1fU) == 0xd65f0000U ||               // RET
        (instruction & 0xfffffc1fU) == 0xd61f0000U ||               // BR
        (instruction & 0xfc000000U) == 0x14000000U ||               // B
        instruction == 0xd65f0bffU || instruction == 0xd65f0fffU) { // RETAA / RETAB
      ERROR_LOG("hook at %p would overwrite code after a terminal instruction", entry_->patch_address);
      DobbySetError(DOBBY_ERROR_FUNCTION_TOO_SHORT);
      return false;
    }
  }
#endif

  // generate original code
  AssemblyCodeChunk *origin = NULL;
  origin = AssemblyCodeBuilder::FinalizeFromAddress((addr_t)entry_->patch_address, tramp_size);
  origin_ = origin;

  // generate the relocated code
  AssemblyCodeChunk *relocated = NULL;
  relocated = AssemblyCodeBuilder::FinalizeFromAddress(0, 0);
  relocated_ = relocated;

  void *relocate_buffer = NULL;
  relocate_buffer = entry_->patch_address;

  GenRelocateCodeAndBranch(relocate_buffer, origin, relocated);
  if (relocated->raw_instruction_start() == 0) {
    if (DobbyGetLastError() == DOBBY_ERROR_NONE)
      DobbySetError(DOBBY_ERROR_RELOCATION_FAILED);
    return false;
  }

  // set the relocated instruction address
  entry_->relocated_origin_instructions = (void *)relocated->raw_instruction_start();
  DLOG(0, "[insn relocate] origin %p - %d", origin->raw_instruction_start(), origin->raw_instruction_size());
  DLOG(0, "[insn relocate] relocated %p - %d", relocated->raw_instruction_start(), relocated->raw_instruction_size());

  // Save the original prologue bytes for a later DobbyDestroy restore.
  // `origin` points at the un-patched target code, so this is a verbatim copy.
  // Guard against the fixed-size inline buffer being overflowed.
  size_t origin_size = origin_->raw_instruction_size();
  if (entry_->max_patch_bytes && origin_size > entry_->max_patch_bytes - prefix) {
    DobbySetError(DOBBY_ERROR_PATCH_LIMIT);
    return false;
  }
  if (origin_size > sizeof(entry_->origin_chunk_.chunk_buffer)) {
    ERROR_LOG("origin prologue size %zu exceeds inline buffer %zu", origin_size,
              sizeof(entry_->origin_chunk_.chunk_buffer));
    return false;
  }
  memcpy((void *)entry_->origin_chunk_.chunk_buffer, (void *)origin_->raw_instruction_start(), origin_size);
  entry_->origin_chunk_.chunk.re_init_region_range(origin_);

  is_ready_ = true;
  return true;
}

bool InterceptRouting::GenerateTrampolineBuffer(void *src, void *dst) {
  CodeBufferBase *trampoline_buffer = NULL;
  if (entry_->needs_bti_prefix)
    src = static_cast<uint8_t *>(src) + 4;
  auto Finish = [&]() {
    if (entry_->needs_bti_prefix && GetTrampolineBuffer()) {
      auto *buffer = new CodeBufferBase;
      buffer->Emit32(0xd503245fU);
      buffer->EmitBuffer(GetTrampolineBuffer()->getRawBuffer(), GetTrampolineBuffer()->getSize());
      delete GetTrampolineBuffer();
      SetTrampolineBuffer(buffer);
    }
    return GetTrampolineBuffer() != nullptr;
  };
#if defined(TARGET_ARCH_ARM64) && defined(DOBBY_HAS_NEAR_BRANCH)
  if (entry_->near_policy == DOBBY_NEAR_AUTO || entry_->near_policy == DOBBY_NEAR_REQUIRED ||
      (entry_->near_policy == -1 && RoutingPluginManager::automatic_near_branch)) {
    extern CodeBufferBase *GenerateNearTrampolineBuffer(InterceptRouting *, addr_t, addr_t);
    trampoline_buffer =
        GenerateNearTrampolineBuffer(this, reinterpret_cast<addr_t>(src), reinterpret_cast<addr_t>(dst));
    if (trampoline_buffer) {
      SetTrampolineBuffer(trampoline_buffer);
      return Finish();
    }
  }
#endif
  if (entry_->near_policy == DOBBY_NEAR_REQUIRED) {
    DobbySetError(DOBBY_ERROR_NEAR_ALLOCATION_FAILED);
    return false;
  }
  // if near branch trampoline plugin enabled
  if (entry_->near_policy == -1 && RoutingPluginManager::near_branch_trampoline) {
    RoutingPluginInterface *plugin = NULL;
    plugin = reinterpret_cast<RoutingPluginInterface *>(RoutingPluginManager::near_branch_trampoline);
    if (plugin->GenerateTrampolineBuffer(this, src, dst) == false) {
      DLOG(0, "Failed enable near branch trampoline plugin");
    }
  }

  if (this->GetTrampolineBuffer() == NULL) {
    trampoline_buffer = GenerateNormalTrampolineBuffer((addr_t)src, (addr_t)dst);
    this->SetTrampolineBuffer(trampoline_buffer);

    DLOG(0, "[trampoline] Generate trampoline buffer %p -> %p", src, dst);
  }
  return Finish();
}

// Active routing, will patch the origin insturctions, and forward to our custom routing.
// Patch the address with branch instr
bool InterceptRouting::Active() {
  if (!is_ready_ || !origin_ || !trampoline_buffer_)
    return false;
  void *patch_address = NULL;
  patch_address = (void *)origin_->raw_instruction_start();

  if (CodePatch(patch_address, (uint8_t *)trampoline_buffer_->getRawBuffer(), trampoline_buffer_->getSize()) !=
      kMemoryOperationSuccess)
    return false;
  entry_->is_committed = true;
  DLOG(0, "[intercept routing] Active patch %p", patch_address);
  return true;
}

bool InterceptRouting::Commit() {
  return this->Active();
}

void InterceptRouting::Destroy() {
  delete trampoline_;
  trampoline_ = nullptr;
  if (trampoline_buffer_) {
    delete trampoline_buffer_;
    trampoline_buffer_ = NULL;
  }
  if (origin_) {
    delete origin_;
    origin_ = NULL;
  }
  if (relocated_) {
    delete relocated_;
    relocated_ = NULL;
  }
}

#if 0
int InterceptRouting::PredefinedTrampolineSize() {
#if __arm64__
  return 12;
#elif __arm__
  return 8;
#endif
}
#endif

HookEntry *InterceptRouting::GetHookEntry() {
  return entry_;
};
