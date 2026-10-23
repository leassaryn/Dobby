#include "dobby.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__aarch64__)
#include <asm/hwcap.h>
#endif

namespace {
int failures = 0;
#define CHECK(expr)                                                                                                    \
  do {                                                                                                                 \
    if (!(expr)) {                                                                                                     \
      std::fprintf(stderr, "FAIL %d: %s (error=%s, errno=%d)\n", __LINE__, #expr,                                      \
                   DobbyGetErrorMessage(DobbyGetLastError()), DobbyGetLastSystemError());                              \
      ++failures;                                                                                                      \
    }                                                                                                                  \
  } while (false)
using Function = int (*)(int);
#if defined(__aarch64__)
constexpr const char *abi = "arm64-v8a";
#elif defined(__arm__)
constexpr const char *abi = "armeabi-v7a";
#elif defined(__x86_64__)
constexpr const char *abi = "x86_64";
#else
constexpr const char *abi = "x86";
#endif

struct Code {
  size_t size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  void *memory = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Code() {
    if (memory == MAP_FAILED)
      std::abort();
  }
  ~Code() {
    munmap(memory, size);
  }
  void Write(const void *bytes, size_t length) {
    std::memcpy(memory, bytes, length);
    __builtin___clear_cache(static_cast<char *>(memory), static_cast<char *>(memory) + length);
  }
  void *Target(bool thumb = false) {
    return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(memory) | (thumb ? 1 : 0));
  }
  void Fill(bool thumb = false) {
#if defined(__aarch64__)
    (void)thumb;
    const uint32_t code[] = {0x528000e0, 0xd503201f, 0xd503201f, 0xd503201f, 0xd503201f, 0xd503201f, 0xd65f03c0};
    Write(code, sizeof(code));
#elif defined(__arm__)
    if (thumb) {
      const uint16_t code[] = {0x2007, 0xbf00, 0xbf00, 0xbf00, 0xbf00, 0xbf00, 0xbf00, 0xbf00, 0xbf00, 0x4770};
      Write(code, sizeof(code));
    } else {
      const uint32_t code[] = {0xe3a00007, 0xe1a00000, 0xe1a00000, 0xe1a00000, 0xe1a00000, 0xe12fff1e};
      Write(code, sizeof(code));
    }
#else
    (void)thumb;
    unsigned char code[32];
    std::memset(code, 0x90, sizeof(code));
    code[0] = 0xb8;
    code[1] = 7;
    code[2] = code[3] = code[4] = 0;
    code[31] = 0xc3;
    Write(code, sizeof(code));
#endif
  }
};
__attribute__((noinline)) int Replacement(int) {
  return 42;
}

void Lifecycle(bool thumb = false) {
  Code code;
  code.Fill(thumb);
  void *target = code.Target(thumb);
  CHECK(reinterpret_cast<Function>(target)(0) == 7);
  Function original = nullptr;
  CHECK(DobbyPrepare(target, reinterpret_cast<void *>(Replacement), reinterpret_cast<void **>(&original)) ==
        RS_SUCCESS);
  if (!original)
    return;
  DobbyHookInfo info{};
  info.struct_size = sizeof(info);
  CHECK(DobbyQueryHook(target, &info) == RS_SUCCESS);
  CHECK(info.state == DOBBY_HOOK_PREPARED && info.kind == DOBBY_HOOK_INLINE);
  CHECK(info.target == target && info.original == reinterpret_cast<void *>(original));
  CHECK(info.patch_size > 0);
  CHECK(reinterpret_cast<Function>(target)(0) == 7);
  CHECK(DobbyReleaseCodeMemory(nullptr) == RS_FAILED && DobbyGetLastError() == DOBBY_ERROR_BUSY);
  CHECK(DobbyCommit(target) == RS_SUCCESS);
  CHECK(DobbyQueryHook(target, &info) == RS_SUCCESS && info.state == DOBBY_HOOK_COMMITTED);
  CHECK(reinterpret_cast<Function>(target)(0) == 42);
  CHECK(original(0) == 7);
  CHECK(DobbyDestroy(target) == RS_SUCCESS);
  CHECK(DobbyQueryHook(target, &info) == RS_FAILED && DobbyGetLastError() == DOBBY_ERROR_NOT_FOUND);
  CHECK(reinterpret_cast<Function>(target)(0) == 7);
  CHECK(original(0) == 7); // safe until caller-declared quiescent collection
  DobbyResourceStats stats{};
  stats.struct_size = sizeof(stats);
  CHECK(DobbyGetResourceStats(&stats) == RS_SUCCESS);
  CHECK(stats.retired_hooks >= 1 && stats.retained_code_bytes >= code.size);
  size_t released = 0;
  CHECK(DobbyCollectRetiredHooks(&released) == RS_SUCCESS && released >= 1);
}

