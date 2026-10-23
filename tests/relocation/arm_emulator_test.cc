#include "dobby_internal.h"
#include "InstructionRelocation/InstructionRelocation.h"
#include <capstone/capstone.h>
#include <unicorn/unicorn.h>
#include <array>
#include <vector>
#include <stdexcept>
#include <cstdio>
#include <sys/mman.h>
namespace {
void Check(bool v, const char *s) {
  if (!v)
    throw std::runtime_error(s);
}
void U(uc_err e) {
  Check(e == UC_ERR_OK, uc_strerror(e));
}
struct Mapping {
  void *p;
  explicit Mapping(uintptr_t hint)
      : p(mmap(reinterpret_cast<void *>(hint), 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) {
    Check(p != MAP_FAILED && reinterpret_cast<uintptr_t>(p) < UINT32_MAX - 4096, "low map");
  }
  ~Mapping() {
    munmap(p, 4096);
  }
};
struct TraceState {
  csh decoder{};
  bool valid = true;
};
void Trace(uc_engine *uc, uint64_t pc, uint32_t size, void *data) {
  auto &d = *static_cast<TraceState *>(data);
  uint8_t bytes[4];
  cs_insn *insn = nullptr;
  if (size > 4 || uc_mem_read(uc, pc, bytes, size) != UC_ERR_OK) {
    d.valid = false;
    uc_emu_stop(uc);
    return;
  }
  const auto count = cs_disasm(d.decoder, bytes, size, pc, 1, &insn);
  if (count != 1 || insn->size != size) {
    d.valid = false;
    uc_emu_stop(uc);
  }
  cs_free(insn, count);
}
struct State {
  std::array<uint32_t, 16> regs{};
  std::array<uint8_t, 4096> stack{};
  bool operator==(const State &) const = default;
};
State Run(Mapping &source, Mapping &destination, uintptr_t output, size_t displaced, bool thumb, bool relocated,
          unsigned seed) {
  uc_engine *uc;
  U(uc_open(UC_ARCH_ARM, thumb ? UC_MODE_THUMB : UC_MODE_ARM, &uc));
  struct Close {
    uc_engine *u;
    ~Close() {
      uc_close(u);
    }
  } close{uc};
  auto start = reinterpret_cast<uintptr_t>(source.p), dest = reinterpret_cast<uintptr_t>(destination.p);
  U(uc_mem_map(uc, start, 4096, UC_PROT_ALL));
  U(uc_mem_write(uc, start, source.p, 4096));
  U(uc_mem_map(uc, dest, 4096, UC_PROT_ALL));
  U(uc_mem_write(uc, dest, destination.p, 4096));
  U(uc_mem_map(uc, 0x3000000, 4096, UC_PROT_ALL));
  U(uc_mem_map(uc, 0x4000000, 4096, UC_PROT_ALL));
  if (relocated) {
    std::vector<uint8_t> poison(displaced, 0xff);
    U(uc_mem_write(uc, start, poison.data(), poison.size()));
  }
  for (int n = 0; n < 13; ++n) {
    uint32_t v = seed + n;
    U(uc_reg_write(uc, UC_ARM_REG_R0 + n, &v));
  }
  uint32_t sp = 0x3000800, lr = 0x4000000 | thumb;
  U(uc_reg_write(uc, UC_ARM_REG_SP, &sp));
  U(uc_reg_write(uc, UC_ARM_REG_LR, &lr));
  TraceState trace;
  Check(cs_open(CS_ARCH_ARM, thumb ? CS_MODE_THUMB : CS_MODE_ARM, &trace.decoder) == CS_ERR_OK, "capstone");
  uc_hook hook;
  U(uc_hook_add(uc, &hook, UC_HOOK_CODE, reinterpret_cast<void *>(Trace), &trace, 1, 0));
  const auto result = uc_emu_start(uc, (relocated ? output : start) | thumb, 0x4000000, 1000000, 1000);
  cs_close(&trace.decoder);
  U(result);
  Check(trace.valid, "decode");
  uint32_t pc;
  U(uc_reg_read(uc, UC_ARM_REG_PC, &pc));
  Check(pc == 0x4000000, "stop PC");
  State state;
  for (int n = 0; n < 13; ++n)
    U(uc_reg_read(uc, UC_ARM_REG_R0 + n, &state.regs[n]));
  U(uc_reg_read(uc, UC_ARM_REG_SP, &state.regs[13]));
  U(uc_reg_read(uc, UC_ARM_REG_LR, &state.regs[14]));
  U(uc_reg_read(uc, UC_ARM_REG_CPSR, &state.regs[15]));
  U(uc_mem_read(uc, 0x3000000, state.stack.data(), 4096));
  return state;
}
} // namespace
int main() {
  try {
    Mapping source(0x1000000), dest(0x2000000);
    PageChunk page{};
    page.page.init_region_range(reinterpret_cast<uintptr_t>(dest.p), 4096);
    page.permission = kReadExecute;
    LiteMutableArray pages(1);
    pages.pushObject(reinterpret_cast<LiteObject *>(&page));
    MemoryArena::page_chunks = &pages;
    struct Fixture {
      const char *name;
      bool thumb;
      size_t displaced;
      std::vector<uint32_t> words;
    };
    std::vector<Fixture> fixtures{
        {"arm-add", false, 4, {0xe2800007, 0xe12fff1e}},
        {"arm-backward", false, 12, {0xe3a00003, 0xe2500001, 0x1afffffd, 0xe12fff1e}},
        {"arm-call", false, 8, {0xe52de004, 0xeb000001, 0xe49de004, 0xe12fff1e, 0xe2800001, 0xe12fff1e}},
        {"arm-literal", false, 4, {0xe59f0000, 0xe12fff1e, 0x12345678}},
        {"arm-byte-literal", false, 4, {0xe5df0000, 0xe12fff1e, 0x12345678}},
        {"arm-rotated-adr", false, 4, {0xe28f0c01, 0xe12fff1e}},
        {"thumb-add", true, 2, {0x47703007}},
        {"thumb-literal", true, 2, {0x47704800, 0x12345678}},
        {"thumb-backward", true, 6, {0x38012003, 0x4770d1fd}},
        {"thumb-adr", true, 2, {0x4770a001, 0, 0}},
        {"thumb-it", true, 4, {0xbf0c2800, 0x21092107, 0x4770}},
    };
    for (const auto &f : fixtures) {
      memset(source.p, 0, 4096);
      memcpy(source.p, f.words.data(), f.words.size() * 4);
      page.page_cursor = reinterpret_cast<uintptr_t>(dest.p);
      AssemblyCodeChunk origin{}, relocated{};
      origin.init_region_range(reinterpret_cast<uintptr_t>(source.p) | f.thumb, f.displaced);
      DobbySetError(DOBBY_ERROR_NONE);
      GenRelocateCodeAndBranch(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(source.p) | f.thumb), &origin,
                               &relocated);
      Check(relocated.address, f.name);
      for (unsigned seed = 0; seed < 16; ++seed)
        Check(Run(source, dest, reinterpret_cast<uintptr_t>(relocated.address), origin.length, f.thumb, false, seed) ==
                  Run(source, dest, reinterpret_cast<uintptr_t>(relocated.address), origin.length, f.thumb, true, seed),
              f.name);
      std::printf("PASS %s (16 states)\n", f.name);
    }
    const uint16_t it[] = {0xbf08, 0x4800, 0x4770};
    memcpy(source.p, it, sizeof(it));
    AssemblyCodeChunk origin{}, relocated{};
    origin.init_region_range(reinterpret_cast<uintptr_t>(source.p) | 1, 4);
    GenRelocateCodeAndBranch(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(source.p) | 1), &origin, &relocated);
    Check(!relocated.address && DobbyGetLastError() == DOBBY_ERROR_UNSUPPORTED_INSTRUCTION, "IT rejection");
    // Last halfword in a mapping: no speculative four-byte read or next-instruction fetch.
    auto *edge = static_cast<uint8_t *>(source.p) + 4094;
    uint16_t narrow = 0x2007;
    memcpy(edge, &narrow, 2);
    page.page_cursor = reinterpret_cast<uintptr_t>(dest.p);
    origin.init_region_range(reinterpret_cast<uintptr_t>(edge) | 1, 2);
    relocated.init_region_range(0, 0);
    DobbySetError(DOBBY_ERROR_NONE);
    GenRelocateCodeAndBranch(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(edge) | 1), &origin, &relocated);
    Check(relocated.address, "last-halfword relocation");
    narrow = 0xf000;
    memcpy(edge, &narrow, 2);
    origin.init_region_range(reinterpret_cast<uintptr_t>(edge) | 1, 2);
    relocated.init_region_range(0, 0);
    DobbySetError(DOBBY_ERROR_NONE);
    GenRelocateCodeAndBranch(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(edge) | 1), &origin, &relocated);
    Check(!relocated.address && DobbyGetLastError() == DOBBY_ERROR_UNSUPPORTED_INSTRUCTION, "truncated Thumb-2");
    const uint32_t conditional_literal = 0x059f0000;
    memcpy(source.p, &conditional_literal, 4);
    origin.init_region_range(reinterpret_cast<uintptr_t>(source.p), 4);
    relocated.init_region_range(0, 0);
    DobbySetError(DOBBY_ERROR_NONE);
    GenRelocateCodeAndBranch(source.p, &origin, &relocated);
    Check(!relocated.address && DobbyGetLastError() == DOBBY_ERROR_UNSUPPORTED_INSTRUCTION,
          "conditional ARM literal rejection");
    MemoryArena::page_chunks = nullptr;
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL %s\n", e.what());
    return 1;
  }
}
