#pragma once
#include "dobby.h"
#include <cstdint>
#include <cstring>
#include <bit>

// Shared semantic vocabulary. Unknown register effects are explicit, never inferred as empty.
namespace dobby {
enum class InstructionKind { Copy, Branch, Call, Conditional, Literal, Address, Terminal, Unsupported, ItBlock };
struct Instruction {
  uintptr_t pc = 0;
  uintptr_t target = 0;
  uint64_t reads = 0, writes = 0;
  uint32_t bits = 0;
  uint8_t size = 0, literal_size = 0;
  InstructionKind kind = InstructionKind::Copy;
  bool effects_known = false;
};
inline int64_t Signed(uint64_t value, unsigned width) {
  const uint64_t sign = uint64_t{1} << (width - 1);
  return static_cast<int64_t>(value ^ sign) - static_cast<int64_t>(sign);
}
inline Instruction DecodeArm64(uint32_t word, uintptr_t pc) {
  Instruction i;
  i.pc = pc;
  i.bits = word;
  i.size = 4;
  const unsigned rd = word & 31;
  if ((word & 0x7c000000U) == 0x14000000U) {
    i.kind = word & 0x80000000U ? InstructionKind::Call : InstructionKind::Branch;
    i.target = pc + Signed(word & 0x03ffffffU, 26) * 4;
    i.effects_known = true;
    i.writes = i.kind == InstructionKind::Call ? uint64_t{1} << 30 : 0;
  } else if ((word & 0xff000010U) == 0x54000000U || (word & 0x7e000000U) == 0x34000000U ||
             (word & 0x7e000000U) == 0x36000000U) {
    i.kind = InstructionKind::Conditional;
    const bool test = (word & 0x7e000000U) == 0x36000000U;
    i.target = pc + Signed((word >> 5) & (test ? 0x3fff : 0x7ffff), test ? 14 : 19) * 4;
    i.effects_known = true;
    if ((word & 0xff000010U) != 0x54000000U)
      i.reads = uint64_t{1} << rd;
  } else if ((word & 0x3b000000U) == 0x18000000U) {
    i.kind = InstructionKind::Literal;
    i.target = pc + Signed((word >> 5) & 0x7ffff, 19) * 4;
    const unsigned opc = word >> 30;
    i.literal_size = word & (1U << 26) ? (opc == 3 ? 0 : 4U << opc) : (opc == 1 ? 8 : opc == 3 ? 0 : 4);
    if ((word & (1U << 26)) && opc == 3)
      i.kind = InstructionKind::Unsupported;
    i.effects_known = true;
    if (!(word & (1U << 26)) && opc != 3)
      i.writes = uint64_t{1} << rd;
  } else if ((word & 0x1f000000U) == 0x10000000U) {
    i.kind = InstructionKind::Address;
    const int64_t imm = Signed(((word >> 5) & 0x7ffff) * 4 + ((word >> 29) & 3), 21);
    i.target = word & 0x80000000U ? (pc & ~uintptr_t{4095}) + imm * 4096 : pc + imm;
    i.effects_known = true;
    i.writes = uint64_t{1} << rd;
  } else if ((word & 0xfffffc1fU) == 0xd65f0000U || (word & 0xfffffc1fU) == 0xd61f0000U || word == 0xd65f0bffU ||
             word == 0xd65f0fffU) {
    i.kind = InstructionKind::Terminal;
  } else if ((word & 0x1f000000U) == 0x11000000U) {
    i.effects_known = true;
    i.reads = uint64_t{1} << ((word >> 5) & 31);
    i.writes = uint64_t{1} << rd;
  }
  return i;
}
inline Instruction DecodeArm(uint32_t word, uintptr_t pc) {
  Instruction i;
  i.pc = pc;
  i.bits = word;
  i.size = 4;
  if ((word & 0x0e000000U) == 0x0a000000U) {
    i.kind = word & 0x01000000 ? InstructionKind::Call : InstructionKind::Branch;
    i.target = pc + 8 + Signed(word & 0xffffff, 24) * 4;
    if ((word >> 28) != 14 && i.kind == InstructionKind::Branch)
      i.kind = InstructionKind::Conditional;
  } else if ((word & 0x0f3f0000U) == 0x051f0000U) {
    i.kind = InstructionKind::Literal;
    i.literal_size = word & (1U << 22) ? 1 : 4;
    i.target = pc + 8 + ((word & 0x00800000) ? int64_t(word & 4095) : -int64_t(word & 4095));
    i.effects_known = true;
    i.writes = uint64_t{1} << ((word >> 12) & 15);
  } else if ((word & 0x0fff0000U) == 0x028f0000U || (word & 0x0fff0000U) == 0x024f0000U) {
    const uint32_t immediate = std::rotr(word & 255, int((word >> 8) & 15) * 2);
    i.kind = InstructionKind::Address;
    i.target = (word & 0x00800000U) ? pc + 8 + immediate : pc + 8 - immediate;
    i.effects_known = true;
    i.writes = uint64_t{1} << ((word >> 12) & 15);
  } else if ((word & 0x0ffffff0U) == 0x012fff10U)
    i.kind = InstructionKind::Terminal;
  if ((i.kind == InstructionKind::Literal || i.kind == InstructionKind::Address) && (word >> 28) != 14)
    i.kind = InstructionKind::Unsupported;
  if ((word & 0xfe000000U) == 0xfa000000U)
    i.kind = InstructionKind::Unsupported;
  return i;
}
// Position-independent narrow encodings that can retain their original ITSTATE.
// Do not accept PC-relative operands, branches, system instructions or wide encodings.
inline bool ThumbItCopyable(uint16_t word) {
  return (word & 0xc000) == 0 ||      // Immediate shifts, low-register ADD/SUB, MOV/CMP/ADD/SUB immediate.
         (word & 0xfc00) == 0x4000 || // Low-register data processing (including flag-setting comparisons).
         (word & 0xf000) == 0x9000 || // SP-relative word loads/stores.
         (word & 0xf800) == 0xa800;   // ADD low register, SP, immediate.
}
// A supported IT block is indivisible. Return zero if any member needs rewriting.
inline size_t ThumbItSize(const void *bytes, size_t available) {
  if (available < 2)
    return 0;
  uint16_t it;
  memcpy(&it, bytes, 2);
  if ((it & 0xff00) != 0xbf00 || !(it & 15) || ((it >> 4) & 15) >= 14)
    return 0;
  unsigned state = it & 255;
  size_t size = 2;
  do {
    if (size + 2 > available)
      return 0;
    uint16_t member;
    memcpy(&member, static_cast<const uint8_t *>(bytes) + size, 2);
    if (!ThumbItCopyable(member))
      return 0;
    size += 2;
    state = (state & 7) ? ((state & 0xe0) | ((state << 1) & 0x1f)) : 0;
  } while (state);
  return size;
}
inline bool ThumbWide(uint16_t word) {
  return (word & 0xf800) >= 0xe800;
}
inline Instruction DecodeThumb(uint32_t word, uintptr_t pc) {
  Instruction i;
  i.pc = pc;
  i.bits = word;
  i.size = ThumbWide(word) ? 4 : 2;
  const uint16_t low = word;
  if (i.size == 2) {
    if (((low & 0xff00) == 0xbf00 && (low & 15)) || (low & 0xff00) == 0xde00 || (low & 0xff00) == 0xdf00)
      i.kind = InstructionKind::Unsupported;
    else if ((low & 0xf800) == 0xe000) {
      i.kind = InstructionKind::Branch;
      i.target = pc + 4 + Signed(low & 0x7ff, 11) * 2;
    } else if ((low & 0xf000) == 0xd000) {
      i.kind = InstructionKind::Conditional;
      i.target = pc + 4 + Signed(low & 255, 8) * 2;
    } else if ((low & 0xf800) == 0x4800) {
      i.kind = InstructionKind::Literal;
      i.target = ((pc + 4) & ~uintptr_t{3}) + (low & 255) * 4;
      i.literal_size = 4;
      i.effects_known = true;
      i.writes = uint64_t{1} << ((low >> 8) & 7);
    } else if ((low & 0xf800) == 0xa000) {
      i.kind = InstructionKind::Address;
      i.target = ((pc + 4) & ~uintptr_t{3}) + (low & 255) * 4;
      i.effects_known = true;
      i.writes = uint64_t{1} << ((low >> 8) & 7);
    } else if ((low & 0xff87) == 0x4700 || (low & 0xff00) == 0xbd00)
      i.kind = InstructionKind::Terminal;
  }
  return i;
}
} // namespace dobby
