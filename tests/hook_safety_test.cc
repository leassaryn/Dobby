#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <inttypes.h>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>

#include "dobby.h"
#include "PlatformUnifiedInterface/ExecMemory/CodePatchTool.h"

#if !defined(__aarch64__)
#error This executable exercises ARM64 machine code.
#endif

namespace {
int failures = 0;
#define CHECK(condition)                                                                                               \
  do {                                                                                                                 \
    if (!(condition)) {                                                                                                \
      std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                                                \
      ++failures;                                                                                                      \
    }                                                                                                                  \
  } while (false)

using Function = int (*)(int);
constexpr uint32_t ret = 0xd65f03c0;
constexpr uint32_t nop = 0xd503201f;
constexpr uint32_t seven = 0x528000e0;
constexpr uint32_t forty_two = 0x52800540;

struct Mapping {
  size_t size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  uint8_t *data = static_cast<uint8_t *>(
      mmap(nullptr, size * 2, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  Mapping() {
    if (data == MAP_FAILED)
      std::abort();
    if (mprotect(data + size, size, PROT_NONE) != 0)
      std::abort();
  }
  ~Mapping() {
    munmap(data, size * 2);
  }
  void Write(size_t offset, std::initializer_list<uint32_t> words) {
    std::memcpy(data + offset, words.begin(), words.size() * sizeof(uint32_t));
    __builtin___clear_cache(reinterpret_cast<char *>(data + offset),
                            reinterpret_cast<char *>(data + offset + words.size() * sizeof(uint32_t)));
  }
};

int CountEntry(void *, void *context) {
  ++*static_cast<int *>(context);
  return 0;
}
int Count() {
  int count = 0;
  CHECK(DobbyEnumerateHooks(CountEntry, &count) == RS_SUCCESS);
  return count;
}

void ShortFunctions() {
  dobby_disable_near_branch_trampoline();
  Mapping memory;
  // Exact bytes from the IL2CPP GetAimPenalty/GetMobility crash.
  memory.Write(0, {0xbd42a800, ret, 0xfc1f0fe8, 0xf90007fe, 0xf940cc08, 0xb4000068});
  std::array<uint8_t, 24> before;
  std::memcpy(before.data(), memory.data, before.size());
  void *far_target = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(memory.data) + (uint64_t{1} << 33));
  void *original = nullptr;
  CHECK(DobbyHook(memory.data, far_target, &original) == RS_FAILED);
  CHECK(original == nullptr);
  CHECK(std::memcmp(memory.data, before.data(), before.size()) == 0);
  CHECK(Count() == 0);

  // RET/BR/B/authenticated returns before the end of a long patch must fail.
  for (uint32_t terminal : {ret, 0xd61f0220U, 0x14000008U, 0xd65f0bffU, 0xd65f0fffU}) {
    for (unsigned index = 0; index < 3; ++index) {
      memory.Write(0, {nop, nop, nop, nop, ret});
      std::memcpy(memory.data + index * 4, &terminal, sizeof(terminal));
      CHECK(DobbyPrepare(memory.data, far_target, nullptr) == RS_FAILED);
      CHECK(Count() == 0);
    }
  }
  memory.Write(0, {nop, nop, nop, ret});
  CHECK(DobbyPrepare(memory.data, far_target, nullptr) == RS_SUCCESS);
  CHECK(DobbyDestroy(memory.data) == RS_SUCCESS);

  // A nearby replacement needs only one B instruction, so the tiny method works.
  memory.Write(0, {seven, ret, 0x52800c60, ret}); // adjacent function returns 99
  memory.Write(128, {forty_two, ret});
  CHECK(DobbyHook(memory.data, memory.data + 128, &original) == RS_SUCCESS);
  CHECK(reinterpret_cast<Function>(memory.data)(0) == 42);
  CHECK(reinterpret_cast<Function>(original)(0) == 7);
  CHECK(reinterpret_cast<Function>(memory.data + 8)(0) == 99);
  CHECK(DobbyDestroy(memory.data) == RS_SUCCESS);
  CHECK(reinterpret_cast<Function>(memory.data)(0) == 7);

  // Relocation must not read the inaccessible page after this single RET.
  memory.Write(memory.size - 4, {ret});
  CHECK(DobbyHook(memory.data + memory.size - 4, memory.data + 128, &original) == RS_SUCCESS);
  CHECK(reinterpret_cast<Function>(original)(17) == 17);
  CHECK(DobbyDestroy(memory.data + memory.size - 4) == RS_SUCCESS);
}

int DestroyEntry(void *address, void *) {
  CHECK(DobbyDestroy(address) == RS_SUCCESS);
  return 0;
}

void ConflictsAndConcurrency() {
  Mapping memory;
  memory.Write(0, {seven, ret});
  memory.Write(128, {forty_two, ret});
  memory.Write(256, {seven, ret});
  CHECK(DobbyHook(memory.data, nullptr, nullptr) == RS_FAILED);
  CHECK(DobbyInstrument(memory.data, nullptr) == RS_FAILED);
  std::atomic<int> succeeded{0};
  std::array<std::thread, 8> threads;
  for (auto &thread : threads)
    thread = std::thread([&] {
      if (DobbyHook(memory.data, memory.data + 128, nullptr) == RS_SUCCESS)
        ++succeeded;
    });
  for (auto &thread : threads)
    thread.join();
  CHECK(succeeded == 8);
  CHECK(Count() == 1);
  CHECK(DobbyHook(memory.data, memory.data + 256, nullptr) == RS_FAILED);
  CHECK(Count() == 1);
  CHECK(reinterpret_cast<Function>(memory.data)(0) == 42);
  // Callback reentry used to deadlock on the registry mutex.
  CHECK(DobbyEnumerateHooks(DestroyEntry, nullptr) == RS_SUCCESS);
  CHECK(Count() == 0);
  CHECK(reinterpret_cast<Function>(memory.data)(0) == 7);
}

void Handler(RegisterContext *, const HookEntryInfo *) {
}
void OtherHandler(RegisterContext *, const HookEntryInfo *) {
}

void InstrumentConflicts() {
  Mapping memory;
  memory.Write(0, {nop, nop, nop, nop, seven, ret});
  CHECK(DobbyInstrument(memory.data, Handler) == RS_SUCCESS);
  CHECK(DobbyInstrument(memory.data, Handler) == RS_SUCCESS);
  CHECK(DobbyInstrument(memory.data, OtherHandler) == RS_FAILED);
  CHECK(Count() == 1);
  CHECK(DobbyDestroy(memory.data) == RS_SUCCESS);
  CHECK(Count() == 0);
}

void FailedRestore() {
  Mapping memory;
  memory.Write(0, {seven, ret});
  memory.Write(128, {forty_two, ret});
  CHECK(DobbyHook(memory.data, memory.data + 128, nullptr) == RS_SUCCESS);
  std::array<uint8_t, 8> installed{};
  std::memcpy(installed.data(), memory.data, installed.size());
  FILE *file = tmpfile();
  if (!file)
    std::abort();
  CHECK(fwrite(memory.data, 1, memory.size, file) == memory.size);
  CHECK(fflush(file) == 0);
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/fd/%d", fileno(file));
  int fd = open(path, O_RDONLY);
  CHECK(fd >= 0);
  // Replace only this test's owned page with a mapping that refuses writes.
  CHECK(mmap(memory.data, memory.size, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0) == memory.data);
  CHECK(DobbyDestroy(memory.data) == RS_FAILED);
  CHECK(Count() == 1); // preserve metadata so restoration can be retried
  CHECK(mmap(memory.data, memory.size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
             0) == memory.data);
  memory.Write(0, {forty_two, ret});
  CHECK(DobbyDestroy(memory.data) == RS_FAILED && DobbyGetLastError() == DOBBY_ERROR_CONFLICT);
  CHECK(Count() == 1);
  std::memcpy(memory.data, installed.data(), installed.size());
  __builtin___clear_cache(reinterpret_cast<char *>(memory.data),
                          reinterpret_cast<char *>(memory.data) + installed.size());
  CHECK(DobbyDestroy(memory.data) == RS_SUCCESS);
  CHECK(reinterpret_cast<Function>(memory.data)(0) == 7);
  CHECK(Count() == 0);
  close(fd);
  fclose(file);
}

int PageProtection(void *address) {
  FILE *maps = fopen("/proc/self/maps", "r");
  if (!maps)
    return -1;
  int result = -1;
  char line[512];
  while (fgets(line, sizeof(line), maps)) {
    uintptr_t start, end;
    char permissions[5]{};
    if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &start, &end, permissions) != 3)
      continue;
    if (reinterpret_cast<uintptr_t>(address) >= start && reinterpret_cast<uintptr_t>(address) < end) {
      result = (permissions[0] == 'r' ? PROT_READ : 0) | (permissions[1] == 'w' ? PROT_WRITE : 0) |
               (permissions[2] == 'x' ? PROT_EXEC : 0);
      break;
    }
  }
  fclose(maps);
  return result;
}

