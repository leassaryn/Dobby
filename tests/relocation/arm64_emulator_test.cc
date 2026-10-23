#include "dobby_internal.h"
#include "InstructionRelocation/InstructionRelocation.h"

#include <capstone/capstone.h>
#include <unicorn/unicorn.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <tuple>
#include <fstream>
#include <random>
#include <charconv>
#include <sys/mman.h>
#include <unistd.h>

namespace {
constexpr uint64_t kPage = 4096; // Unicorn mappings; independent of the Android kernel page size.
constexpr uint64_t kStop = 0x2000000;
constexpr uint64_t kStack = 0x3000000;
constexpr uint64_t kData = 0x4000000;
constexpr uint32_t kRet = 0xd65f03c0;
constexpr uint32_t kNop = 0xd503201f;
constexpr uint32_t kTrap = 0xd4200000;

void Require(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}
void Check(uc_err error) {
  Require(error == UC_ERR_OK, uc_strerror(error));
}

struct Mapping {
  size_t size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  void *data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Mapping() {
    Require(data != MAP_FAILED, "mmap failed");
  }
  ~Mapping() {
    munmap(data, size);
  }
  Mapping(const Mapping &) = delete;
  Mapping &operator=(const Mapping &) = delete;
  uint64_t address() const {
#ifdef DOBBY_EMULATOR_FAR
    return 0x1000000; // Synthetic guest PC, never dereferenced by the generator.
#else
    return reinterpret_cast<uintptr_t>(data);
#endif
  }
};
struct CloseEngine {
  void operator()(uc_engine *engine) const {
    uc_close(engine);
  }
};
struct Decoder {
  csh handle{};
  Decoder() {
    Require(cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &handle) == CS_ERR_OK, "cs_open failed");
  }
  ~Decoder() {
    cs_close(&handle);
  }
  Decoder(const Decoder &) = delete;
  Decoder &operator=(const Decoder &) = delete;
  std::string Decode(const void *bytes, uint64_t address) const {
    cs_insn *instruction = nullptr;
    const size_t count = cs_disasm(handle, static_cast<const uint8_t *>(bytes), 4, address, 1, &instruction);
    if (count != 1)
      return {};
    std::string text = std::string(instruction->mnemonic) + " " + instruction->op_str;
    const bool valid = instruction->size == 4;
    cs_free(instruction, count);
    return valid ? text : std::string{};
  }
};

int X(unsigned index) {
  if (index == 29)
    return UC_ARM64_REG_X29;
  if (index == 30)
    return UC_ARM64_REG_X30;
  return UC_ARM64_REG_X0 + static_cast<int>(index);
}
struct State {
  std::array<uint64_t, 31> x{};
  std::array<std::array<uint64_t, 2>, 32> q{};
  uint64_t sp{}, flags{}, fpcr{}, fpsr{};
  std::array<uint8_t, kPage> data{}, stack{};
};
struct Fixture {
  const char *name;
  const char *first_mnemonic;
  std::vector<uint32_t> words;
  size_t displaced;
  bool object_argument = false;
};
struct Trace {
  const Decoder *decoder;
  std::vector<std::string> instructions;
  bool invalid = false;
};
void Instruction(uc_engine *engine, uint64_t address, uint32_t size, void *context) {
  auto &trace = *static_cast<Trace *>(context);
  uint32_t word{};
  if (size != 4 || uc_mem_read(engine, address, &word, sizeof(word)) != UC_ERR_OK) {
    trace.invalid = true;
    uc_emu_stop(engine);
    return;
  }
  const auto decoded = trace.decoder->Decode(&word, address);
  if (decoded.empty()) {
    trace.invalid = true;
    uc_emu_stop(engine);
    return;
  }
  char prefix[32];
  std::snprintf(prefix, sizeof(prefix), "0x%llx: ", static_cast<unsigned long long>(address));
  trace.instructions.push_back(prefix + decoded);
}

