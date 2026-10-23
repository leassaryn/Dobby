#pragma once
#include "InstructionRelocation/InstructionPlan.h"
#include "InstructionRelocation/x86/x86_insn_decode/x86_insn_decode.h"
namespace dobby {
inline Instruction ClassifyX86(const x86_insn_decode_t &decoded, const void *bytes, uintptr_t pc) {
  Instruction i;
  i.pc = pc;
  i.size = decoded.length;
  const auto op = decoded.primary_opcode;
  if (!i.size || decoded.unsupported || (!decoded.two_byte_opcode && op >= 0xe0 && op <= 0xe3)) {
    i.kind = InstructionKind::Unsupported;
    return i;
  }
  const bool short_cond = !decoded.two_byte_opcode && op >= 0x70 && op <= 0x7f;
  const bool near_cond = decoded.two_byte_opcode && op >= 0x80 && op <= 0x8f;
  const bool short_jump = !decoded.two_byte_opcode && op == 0xeb;
  const bool near_jump = !decoded.two_byte_opcode && (op == 0xe8 || op == 0xe9);
  if (short_cond || near_cond || short_jump || near_jump) {
    if ((decoded.prefix & INSN_PREFIX_OPERAND_SIZE) && (near_cond || near_jump)) {
      i.kind = InstructionKind::Unsupported;
      return i;
    }
    i.kind = short_cond || near_cond ? InstructionKind::Conditional
             : op == 0xe8            ? InstructionKind::Call
                                     : InstructionKind::Branch;
    i.target =
        pc + i.size +
        (short_cond || short_jump ? static_cast<int8_t>(decoded.immediate) : static_cast<int32_t>(decoded.immediate));
  } else if (decoded.flags & X86_INSN_DECODE_FLAG_IP_RELATIVE) {
    if ((decoded.prefix & INSN_PREFIX_ADDRESS_SIZE) || !decoded.displacement_offset ||
        decoded.displacement_offset + 4 > decoded.length) {
      i.kind = InstructionKind::Unsupported;
      return i;
    }
    int32_t displacement;
    memcpy(&displacement, static_cast<const uint8_t *>(bytes) + decoded.displacement_offset, 4);
    i.kind = InstructionKind::Address;
    i.target = pc + i.size + displacement;
  } else if (!decoded.two_byte_opcode && (op == 0xc3 || op == 0xc2))
    i.kind = InstructionKind::Terminal;
  return i;
}
} // namespace dobby