void InternalBranches() {
  dobby_disable_near_branch_trampoline();
  Mapping memory;
  void *far_target = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(memory.data) + (uint64_t{1} << 33));
  void *original = nullptr;
  // CBZ forward into the displaced prologue.
  memory.Write(0, {0x34000040, 0x11000400, 0x11000800, ret});
  CHECK(DobbyPrepare(memory.data, far_target, &original) == RS_SUCCESS);
  CHECK(reinterpret_cast<Function>(original)(0) == 2);
  CHECK(reinterpret_cast<Function>(original)(4) == 7);
  CHECK(DobbyDestroy(memory.data) == RS_SUCCESS);
  // SUB / CBNZ backward to the relocated first instruction.
  memory.Write(0, {0x51000400, 0x35ffffe0, seven, ret});
  CHECK(DobbyPrepare(memory.data, far_target, &original) == RS_SUCCESS);
  CHECK(reinterpret_cast<Function>(original)(3) == 7);
  CHECK(DobbyDestroy(memory.data) == RS_SUCCESS);
  // TBZ forward, taken and fallthrough paths.
  memory.Write(0, {0x36000040, 0x11000400, 0x11000800, ret});
  CHECK(DobbyPrepare(memory.data, far_target, &original) == RS_SUCCESS);
  CHECK(reinterpret_cast<Function>(original)(2) == 4);
  CHECK(reinterpret_cast<Function>(original)(1) == 4);
  CHECK(DobbyDestroy(memory.data) == RS_SUCCESS);
  // CMP / B.EQ forward to RET, taken and fallthrough paths.
  memory.Write(0, {0x7100001f, 0x54000040, forty_two, ret});
  CHECK(DobbyPrepare(memory.data, far_target, &original) == RS_SUCCESS);
  CHECK(reinterpret_cast<Function>(original)(0) == 0);
  CHECK(reinterpret_cast<Function>(original)(1) == 42);
  CHECK(DobbyDestroy(memory.data) == RS_SUCCESS);
}