void Errors() {
  CHECK(DobbyCommit(nullptr) == RS_FAILED);
  CHECK(DobbyGetLastError() == DOBBY_ERROR_INVALID_ARGUMENT);
  std::thread worker([] {
    CHECK(DobbyCommit(reinterpret_cast<void *>(0x1000)) == RS_FAILED);
    CHECK(DobbyGetLastError() == DOBBY_ERROR_NOT_FOUND);
  });
  worker.join();
  CHECK(DobbyGetLastError() == DOBBY_ERROR_INVALID_ARGUMENT);
  DobbyHookInfo info{};
  CHECK(DobbyQueryHook(reinterpret_cast<void *>(0x1000), &info) == RS_FAILED);
  CHECK(DobbyGetLastError() == DOBBY_ERROR_INVALID_ARGUMENT);
}

#ifdef DOBBY_TEST_DBI
void SelfDestroy(RegisterContext *, const HookEntryInfo *entry) {
  CHECK(DobbyDestroy(entry->target_address) == RS_SUCCESS);
  CHECK(DobbyCollectRetiredHooks(nullptr) == RS_FAILED && DobbyGetLastError() == DOBBY_ERROR_BUSY);
}
void InstrumentLifetime() {
  Code code;
  code.Fill();
  CHECK(DobbyInstrument(code.memory, SelfDestroy) == RS_SUCCESS);
  CHECK(reinterpret_cast<Function>(code.memory)(0) == 7);
  CHECK(reinterpret_cast<Function>(code.memory)(0) == 7);
  CHECK(DobbyCollectRetiredHooks(nullptr) == RS_SUCCESS);
}
#endif

#if defined(__arm__)
void RejectThumbTrap() {
  for (uint16_t trap : {uint16_t(0xde00), uint16_t(0xdf00)}) {
    Code code;
    code.Fill(true);
    code.Write(&trap, sizeof(trap));
    void *original = nullptr;
    CHECK(DobbyPrepare(code.Target(true), reinterpret_cast<void *>(Replacement), &original) == RS_FAILED);
    CHECK(DobbyGetLastError() == DOBBY_ERROR_UNSUPPORTED_INSTRUCTION);
    CHECK(*static_cast<uint16_t *>(code.memory) == trap);
  }
}
void ArmLiteral(bool thumb) {
  Code code;
  uint32_t bytes[16];
  for (auto &word : bytes)
    word = thumb ? 0xbf00bf00 : 0xe1a00000;
  if (thumb) {
    const uint16_t instructions[] = {0x4808, 0xbf00, 0xbf00, 0xbf00, 0xbf00, 0xbf00, 0xbf00, 0x4770};
    std::memcpy(bytes, instructions, sizeof(instructions));
    bytes[9] = 0x87654321;
  } else {
    bytes[0] = 0xe59f001c; // LDR R0, [PC, #28] -> +36
    bytes[4] = 0xe12fff1e;
    bytes[9] = 0x87654321;
  }
  code.Write(bytes, sizeof(bytes));
  void *target = code.Target(thumb);
  const int expected = reinterpret_cast<Function>(target)(0);
  void *original = nullptr;
  CHECK(DobbyHook(target, reinterpret_cast<void *>(Replacement), &original) == RS_SUCCESS);
  if (!original)
    return;
  CHECK(reinterpret_cast<Function>(target)(0) == 42);
  CHECK(reinterpret_cast<Function>(original)(0) == expected);
  CHECK(DobbyDestroy(target) == RS_SUCCESS);
  CHECK(DobbyCollectRetiredHooks(nullptr) == RS_SUCCESS);
}
#endif