void Map(uc_engine *engine, uint64_t address, const void *bytes, size_t size, uint32_t permissions) {
  const uint64_t base = address & ~(kPage - 1);
  const size_t span = (address - base + size + kPage - 1) & ~(kPage - 1);
  Check(uc_mem_map(engine, base, span, permissions));
  Check(uc_mem_write(engine, address, bytes, size));
}

State Run(const Decoder &decoder, const Mapping &source, const AssemblyCodeChunk &relocated, const Fixture &fixture,
          unsigned seed, bool use_relocated, bool mutate = false) {
  uc_engine *raw = nullptr;
  Check(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &raw));
  std::unique_ptr<uc_engine, CloseEngine> engine(raw);
  State state;
  for (unsigned i = 0; i < state.x.size(); ++i)
    state.x[i] = 0x1000 + i * 17 + seed;
  state.x[0] = fixture.object_argument ? kData : seed;
  state.x[1] = kData;
  state.x[30] = kStop;
  state.sp = kStack + kPage / 2;
  state.flags = (seed & 15U) << 28;
  for (unsigned i = 0; i < state.q.size(); ++i)
    state.q[i] = {0x1020304050607080ULL + i + seed, 0x8877665544332211ULL + i};
  for (size_t i = 0; i < state.data.size(); ++i) {
    state.data[i] = static_cast<uint8_t>(i * 13 + seed);
    state.stack[i] = static_cast<uint8_t>(i * 7 + seed);
  }
  Map(raw, source.address(), source.data, source.size, UC_PROT_READ | UC_PROT_EXEC);
  Map(raw, kStop, &kNop, sizeof(kNop), UC_PROT_READ | UC_PROT_EXEC);
  Map(raw, kStack, state.stack.data(), state.stack.size(), UC_PROT_READ | UC_PROT_WRITE);
  Map(raw, kData, state.data.data(), state.data.size(), UC_PROT_READ | UC_PROT_WRITE);
  if (use_relocated) {
    Map(raw, reinterpret_cast<uintptr_t>(relocated.address), relocated.address, relocated.length,
        UC_PROT_READ | UC_PROT_EXEC);
    // Poison the displaced source, so an internal branch accidentally returning
    // to the original prologue cannot pass a semantic comparison unnoticed.
    for (size_t i = 0; i < fixture.displaced; i += 4)
      Check(uc_mem_write(raw, source.address() + i, &kTrap, sizeof(kTrap)));
    if (mutate)
      Check(uc_mem_write(raw, reinterpret_cast<uintptr_t>(relocated.address), &kNop, sizeof(kNop)));
  }
  for (unsigned i = 0; i < state.x.size(); ++i)
    Check(uc_reg_write(raw, X(i), &state.x[i]));
  for (unsigned i = 0; i < state.q.size(); ++i)
    Check(uc_reg_write(raw, UC_ARM64_REG_Q0 + i, state.q[i].data()));
  Check(uc_reg_write(raw, UC_ARM64_REG_SP, &state.sp));
  Check(uc_reg_write(raw, UC_ARM64_REG_NZCV, &state.flags));
  Check(uc_reg_write(raw, UC_ARM64_REG_FPCR, &state.fpcr));
  Check(uc_reg_write(raw, UC_ARM64_REG_FPSR, &state.fpsr));
  Trace trace{&decoder, {}, false};
  uc_hook hook{};
  Check(uc_hook_add(raw, &hook, UC_HOOK_CODE, reinterpret_cast<void *>(Instruction), &trace, 1, 0));
  const uint64_t start = use_relocated ? reinterpret_cast<uintptr_t>(relocated.address) : source.address();
  const uc_err status = uc_emu_start(raw, start, kStop, 1000000, 1000);
  uint64_t pc{};
  Check(uc_reg_read(raw, UC_ARM64_REG_PC, &pc));
  if (status != UC_ERR_OK || trace.invalid || pc != kStop) {
    for (const auto &line : trace.instructions)
      std::fprintf(stderr, "  %s\n", line.c_str());
    throw std::runtime_error(std::string(use_relocated ? "relocated" : "original") +
                             " execution did not reach stop: " + uc_strerror(status));
  }
  for (unsigned i = 0; i < state.x.size(); ++i)
    Check(uc_reg_read(raw, X(i), &state.x[i]));
  for (unsigned i = 0; i < state.q.size(); ++i)
    Check(uc_reg_read(raw, UC_ARM64_REG_Q0 + i, state.q[i].data()));
  Check(uc_reg_read(raw, UC_ARM64_REG_SP, &state.sp));
  Check(uc_reg_read(raw, UC_ARM64_REG_NZCV, &state.flags));
  Check(uc_reg_read(raw, UC_ARM64_REG_FPCR, &state.fpcr));
  Check(uc_reg_read(raw, UC_ARM64_REG_FPSR, &state.fpsr));
  Check(uc_mem_read(raw, kData, state.data.data(), state.data.size()));
  Check(uc_mem_read(raw, kStack, state.stack.data(), state.stack.size()));
  return state;
}