void CrossPagePatch() {
  Mapping memory;
  CHECK(mprotect(memory.data + memory.size, memory.size, PROT_READ) == 0);
  const int first = PageProtection(memory.data);
  const int second = PageProtection(memory.data + memory.size);
  std::array<uint8_t, 8> bytes{1, 2, 3, 4, 5, 6, 7, 8};
  CHECK(CodePatch(memory.data + memory.size - 4, bytes.data(), bytes.size()) == kMemoryOperationSuccess);
  CHECK(std::memcmp(memory.data + memory.size - 4, bytes.data(), bytes.size()) == 0);
  CHECK(PageProtection(memory.data) == first);
  CHECK(PageProtection(memory.data + memory.size) == second);
}

void FailedCommit() {
  Mapping code;
  code.Write(0, {nop, nop, nop, nop, seven, ret});
  FILE *file = tmpfile();
  if (!file)
    std::abort();
  CHECK(fwrite(code.data, 1, code.size, file) == code.size);
  CHECK(fflush(file) == 0);
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/fd/%d", fileno(file));
  int fd = open(path, O_RDONLY);
  CHECK(fd >= 0);
  void *target = mmap(nullptr, code.size, PROT_READ, MAP_SHARED, fd, 0);
  CHECK(target != MAP_FAILED);
  // A shared mapping from an O_RDONLY descriptor cannot gain write permission.
  CHECK(DobbyPrepare(target, code.data, nullptr) == RS_SUCCESS);
  CHECK(DobbyCommit(target) == RS_FAILED);
  CHECK(DobbyCommit(target) == RS_FAILED);
  CHECK(std::memcmp(target, code.data, 24) == 0);
  CHECK(DobbyDestroy(target) == RS_SUCCESS); // failed commit is still only prepared
  CHECK(Count() == 0);
  munmap(target, code.size);
  close(fd);
  fclose(file);

  uint8_t byte = 0;
  CHECK(CodePatch(nullptr, &byte, 1) == kMemoryOperationError);
  CHECK(CodePatch(code.data, nullptr, 1) == kMemoryOperationError);
  CHECK(CodePatch(code.data, &byte, 0) == kMemoryOperationError);
  CHECK(CodePatch(reinterpret_cast<void *>(UINTPTR_MAX), &byte, 2) == kMemoryOperationError);
}
} // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::puts("Short functions");
  ShortFunctions();
  std::puts("Conflicts and concurrency");
  ConflictsAndConcurrency();
  std::puts("Instrumentation conflicts");
  InstrumentConflicts();
  std::puts("Failed commit");
  FailedCommit();
  std::puts("Failed restore");
  FailedRestore();
  std::puts("Internal branches");
  InternalBranches();
  CrossPagePatch();
  dobby_enable_near_branch_trampoline();
  CHECK(DobbyCollectRetiredHooks(nullptr) == RS_SUCCESS);
  std::printf("Hook safety: %d failures\n", failures);
  return failures ? 1 : 0;
}
