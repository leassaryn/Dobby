#include "dobby_internal.h"
#include "InstructionRelocation/InstructionRelocation.h"
#include "MemoryAllocator/NearMemoryArena.h"
#include <capstone/capstone.h>
#include <unicorn/unicorn.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <sys/mman.h>

namespace {
void Check(bool ok, const char *what) {
  if (!ok)
    throw std::runtime_error(what);
}
void U(uc_err error) {
  Check(error == UC_ERR_OK, uc_strerror(error));
}
constexpr size_t page = 4096;
struct Mapping {
  void *p;
  explicit Mapping(uintptr_t hint)
      : p(mmap(reinterpret_cast<void *>(hint), page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) {
    Check(p != MAP_FAILED && reinterpret_cast<uintptr_t>(p) < UINT32_MAX - page, "low mapping unavailable");
  }
  ~Mapping() {
    munmap(p, page);
  }
};
#if defined(DOBBY_TEST_TARGET_X64)
constexpr auto mode = UC_MODE_64;
constexpr auto cs_mode_value = CS_MODE_64;
constexpr int pc_reg = UC_X86_REG_RIP, sp_reg = UC_X86_REG_RSP;
constexpr std::array<int, 16> regs{UC_X86_REG_RAX, UC_X86_REG_RBX, UC_X86_REG_RCX, UC_X86_REG_RDX,
                                   UC_X86_REG_RSI, UC_X86_REG_RDI, UC_X86_REG_RBP, UC_X86_REG_RSP,
                                   UC_X86_REG_R8,  UC_X86_REG_R9,  UC_X86_REG_R10, UC_X86_REG_R11,
                                   UC_X86_REG_R12, UC_X86_REG_R13, UC_X86_REG_R14, UC_X86_REG_R15};
#else
constexpr auto mode = UC_MODE_32;
constexpr auto cs_mode_value = CS_MODE_32;
constexpr int pc_reg = UC_X86_REG_EIP, sp_reg = UC_X86_REG_ESP;
constexpr std::array<int, 8> regs{UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                                  UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP};
#endif
struct State {
  std::array<uint64_t, regs.size()> regs{};
  uint64_t flags{};
  std::array<uint8_t, page> stack{};
  bool operator==(const State &) const = default;
};
struct Decoder {
  csh handle{};
  bool valid = true;
  Decoder() {
    Check(cs_open(CS_ARCH_X86, cs_mode_value, &handle) == CS_ERR_OK, "capstone");
  }
  ~Decoder() {
    cs_close(&handle);
  }
};
void Trace(uc_engine *uc, uint64_t pc, uint32_t size, void *context) {
  auto &decoder = *static_cast<Decoder *>(context);
  std::array<uint8_t, 15> bytes{};
  cs_insn *insn = nullptr;
  if (size > bytes.size() || uc_mem_read(uc, pc, bytes.data(), size) != UC_ERR_OK) {
    decoder.valid = false;
    uc_emu_stop(uc);
    return;
  }
  size_t count = cs_disasm(decoder.handle, bytes.data(), size, pc, 1, &insn);
  if (count != 1 || insn->size != size) {
    decoder.valid = false;
    uc_emu_stop(uc);
  }
  cs_free(insn, count);
}
State Run(Mapping &source, Mapping &dest, size_t displaced, bool relocated, unsigned seed) {
  uc_engine *uc = nullptr;
  U(uc_open(UC_ARCH_X86, mode, &uc));
  struct Close {
    uc_engine *p;
    ~Close() {
      uc_close(p);
    }
  } close{uc};
  const uint64_t start = reinterpret_cast<uintptr_t>(source.p), output = reinterpret_cast<uintptr_t>(dest.p);
  U(uc_mem_map(uc, start, page, UC_PROT_ALL));
  U(uc_mem_write(uc, start, source.p, page));
  U(uc_mem_map(uc, output, page, UC_PROT_ALL));
  U(uc_mem_write(uc, output, dest.p, page));
  U(uc_mem_map(uc, 0x3000000, page, UC_PROT_ALL));
  U(uc_mem_map(uc, 0x4000000, page, UC_PROT_ALL));
  if (relocated) {
    std::vector<uint8_t> traps(displaced, 0xcc);
    U(uc_mem_write(uc, start, traps.data(), traps.size()));
  }
  for (size_t i = 0; i < regs.size(); ++i) {
    uint64_t v = seed + i;
    U(uc_reg_write(uc, regs[i], &v));
  }
  uint64_t sp = 0x3000800, stop = 0x4000000, flags = 2 | ((seed & 1) ? 0x40 : 0);
  U(uc_reg_write(uc, sp_reg, &sp));
  U(uc_reg_write(uc, UC_X86_REG_EFLAGS, &flags));
  U(uc_mem_write(uc, sp, &stop, mode == UC_MODE_64 ? 8 : 4));
  Decoder decoder;
  uc_hook hook;
  U(uc_hook_add(uc, &hook, UC_HOOK_CODE, reinterpret_cast<void *>(Trace), &decoder, 1, 0));
  U(uc_emu_start(uc, relocated ? output : start, stop, 1000000, 1000));
  uint64_t pc = 0;
  U(uc_reg_read(uc, pc_reg, &pc));
  Check(pc == stop && decoder.valid, "execution/decoding failed");
  State result;
  for (size_t i = 0; i < regs.size(); ++i)
    U(uc_reg_read(uc, regs[i], &result.regs[i]));
  U(uc_reg_read(uc, UC_X86_REG_EFLAGS, &result.flags));
  U(uc_mem_read(uc, 0x3000000, result.stack.data(), page));
  return result;
}
} // namespace
int main() {
  try {
    struct Fixture {
      const char *name;
      std::vector<uint8_t> bytes;
      size_t displaced;
    };
    std::vector<Fixture> fixtures{
        {"mov", {0xb8, 7, 0, 0, 0, 0xc3}, 5},
        {"short-jmp", {0xeb, 2, 0x0f, 0x0b, 0xb8, 7, 0, 0, 0, 0xc3}, 2},
        {"short-je", {0x74, 5, 0xb8, 7, 0, 0, 0, 0xc3}, 2},
        {"near-je", {0x0f, 0x84, 5, 0, 0, 0, 0xb8, 7, 0, 0, 0, 0xc3}, 6},
        {"internal-je", {0x74, 5, 0xb8, 7, 0, 0, 0, 0x83, 0xc0, 2, 0xc3}, 10},
    };
#if defined(DOBBY_TEST_TARGET_X64)
    fixtures.push_back({"movabs", {0x48, 0xb8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xc3}, 10});
    fixtures.push_back({"rip-load", {0x8b, 0x05, 2, 0, 0, 0, 0xc3, 0x90, 7, 0, 0, 0}, 6});
#endif
    for (const auto &fixture : fixtures) {
      Mapping source(0x1000000), destination(0x2000000);
      memcpy(source.p, fixture.bytes.data(), fixture.bytes.size());
      AssemblyCodeChunk origin{}, relocated{};
      origin.init_region_range(reinterpret_cast<uintptr_t>(source.p), fixture.displaced);
      relocated.init_region_range(reinterpret_cast<uintptr_t>(destination.p), page);
      DobbySetError(DOBBY_ERROR_NONE);
      GenRelocateCodeAndBranch(source.p, &origin, &relocated);
      Check(relocated.address, "relocation failed");
      for (unsigned seed = 0; seed < 16; ++seed)
        Check(Run(source, destination, origin.length, false, seed) ==
                  Run(source, destination, origin.length, true, seed),
              fixture.name);
      std::printf("PASS %s (16 states)\n", fixture.name);
    }
    for (const auto &bytes : std::vector<std::vector<uint8_t>>{{0xe2, 0xfe}, {0xc5, 0xf8, 0x77}}) {
      Mapping source(0x1000000), destination(0x2000000);
      memcpy(source.p, bytes.data(), bytes.size());
      AssemblyCodeChunk origin{}, relocated{};
      origin.init_region_range(reinterpret_cast<uintptr_t>(source.p), bytes.size());
      relocated.init_region_range(reinterpret_cast<uintptr_t>(destination.p), page);
      DobbySetError(DOBBY_ERROR_NONE);
      GenRelocateCodeAndBranch(source.p, &origin, &relocated);
      Check(!relocated.address && DobbyGetLastError() == DOBBY_ERROR_UNSUPPORTED_INSTRUCTION,
            "unsupported instruction accepted");
    }
    std::puts("PASS explicit rejection: LOOP and VEX");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL %s\n", e.what());
    return 1;
  }
}