std::string Difference(const State &a, const State &b) {
  for (unsigned i = 0; i < a.x.size(); ++i) {
    if (i == 16 || i == 17) // documented Dobby scratch registers
      continue;
    if (a.x[i] != b.x[i])
      return "x" + std::to_string(i);
  }
  if (a.q != b.q)
    return "SIMD/FP registers";
  if (a.sp != b.sp || a.flags != b.flags || a.fpcr != b.fpcr || a.fpsr != b.fpsr)
    return "SP/NZCV/FPCR/FPSR";
  if (a.data != b.data)
    return "data memory";
  if (a.stack != b.stack)
    return "stack memory";
  return {};
}

std::vector<Fixture> Fixtures() {
  std::vector<Fixture> fixtures{
      {"arithmetic-and-store", "add", {0x91000c00, 0xf9000020, kRet}, 8},
      {"cbz-forward", "cbz", {0x34000040, 0x11000400, 0x11000800, kRet}, 12},
      {"cbnz-forward", "cbnz", {0x35000040, 0x11000400, 0x11000800, kRet}, 12},
      {"cbnz-backward", "add", {0x11000400, 0x51000400, 0x35ffffe0, kRet}, 12},
      {"tbz-forward", "tbz", {0x36000040, 0x11000400, 0x11000800, kRet}, 12},
      {"tbnz-forward", "tbnz", {0x37000040, 0x11000400, 0x11000800, kRet}, 12},
      {"b-eq-forward", "cmp", {0x7100001f, 0x54000040, 0x52800540, kRet}, 12},
      {"b-forward", "b", {0x14000002, kTrap, 0x91000800, kRet}, 12},
      {"adr-positive", "adr", {0x10000402, kRet}, 4},
      {"adr-negative", "adr", {0x10ffff82, kRet}, 4},
      {"adrp", "adrp", {0x90000002, kRet}, 4},
      {"ldr-s-literal", "ldr", {0x1c000402, kRet}, 4},
      {"ldr-d-literal", "ldr", {0x5c000402, kRet}, 4},
      {"ldr-q-literal", "ldr", {0x9c000402, kRet}, 4},
      {"ldr-w-literal", "ldr", {0x18000402, kRet}, 4},
      {"ldr-x-literal", "ldr", {0x58000402, kRet}, 4},
      {"ldrsw-literal", "ldrsw", {0x98000402, kRet}, 4},
      {"stack-pair", "stp", {0xa9bf7bfd, 0x910003fd, 0xa8c17bfd, kRet}, 12},
      // Actual short IL2CPP prologue; S0 must match, not just integer return registers.
      {"il2cpp-ldr-s0", "ldr", {0xbd42a800, kRet}, 4, true},
      {"simd-load-store", "ldr", {0x3dc00020, 0x3d800420, kRet}, 8},
      {"bti-entry", "bti", {0xd503245f, 0x91000400, kRet}, 8},
  };
  for (auto &fixture : fixtures) {
    fixture.words.resize(40, kNop);
    fixture.words[32] = 0x80000007; // sign-extension-sensitive literal
    fixture.words[33] = 0x12345678;
  }
  Fixture call{"bl-external", "mov", {0xaa1e03f3, 0x9400001f, 0xaa1303fe, kRet}, 12};
  call.words.resize(40, kNop);
  call.words[32] = 0x91000400;
  call.words[33] = kRet;
  fixtures.push_back(call);
  for (const auto &[name, mnemonic, branch] :
       std::array<std::tuple<const char *, const char *, uint32_t>, 7>{{{"b-external", "b", 0x14000020},
                                                                        {"cbz-external", "cbz", 0x34000400},
                                                                        {"cbnz-external", "cbnz", 0x35000400},
                                                                        {"tbz-external", "tbz", 0x36000400},
                                                                        {"tbnz-external", "tbnz", 0x37000400},
                                                                        {"b-eq-external", "b.eq", 0x54000400},
                                                                        {"b-ne-external", "b.ne", 0x54000401}}}) {
    Fixture external{name, mnemonic, {branch, 0x91000400, kRet}, 4};
    external.words.resize(40, kNop);
    external.words[32] = 0x91000800;
    external.words[33] = kRet;
    fixtures.push_back(external);
  }
  return fixtures;
}
} // namespace