#if defined(__aarch64__) && defined(DOBBY_TEST_NEAR)
void UnsupportedLiteral() {
  Code code;
  const uint32_t instructions[] = {0xdc000080, 0xd503201f, 0xd503201f, 0xd65f03c0, 0x3f800000};
  code.Write(instructions, sizeof(instructions));
  CHECK(DobbyPrepare(code.memory, reinterpret_cast<void *>(Replacement), nullptr) == RS_FAILED);
  CHECK(DobbyGetLastError() == DOBBY_ERROR_UNSUPPORTED_INSTRUCTION);
  CHECK(std::memcmp(code.memory, instructions, sizeof(instructions)) == 0);
  CHECK(DobbyPrepare(static_cast<char *>(code.memory) + 1, reinterpret_cast<void *>(Replacement), nullptr) ==
        RS_FAILED);
  CHECK(DobbyGetLastError() == DOBBY_ERROR_INVALID_ARGUMENT);
}

void ShortRelay() {
  Code code;
  const uint32_t bytes[] = {0x528000e0, 0xd65f03c0, 0x52800c60, 0xd65f03c0};
  code.Write(bytes, sizeof(bytes));
  Function original = nullptr;
  CHECK(DobbyHook(code.memory, reinterpret_cast<void *>(Replacement), reinterpret_cast<void **>(&original)) ==
        RS_SUCCESS);
  if (!original)
    return;
  DobbyHookInfo info{};
  info.struct_size = sizeof(info);
  CHECK(DobbyQueryHook(code.memory, &info) == RS_SUCCESS && info.patch_size == 4);
  const auto distance = static_cast<int64_t>(reinterpret_cast<uintptr_t>(Replacement)) -
                        static_cast<int64_t>(reinterpret_cast<uintptr_t>(code.memory));
  if (distance <= -(int64_t{1} << 27) || distance >= (int64_t{1} << 27))
    CHECK(info.relay != nullptr);
  CHECK(reinterpret_cast<Function>(code.memory)(0) == 42);
  CHECK(original(0) == 7);
  CHECK(reinterpret_cast<Function>(static_cast<char *>(code.memory) + 8)(0) == 99);
  CHECK(DobbyDestroy(code.memory) == RS_SUCCESS);
  CHECK(DobbyCollectRetiredHooks(nullptr) == RS_SUCCESS);
}

void ProtectedEntry(bool guarded, bool pac, bool implicit = false) {
  Code code;
  // Explicit/implicit landing pads, PAC signing and authentication at the same SP.
  const uint32_t plain[] = {0xd503245f, 0x528000e0, 0xd503201f, 0xd65f03c0};
  const uint32_t signed_code[] = {0xd503245f, 0xd503233f, 0x528000e0, 0xd50323bf, 0xd65f03c0};
  if (pac)
    code.Write(signed_code + (implicit ? 1 : 0), sizeof(signed_code) - (implicit ? 4 : 0));
  else
    code.Write(plain, sizeof(plain));
#ifdef PROT_BTI
  if (guarded && mprotect(code.memory, code.size, PROT_READ | PROT_EXEC | PROT_BTI) != 0) {
    CHECK(false);
    return;
  }
#endif
  CHECK(reinterpret_cast<Function>(code.memory)(0) == 7);
  Function original = nullptr;
  CHECK(DobbyHook(code.memory, reinterpret_cast<void *>(Replacement), reinterpret_cast<void **>(&original)) ==
        RS_SUCCESS);
  if (!original)
    return;
  CHECK(reinterpret_cast<Function>(code.memory)(0) == 42);
  CHECK(original(0) == 7);
  CHECK(DobbyDestroy(code.memory) == RS_SUCCESS);
  CHECK(reinterpret_cast<Function>(code.memory)(0) == 7);
  CHECK(DobbyCollectRetiredHooks(nullptr) == RS_SUCCESS);
}

