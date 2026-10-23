#include "platform_macro.h"
#if defined(TARGET_ARCH_X64)

#include "InstructionRelocation/x64/X64InstructionRelocation.h"

#include <string.h>
#include "InstructionRelocation/x86/RelocationFixups.h"

#include "dobby_internal.h"
#include "MemoryAllocator/NearMemoryArena.h"

#include "InstructionRelocation/x86/x86_insn_decode/x86_insn_decode.h"

#include "core/arch/x64/registers-x64.h"
#include "core/modules/assembler/assembler-x64.h"
#include "core/modules/codegen/codegen-x64.h"

using namespace zz::x64;

static int GenRelocateCodeFixed(void *buffer, AssemblyCodeChunk *origin, AssemblyCodeChunk *relocated) {
  TurboAssembler turbo_assembler_(0);
  // Set fixed executable code chunk address
  turbo_assembler_.SetRealizedAddress((void *)relocated->raw_instruction_start());
#define _ turbo_assembler_.
#define __ turbo_assembler_.GetCodeBuffer()->

  addr64_t curr_orig_ip = origin->raw_instruction_start();
  addr64_t curr_relo_ip = relocated->raw_instruction_start();

  addr_t buffer_cursor = (addr_t)buffer;

  x86_options_t conf = {0};
  conf.mode = 64;

  int predefined_relocate_size = origin->raw_instruction_size();
  X86RelocationFixups fixups;

  while ((buffer_cursor < ((addr_t)buffer + predefined_relocate_size))) {
    int last_relo_offset = turbo_assembler_.GetCodeBuffer()->getSize();

    x86_insn_decode_t insn = {0};
    memset(&insn, 0, sizeof(insn));
    const size_t available = fixups.Readable(buffer_cursor);
    if (!available) {
      DobbySetError(DOBBY_ERROR_RELOCATION_FAILED);
      return RT_FAILED;
    }
    x86_insn_decode_bounded(&insn, reinterpret_cast<uint8_t *>(buffer_cursor), available, &conf);
    if (!insn.length || insn.unsupported) {
      DobbySetError(DOBBY_ERROR_UNSUPPORTED_INSTRUCTION);
      return RT_FAILED;
    }
    fixups.offsets[curr_orig_ip] = turbo_assembler_.ip_offset();
    const auto opcode = insn.primary_opcode;
    const bool short_cond = !insn.two_byte_opcode && opcode >= 0x70 && opcode <= 0x7f;
    const bool near_cond = insn.two_byte_opcode && opcode >= 0x80 && opcode <= 0x8f;
    const bool short_jump = !insn.two_byte_opcode && opcode == 0xeb;
    const bool near_jump = !insn.two_byte_opcode && (opcode == 0xe8 || opcode == 0xe9);
    if (short_cond || near_cond || short_jump || near_jump) {
      if ((insn.prefix & INSN_PREFIX_OPERAND_SIZE) && (near_cond || near_jump)) {
        DobbySetError(DOBBY_ERROR_UNSUPPORTED_INSTRUCTION);
        return RT_FAILED;
      }
      const int64_t displacement =
          short_cond || short_jump ? static_cast<int8_t>(insn.immediate) : static_cast<int32_t>(insn.immediate);
      const uint64_t target = curr_orig_ip + insn.length + displacement;
      if (short_cond || near_cond) {
        __ Emit8(0x0f);
        __ Emit8(0x80 | (opcode & 15));
      } else {
        __ Emit8(opcode == 0xe8 ? 0xe8 : 0xe9);
      }
      const size_t position = turbo_assembler_.ip_offset();
      __ Emit32(0);
      fixups.entries.push_back({position, position + 4, target, true});
    } else if (insn.flags & X86_INSN_DECODE_FLAG_IP_RELATIVE) {
      if ((insn.prefix & INSN_PREFIX_ADDRESS_SIZE) || !insn.displacement_offset ||
          insn.displacement_offset + 4 > insn.length) {
        DobbySetError(DOBBY_ERROR_UNSUPPORTED_INSTRUCTION);
        return RT_FAILED;
      }
      int32_t displacement;
      memcpy(&displacement, reinterpret_cast<void *>(buffer_cursor + insn.displacement_offset), sizeof(displacement));
      const size_t position = turbo_assembler_.ip_offset();
      __ EmitBuffer(reinterpret_cast<void *>(buffer_cursor), insn.length);
      fixups.entries.push_back({position + insn.displacement_offset, position + insn.length,
                                curr_orig_ip + insn.length + displacement, false});
    } else if (!insn.two_byte_opcode && opcode >= 0xe0 && opcode <= 0xe3) {
      DobbySetError(DOBBY_ERROR_UNSUPPORTED_INSTRUCTION);
      return RT_FAILED;
    } else {
      __ EmitBuffer(reinterpret_cast<void *>(buffer_cursor), insn.length);
    }

    // go next
    curr_orig_ip += insn.length;
    buffer_cursor += insn.length;

#if 0
    {
      // 1 orignal instrution => ? relocated instruction
      int relo_offset = turbo_assembler_.GetCodeBuffer()->getSize();
      int relo_len    = relo_offset - last_relo_offset;
      curr_relo_ip += relo_len;
    }
#endif
    curr_relo_ip = relocated->raw_instruction_start() + turbo_assembler_.ip_offset();
  }

  if (!fixups.Resolve(turbo_assembler_.GetCodeBuffer(), origin->raw_instruction_start(), curr_orig_ip,
                      relocated->raw_instruction_start(), true))
    return RT_FAILED;

  // jmp to the origin rest instructions
  CodeGen codegen(&turbo_assembler_);
  // TODO: 6 == jmp [RIP + disp32] instruction size
  addr64_t stub_addr = curr_relo_ip + 6;
  codegen.JmpNearIndirect(stub_addr);
  turbo_assembler_.GetCodeBuffer()->Emit64(curr_orig_ip);

  // update origin
  int new_origin_len = curr_orig_ip - origin->raw_instruction_start();
  origin->re_init_region_range(origin->raw_instruction_start(), new_origin_len);

  int relo_len = turbo_assembler_.GetCodeBuffer()->getSize();
  if (relo_len > relocated->raw_instruction_size()) {
    DLOG(0, "pre-alloc code chunk not enough");
    return RT_FAILED;
  }

  // Generate executable code
  {
    AssemblyCodeChunk *code = NULL;
    code = AssemblyCodeBuilder::FinalizeFromTurboAssembler(&turbo_assembler_);
    if (!code)
      return RT_FAILED;
    delete code;
  }

  return RT_SUCCESS;
}

void GenRelocateCodeAndBranch(void *buffer, AssemblyCodeChunk *origin, AssemblyCodeChunk *relocated) {
  // pre-alloc code chunk
  AssemblyCodeChunk *cchunk = NULL;

  int relo_code_chunk_size = 32;
  const int chunk_size_step = 16;

x64_try_again:
  if (relocated->raw_instruction_start() == 0) {
    cchunk = NearMemoryArena::AllocateCodeChunk(origin->raw_instruction_start(), (size_t)(1 * 1024 * 1024 * 1024),
                                                relo_code_chunk_size);
    if (cchunk == nullptr) {
      return;
    }
    relocated->re_init_region_range((addr_t)cchunk->address, (int)cchunk->length);
  }

  int ret = GenRelocateCodeFixed(buffer, origin, relocated);
  if (ret != RT_SUCCESS) {
    // free the cchunk
    delete cchunk;

    if (DobbyGetLastError() != DOBBY_ERROR_NONE) {
      relocated->re_init_region_range(0, 0);
      return;
    }
    relo_code_chunk_size += chunk_size_step;
    relocated->re_init_region_range(0, 0);

    goto x64_try_again;
  }
  delete cchunk;
}

#endif