int main(int argc, char **argv) {
  Fixture failing{"replay", "", {}, 0};
  std::string failure_path = "relocation-failure.txt";
  unsigned failing_seed = 0;
  bool executing = false;
  try {
    unsigned random_seed = 0xD0BB1, generated_count = 0;
    std::string replay;
    for (int i = 1; i < argc; i += 2) {
      Require(i + 1 < argc, "missing option value");
      std::string arg = argv[i];
      if (arg == "--replay")
        replay = argv[i + 1];
      else if (arg == "--failure")
        failure_path = argv[i + 1];
      else {
        unsigned value = 0;
        const char *end = argv[i + 1] + std::strlen(argv[i + 1]);
        const auto parsed = std::from_chars(argv[i + 1], end, value);
        Require(parsed.ec == std::errc{} && parsed.ptr == end, "invalid number");
        if (arg == "--seed")
          random_seed = value;
        else if (arg == "--cases") {
          Require(value <= 100000, "too many cases");
          generated_count = value;
        } else
          Require(false, "unknown option");
      }
    }
    auto fixtures = Fixtures();
    if (!replay.empty()) {
      std::ifstream file(replay);
      Fixture fixture{"replay", "", {}, 0};
      size_t count;
      unsigned saved_seed;
      unsigned version;
      Require(bool(file >> version >> fixture.displaced >> fixture.object_argument >> saved_seed >> count) &&
                  version == 1 && count > 0 && count <= 1024 && fixture.displaced && fixture.displaced % 4 == 0 &&
                  fixture.displaced <= count * 4 && saved_seed < 16,
              "invalid replay header");
      fixture.words.resize(count);
      for (auto &word : fixture.words)
        Require(bool(file >> std::hex >> word), "invalid replay word");
      fixtures.push_back(fixture); // fixed corpus keeps the negative control active
    }
    std::mt19937 random(random_seed);
    for (unsigned n = 0; n < generated_count; ++n) {
      Fixture fixture{"generated", "add", {0x91000400}, 0};
      for (unsigned j = 0; j < 12; ++j) {
        const uint32_t rd = 2 + random() % 14, rn = 2 + random() % 14;
        switch (random() % 7) {
        case 0:
          fixture.words.push_back(0x91000000U | ((random() % 4096) << 10) | (rn << 5) | rd);
          break;
        case 1:
          fixture.words.push_back(0xf1000000U | ((random() % 4096) << 10) | (rn << 5) | rd);
          break;
        case 2:
          fixture.words.push_back(0xca000000U | ((2 + random() % 14) << 16) | (rn << 5) | rd);
          break;
        case 3:
          fixture.words.push_back(0xf9400020U | ((random() % 32) << 10) | rd);
          break;
        case 4:
          fixture.words.push_back(0xf9000020U | ((random() % 32) << 10) | rd);
          break;
        case 5:
          fixture.words.push_back(0x34000040U);
          break; // bounded forward CBZ
        case 6:
          fixture.words.push_back(0x54000040U | (random() % 14));
          break;
        }
      }
      fixture.displaced = fixture.words.size() * 4;
      fixture.words.push_back(kNop);
      fixture.words.push_back(kRet);
      fixtures.push_back(std::move(fixture));
    }
    std::printf("Fuzz seed=%u generated=%u\n", random_seed, generated_count);
    Decoder decoder;
    unsigned comparisons = 0;
    bool detected_mutation = false;
    for (const auto &fixture : fixtures) {
      failing = fixture;
      executing = true;
      Mapping source;
      Require(fixture.words.size() * 4 <= source.size, "fixture exceeds source mapping");
      std::memcpy(source.data, fixture.words.data(), fixture.words.size() * sizeof(uint32_t));
      const auto decoded = decoder.Decode(source.data, source.address());
      Require(!decoded.empty() &&
                  (!*fixture.first_mnemonic || decoded.starts_with(std::string(fixture.first_mnemonic) + " ")),
              std::string(fixture.name) + ": unexpected source instruction: " + decoded);
      AssemblyCodeChunk origin{}, relocated{};
      origin.init_region_range(source.address(), fixture.displaced);
      DobbySetError(DOBBY_ERROR_NONE);
      GenRelocateCodeAndBranch(source.data, &origin, &relocated);
      Require(relocated.address && relocated.length,
              std::string(fixture.name) + ": " + DobbyGetErrorMessage(DobbyGetLastError()));
#ifdef DOBBY_EMULATOR_FAR
      const auto distance =
          static_cast<int64_t>(reinterpret_cast<uintptr_t>(relocated.address)) - static_cast<int64_t>(source.address());
      Require(distance <= -(int64_t{1} << 27) || distance >= (int64_t{1} << 27), "far setup is not outside B range");
#endif
      for (unsigned seed = 0; seed < 16; ++seed) {
        failing_seed = seed;
        const auto original = Run(decoder, source, relocated, fixture, seed, false);
        const auto rewritten = Run(decoder, source, relocated, fixture, seed, true);
        const auto difference = Difference(original, rewritten);
        Require(difference.empty(),
                std::string(fixture.name) + " seed=" + std::to_string(seed) + ": mismatch " + difference);
        ++comparisons;
        if (!detected_mutation && std::string(fixture.name) == "arithmetic-and-store") {
          detected_mutation = !Difference(original, Run(decoder, source, relocated, fixture, seed, true, true)).empty();
          Require(detected_mutation, "negative control failed: mutation was not detected");
        }
      }
      Require(DobbyReleaseCodeMemory(nullptr) == RS_SUCCESS, "pool teardown failed");
      executing = false;
      if (std::strcmp(fixture.name, "generated"))
        std::printf("PASS %s (16 states)\n", fixture.name);
    }
    Require(detected_mutation, "negative control not executed");
    std::printf("ARM64 relocation: %u differential comparisons passed; mutation detected\n", comparisons);
    return 0;
  } catch (const std::exception &error) {
    if (executing) {
      std::ofstream file(failure_path);
      file << 1 << ' ' << failing.displaced << ' ' << failing.object_argument << ' ' << failing_seed << ' '
           << failing.words.size() << '\n';
      for (auto word : failing.words)
        file << std::hex << word << '\n';
      std::fprintf(stderr, "Replay artifact: %s (%s)\n", failure_path.c_str(), file ? "saved" : "write failed");
    }
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