void LoaderStub() {
  Code code;
  const uint32_t instructions[] = {0xd503245f, 0x14000001, 0x528000e0, 0xd65f03c0};
  code.Write(instructions, sizeof(instructions));
  Function original = nullptr;
  CHECK(DobbyHook(code.memory, reinterpret_cast<void *>(Replacement), reinterpret_cast<void **>(&original)) ==
        RS_SUCCESS);
  if (!original)
    return;
  CHECK(reinterpret_cast<Function>(code.memory)(0) == 42);
  CHECK(original(0) == 7);
  CHECK(*static_cast<uint32_t *>(code.memory) == 0xd503245f);
  CHECK(DobbyDestroy(code.memory) == RS_SUCCESS);
  CHECK(std::memcmp(code.memory, instructions, sizeof(instructions)) == 0);
  CHECK(DobbyCollectRetiredHooks(nullptr) == RS_SUCCESS);
}
#endif
} // namespace

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  bool bti = false, pac = false;
#if defined(__aarch64__) && defined(DOBBY_TEST_NEAR)
  bti = (getauxval(AT_HWCAP2) & HWCAP2_BTI) != 0;
  pac = (getauxval(AT_HWCAP) & HWCAP_PACA) != 0;
#endif
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--require-bti"))
      CHECK(bti);
    else if (!std::strcmp(argv[i], "--require-pac"))
      CHECK(pac);
    else if (!std::strcmp(argv[i], "--require-16k"))
      CHECK(sysconf(_SC_PAGESIZE) == 16384);
    else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return 2;
    }
  }
  std::printf("ABI=%s page_size=%ld BTI=%d PAC=%d\n", abi, sysconf(_SC_PAGESIZE), bti, pac);
  Errors();
  Lifecycle();
#if defined(__arm__)
  Lifecycle(true);
  RejectThumbTrap();
  ArmLiteral(false);
  ArmLiteral(true);
#endif
#ifdef DOBBY_TEST_DBI
  InstrumentLifetime();
#endif
#if defined(__aarch64__) && defined(DOBBY_TEST_NEAR)
  UnsupportedLiteral();
  ShortRelay();
  LoaderStub();
  ProtectedEntry(false, false);
  if (bti)
    ProtectedEntry(true, false);
  ProtectedEntry(false, true);
  ProtectedEntry(false, true, true);
  if (bti && pac) {
    ProtectedEntry(true, true);
    ProtectedEntry(true, true, true);
  }
#endif
  size_t released_bytes = 0;
  CHECK(DobbyReleaseCodeMemory(&released_bytes) == RS_SUCCESS && released_bytes > 0);
  DobbyResourceStats empty{};
  empty.struct_size = sizeof(empty);
  CHECK(DobbyGetResourceStats(&empty) == RS_SUCCESS && empty.retained_code_bytes == 0 && empty.retired_hooks == 0);
  Lifecycle(); // fresh arenas after explicit release
#ifdef DOBBY_TEST_DBI
  InstrumentLifetime(); // cached closure bridge must have been rebuilt
#endif
  CHECK(DobbyReleaseCodeMemory(nullptr) == RS_SUCCESS);
  std::printf("DOBBY_RESULT {\"abi\":\"%s\",\"page_size\":%ld,\"bti\":\"%s\",\"pac\":\"%s\",\"failures\":%d}\n", abi,
              sysconf(_SC_PAGESIZE), bti ? "tested" : "skipped", pac ? "tested" : "skipped", failures);
  return failures ? 1 : 0;
}
