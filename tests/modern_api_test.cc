#include "dobby.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <fstream>

#ifdef DOBBY_TEST_PATCH_FAILURES
extern "C" int __real_mprotect(void *, size_t, int);
void *fail_target = nullptr, *rollback_target = nullptr;
bool fail_rollback = false, commit_failed = false;
extern "C" int __wrap_mprotect(void *address, size_t length, int protection) {
  if ((protection & PROT_WRITE) &&
      (address == fail_target || (commit_failed && fail_rollback && address == rollback_target))) {
    if (address == fail_target)
      commit_failed = true;
    errno = EACCES;
    return -1;
  }
  return __real_mprotect(address, length, protection);
}
#endif
namespace {
int failures = 0;
#define CHECK(x)                                                                                                       \
  do {                                                                                                                 \
    if (!(x)) {                                                                                                        \
      std::fprintf(stderr, "FAIL %d: %s (%s)\n", __LINE__, #x, DobbyGetErrorMessage(DobbyGetLastError()));             \
      ++failures;                                                                                                      \
    }                                                                                                                  \
  } while (0)
using Fn = int (*)();
__attribute__((noinline)) int Replacement() {
  return 4242;
}
struct Code {
  size_t size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  void *memory = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Code() {
    if (memory == MAP_FAILED)
      std::abort();
#if defined(__aarch64__)
    const uint32_t bytes[] = {0x528000e0, 0xd503201f, 0xd503201f, 0xd503201f, 0xd503201f, 0xd65f03c0};
#elif defined(__arm__)
    const uint32_t bytes[] = {0xe3a00007, 0xe1a00000, 0xe1a00000, 0xe1a00000, 0xe1a00000, 0xe12fff1e};
#else
    const unsigned char bytes[] = {0xb8, 7, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xc3};
#endif
    std::memcpy(memory, bytes, sizeof(bytes));
    __builtin___clear_cache(static_cast<char *>(memory), static_cast<char *>(memory) + sizeof(bytes));
  }
  ~Code() {
    munmap(memory, size);
  }
};
void OptionsAndBatch() {
  Code a, b;
  DobbyHookOptions limited{sizeof(limited), DOBBY_API_VERSION, DOBBY_NEAR_AUTO, 1};
  CHECK(DobbyPrepareEx(a.memory, reinterpret_cast<void *>(Replacement), nullptr, &limited) == RS_FAILED);
  CHECK(DobbyGetLastError() == DOBBY_ERROR_PATCH_LIMIT);
  CHECK(reinterpret_cast<Fn>(a.memory)() == 7);
  DobbyHookOptions invalid{sizeof(invalid), 100, DOBBY_NEAR_AUTO, 0};
  CHECK(DobbyPrepareEx(a.memory, reinterpret_cast<void *>(Replacement), nullptr, &invalid) == RS_FAILED);
  CHECK(DobbyGetLastError() == DOBBY_ERROR_INVALID_ARGUMENT);
#if defined(__aarch64__)
  DobbyHookOptions near{sizeof(near), DOBBY_API_VERSION, DOBBY_NEAR_REQUIRED, 4};
  DobbyRuntimeInfo capabilities{sizeof(capabilities), DOBBY_API_VERSION};
  CHECK(DobbyGetRuntimeInfo(&capabilities) == RS_SUCCESS);
  if (capabilities.build_features & DOBBY_FEATURE_NEAR) {
    dobby_disable_near_branch_trampoline();
    CHECK(DobbyPrepareEx(a.memory, reinterpret_cast<void *>(Replacement), nullptr, &near) == RS_SUCCESS);
    DobbyHookInfo near_info{};
    near_info.struct_size = sizeof(near_info);
    CHECK(DobbyQueryHook(a.memory, &near_info) == RS_SUCCESS && near_info.patch_size == 4);
    CHECK(DobbyDestroy(a.memory) == RS_SUCCESS);
    dobby_enable_near_branch_trampoline();
  }
#endif
  void *original_a = nullptr, *original_b = nullptr;
  DobbyHookRequest requests[] = {{a.memory, reinterpret_cast<void *>(Replacement), &original_a, nullptr},
                                 {b.memory, reinterpret_cast<void *>(Replacement), &original_b, &limited}};
  DobbyBatchResult result{sizeof(result), DOBBY_API_VERSION};
  CHECK(DobbyHookBatch(requests, 2, &result) == RS_FAILED);
  CHECK(result.failed_index == 1 && result.prepared == 1 && result.committed == 0 && result.rollback_failed == 0);
  DobbyHookInfo info{};
  info.struct_size = sizeof(info);
  CHECK(DobbyQueryHook(a.memory, &info) == RS_FAILED && DobbyGetLastError() == DOBBY_ERROR_NOT_FOUND);
  CHECK(reinterpret_cast<Fn>(a.memory)() == 7);
  requests[1].options = nullptr;
  CHECK(DobbyHookBatch(requests, 2, &result) == RS_SUCCESS);
  CHECK(result.failed_index == SIZE_MAX && result.committed == 2);
  CHECK(reinterpret_cast<Fn>(a.memory)() == 4242 && reinterpret_cast<Fn>(b.memory)() == 4242);
  CHECK(reinterpret_cast<Fn>(original_a)() == 7 && reinterpret_cast<Fn>(original_b)() == 7);
  CHECK(DobbyHookBatch(requests, 2, &result) == RS_FAILED && DobbyGetLastError() == DOBBY_ERROR_CONFLICT);
  CHECK(DobbyDestroy(a.memory) == RS_SUCCESS && DobbyDestroy(b.memory) == RS_SUCCESS);
  requests[1].target = a.memory;
  CHECK(DobbyHookBatch(requests, 2, &result) == RS_FAILED && result.failed_index == 1);
  CHECK(DobbyReleaseCodeMemory(nullptr) == RS_SUCCESS);
}
#ifdef DOBBY_TEST_PATCH_FAILURES
void FailedBatchCommit(bool rollback_fails) {
  Code a, b;
  DobbyHookRequest requests[] = {{a.memory, reinterpret_cast<void *>(Replacement), nullptr, nullptr},
                                 {b.memory, reinterpret_cast<void *>(Replacement), nullptr, nullptr}};
  DobbyBatchResult result{sizeof(result), DOBBY_API_VERSION};
  fail_target = b.memory;
  rollback_target = a.memory;
  fail_rollback = rollback_fails;
  commit_failed = false;
  CHECK(DobbyHookBatch(requests, 2, &result) == RS_FAILED);
  CHECK(result.failed_index == 1 && result.prepared == 2 && result.committed == 1);
  CHECK(result.cause == DOBBY_ERROR_PROTECTION_FAILED && result.system_error == EACCES);
  CHECK(result.rollback_failed == static_cast<size_t>(rollback_fails));
  DobbyHookInfo info{};
  info.struct_size = sizeof(info);
  if (rollback_fails) {
    CHECK(DobbyGetLastError() == DOBBY_ERROR_ROLLBACK_FAILED);
    CHECK(DobbyQueryHook(a.memory, &info) == RS_SUCCESS && info.state == DOBBY_HOOK_COMMITTED);
    CHECK(reinterpret_cast<Fn>(a.memory)() == 4242);
  } else {
    CHECK(DobbyQueryHook(a.memory, &info) == RS_FAILED && DobbyGetLastError() == DOBBY_ERROR_NOT_FOUND);
    CHECK(reinterpret_cast<Fn>(a.memory)() == 7);
  }
  CHECK(DobbyQueryHook(b.memory, &info) == RS_FAILED && DobbyGetLastError() == DOBBY_ERROR_NOT_FOUND);
  fail_target = nullptr;
  rollback_target = nullptr;
  fail_rollback = false;
  if (rollback_fails)
    CHECK(DobbyDestroy(a.memory) == RS_SUCCESS);
  CHECK(DobbyReleaseCodeMemory(nullptr) == RS_SUCCESS);
}
#endif

std::string ModuleMaps() {
  std::ifstream file("/proc/self/maps");
  std::string line, result;
  while (std::getline(file, line))
    if (line.find("libdobby_test_importer.so") != std::string::npos)
      result += line + "\n";
  return result;
}
void Imports(const char *executable) {
  std::string path = executable;
  path = path.substr(0, path.find_last_of('/') + 1) + "libdobby_test_importer.so";
  void *library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    std::fprintf(stderr, "dlopen: %s\n", dlerror());
    CHECK(library);
    return;
  }
  auto call = reinterpret_cast<Fn>(dlsym(library, "dobby_test_imported_call"));
  CHECK(call && call() == getpid());
  if (!call) {
    dlclose(library);
    return;
  }
  DobbyModuleHandle module = 0;
  CHECK(DobbyAcquireModule("libdobby_test_importer.so", &module) == RS_SUCCESS);
  DobbyModuleInfo info{sizeof(info), DOBBY_API_VERSION};
  CHECK(DobbyQueryModule(module, &info) == RS_SUCCESS && info.base && info.path);
#ifdef DOBBY_HAS_LINKER_CALLBACK
  auto callback = reinterpret_cast<linker_load_callback_t>(dlsym(library, "dobby_test_module_callback"));
  DobbyCallbackHandle callback_handle = 0;
  CHECK(DobbyRegisterImageLoadCallback(callback, &callback_handle) == RS_SUCCESS);
  CHECK(DobbyReleaseModule(module) == RS_FAILED && DobbyGetLastError() == DOBBY_ERROR_BUSY);
  CHECK(DobbyUnregisterImageLoadCallback(callback_handle) == RS_SUCCESS);
  CHECK(DobbyStopImageLoadMonitor() == RS_SUCCESS);
  CHECK(DobbyReleaseCodeMemory(nullptr) == RS_SUCCESS);
#endif
  dlclose(library); // the module handle now owns the remaining pin
  CHECK(call() == getpid());
  const auto permissions = ModuleMaps();
  CHECK(!permissions.empty());
  DobbyImportHandle hook = 0, conflict = 0;
  CHECK(DobbyHookImport("libdobby_test_importer.so", "missing_symbol", reinterpret_cast<void *>(Replacement), &hook) ==
        RS_FAILED);
  CHECK(DobbyGetLastError() == DOBBY_ERROR_NOT_FOUND);
  CHECK(DobbyHookImport("libdobby_test_importer.so", "getpid", reinterpret_cast<void *>(Replacement), &hook) ==
        RS_SUCCESS);
  if (!hook) {
    DobbyReleaseModule(module);
    return;
  }
  CHECK(call() == 4242);
  CHECK(ModuleMaps() == permissions);
  CHECK(DobbyHookImport("libdobby_test_importer.so", "getpid", reinterpret_cast<void *>(Replacement), &conflict) ==
        RS_FAILED);
  CHECK(DobbyGetLastError() == DOBBY_ERROR_CONFLICT);
  void *original = nullptr;
  CHECK(DobbyGetImportOriginal(hook, &original) == RS_SUCCESS);
  CHECK(reinterpret_cast<Fn>(original)() == getpid());
  CHECK(DobbyDestroyImport(hook) == RS_SUCCESS);
  CHECK(ModuleMaps() == permissions);
  CHECK(call() == getpid());
  CHECK(DobbyDestroyImport(hook) == RS_FAILED && DobbyGetLastError() == DOBBY_ERROR_NOT_FOUND);
  CHECK(DobbyReleaseModule(module) == RS_SUCCESS);
  CHECK(DobbyQueryModule(module, &info) == RS_FAILED && DobbyGetLastError() == DOBBY_ERROR_NOT_FOUND);
  CHECK(DobbyAcquireModule("libdobby_test_importer.so", &module) == RS_FAILED &&
        DobbyGetLastError() == DOBBY_ERROR_NOT_FOUND);
}
} // namespace
int main(int, char **argv) {
  DobbyRuntimeInfo info{sizeof(info), DOBBY_API_VERSION};
  CHECK(DobbyGetRuntimeInfo(&info) == RS_SUCCESS);
  CHECK(info.page_size == static_cast<size_t>(sysconf(_SC_PAGESIZE)) && info.architecture);
  OptionsAndBatch();
#ifdef DOBBY_TEST_PATCH_FAILURES
  FailedBatchCommit(false);
  FailedBatchCommit(true);
#endif
  Imports(argv[0]);
  std::printf("Modern API: %d failures\n", failures);
  return failures ? 1 : 0;
}
