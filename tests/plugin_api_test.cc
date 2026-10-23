#include "dobby.h"
#include "PluginSupport/PackedRelocations.h"
#include "PluginSupport/ElfSymbols.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <dlfcn.h>
#include <unistd.h>
#include <climits>
namespace {
int failures = 0;
int early_status = RS_FAILED;
__attribute__((constructor)) void EarlyModuleLifecycle() {
  DobbyModuleHandle module = 0;
  if (DobbyAcquireModule("libc.so", &module) == RS_SUCCESS)
    early_status = DobbyReleaseModule(module);
}
#define CHECK(x)                                                                                                       \
  do {                                                                                                                 \
    if (!(x)) {                                                                                                        \
      ++failures;                                                                                                      \
      std::fprintf(stderr, "FAIL %d: %s (%s)\n", __LINE__, #x, DobbyGetErrorMessage(DobbyGetLastError()));             \
    }                                                                                                                  \
  } while (0)
using Fn = int (*)();
__attribute__((noinline)) int Replacement() {
  return 4242;
}
void Sleb(std::vector<uint8_t> &out, int64_t value) {
  for (;;) {
    uint8_t byte = value & 127;
    value >>= 7;
    const bool done = (value == 0 && !(byte & 64)) || (value == -1 && (byte & 64));
    out.push_back(byte | (done ? 0 : 128));
    if (done)
      break;
  }
}
std::vector<uint8_t> Stream(std::initializer_list<int64_t> values) {
  std::vector<uint8_t> bytes{'A', 'P', 'S', '2'};
  for (auto value : values)
    Sleb(bytes, value);
  return bytes;
}
void Packed() {
  for (unsigned bits : {32u, 64u}) {
    for (bool rela : {false, true}) {
      for (int flags = 0; flags < 16; ++flags) {
        if (((flags & 4) && !(flags & 8)) || (!rela && (flags & 8)))
          continue;
        auto bytes = Stream({3, 1024, 3, flags});
        if (flags & 2)
          Sleb(bytes, 8);
        if (flags & 1)
          Sleb(bytes, 42);
        if (flags & 4)
          Sleb(bytes, -7);
        for (int i = 0; i < 3; ++i) {
          if (!(flags & 2))
            Sleb(bytes, i == 2 ? -8 : 8);
          if (!(flags & 1))
            Sleb(bytes, 42 + i);
          if ((flags & 8) && !(flags & 4))
            Sleb(bytes, -7);
        }
        size_t count = 0;
        CHECK(dobby::VisitPacked(bytes.data(), bytes.size(), rela, bits,
                                 [&](uint64_t offset, uint64_t info, int64_t addend) {
                                   CHECK(offset == 1024 + ((count == 2 && !(flags & 2)) ? 8 : (count + 1) * 8));
                                   CHECK(info == 42 + ((flags & 1) ? 0 : count));
                                   CHECK(addend == ((flags & 8) ? ((flags & 4) ? -7 : -7 * int64_t(count + 1)) : 0));
                                   ++count;
                                   return true;
                                 }));
        CHECK(count == 3);
        for (size_t size = 0; size < bytes.size(); ++size)
          CHECK(!dobby::VisitPacked(bytes.data(), size, rela, bits, [](auto, auto, auto) { return true; }));
      }
    }
  }
  for (auto invalid : {Stream({1, 0, 0, 0}), Stream({1, 0, 2, 0}), Stream({1, 0, 1, 16}), Stream({1, 0, 1, 4}),
                       Stream({1, INT64_MAX, 1, 3, 1, 0}), Stream({-1, 0}), Stream({1048577, 0})})
    CHECK(!dobby::VisitPacked(invalid.data(), invalid.size(), true, 64, [](auto, auto, auto) { return true; }));
  auto overflowing = Stream({1, 0, 1, 0});
  overflowing.insert(overflowing.end(), 12, 0xff);
  CHECK(!dobby::VisitPacked(overflowing.data(), overflowing.size(), true, 64, [](auto, auto, auto) { return true; }));
  // Deterministic malformed-input coverage; no execution or allocation based on decoded offsets.
  uint32_t random = 12345;
  for (unsigned n = 0; n < 10000; ++n) {
    std::array<uint8_t, 64> bytes{};
    memcpy(bytes.data(), "APS2", 4);
    for (size_t i = 4; i < bytes.size(); ++i) {
      random = random * 1664525 + 1013904223;
      bytes[i] = random >> 24;
    }
    dobby::VisitPacked(bytes.data(), 4 + n % 61, true, 64, [](auto, auto, auto) { return true; });
  }
}
void Symbols() {
  const size_t sections = sizeof(ElfW(Ehdr));
  const size_t symbols = sections + 3 * sizeof(ElfW(Shdr));
  const size_t strings = symbols + sizeof(ElfW(Sym));
  std::vector<uint8_t> file(strings + 5);
  ElfW(Ehdr) header{};
  memcpy(header.e_ident, ELFMAG, SELFMAG);
  header.e_ident[EI_CLASS] = sizeof(void *) == 8 ? ELFCLASS64 : ELFCLASS32;
  header.e_ident[EI_DATA] = ELFDATA2LSB;
  header.e_ident[EI_VERSION] = EV_CURRENT;
  header.e_type = ET_DYN;
  header.e_shoff = sections;
  header.e_shentsize = sizeof(ElfW(Shdr));
  header.e_shnum = 3;
  memcpy(file.data(), &header, sizeof(header));
  ElfW(Shdr) table{};
  table.sh_type = SHT_SYMTAB;
  table.sh_offset = symbols;
  table.sh_size = sizeof(ElfW(Sym));
  table.sh_entsize = sizeof(ElfW(Sym));
  table.sh_link = 2;
  memcpy(file.data() + sections + sizeof(table), &table, sizeof(table));
  ElfW(Shdr) names{};
  names.sh_type = SHT_STRTAB;
  names.sh_offset = strings;
  names.sh_size = 5;
  memcpy(file.data() + sections + 2 * sizeof(table), &names, sizeof(names));
  ElfW(Sym) symbol{};
  symbol.st_name = 1;
  symbol.st_shndx = 1;
  symbol.st_value = 0x1234;
  symbol.st_info = STT_FUNC;
  memcpy(file.data() + symbols, &symbol, sizeof(symbol));
  memcpy(file.data() + strings, "\0foo", 5);
  uintptr_t value = 0;
  CHECK(dobby::FindFileSymbol(file.data(), file.size(), "foo", value) && value == 0x1234);
  CHECK(!dobby::FindFileSymbol(file.data(), file.size(), "bar", value));
  for (size_t size = 0; size < file.size(); ++size)
    CHECK(!dobby::FindFileSymbol(file.data(), size, "foo", value));
  for (size_t offset = 0; offset < file.size(); ++offset) {
    auto corrupt = file;
    corrupt[offset] = 0xff;
    dobby::FindFileSymbol(corrupt.data(), corrupt.size(), "foo", value);
  }
  file.back() = 'X';
  CHECK(!dobby::FindFileSymbol(file.data(), file.size(), "foo", value));
}
void *refuse_target = nullptr;
int Enter(void *address, size_t, DobbyPatchOperation, void *) {
#if defined(__arm__)
  address = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(address) & ~uintptr_t{1});
  void *target = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(refuse_target) & ~uintptr_t{1});
#else
  void *target = refuse_target;
#endif
  return address == target ? RS_FAILED : RS_SUCCESS;
}
void Leave(void *, size_t, DobbyPatchOperation, int, void *) {
}
bool unloaded = false;
void OnUnload() {
  DobbyPluginState state{sizeof(state), DOBBY_API_VERSION};
  unloaded = DobbyQueryPluginState(&state) == RS_SUCCESS;
}
void Plugins(const char *executable) {
  const std::string path =
      std::string(executable).substr(0, std::string(executable).find_last_of('/') + 1) + "libdobby_test_packed.so";
  DobbyEnableDiagnostics(1);
  DobbyAddressSpec spec{sizeof(spec), DOBBY_API_VERSION};
  char symbol_name[] = "dobby_test_imported_call";
  spec.symbol = symbol_name;
  DobbyDeferredHandle request = 0;
  CHECK(DobbyRegisterDeferredHook(path.c_str(), &spec, reinterpret_cast<void *>(Replacement), nullptr, &request) ==
        RS_SUCCESS);
  DobbyDeferredInfo deferred{sizeof(deferred), DOBBY_API_VERSION};
  CHECK(DobbyQueryDeferredHook(request, &deferred) == RS_SUCCESS && deferred.state == DOBBY_DEFERRED_PENDING);
  CHECK(DobbyShutdownPlugins() == RS_FAILED && DobbyGetLastError() == DOBBY_ERROR_BUSY);
  symbol_name[0] = 'X'; // Registration must own its input strings.
  CHECK(DobbyQueryDeferredHook(0, &deferred) == RS_FAILED);
  void *library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  CHECK(DobbyGetLastError() == DOBBY_ERROR_NOT_FOUND);
  CHECK(library);
  if (!library) {
    std::fprintf(stderr, "%s\n", dlerror());
    return;
  }
#ifndef DOBBY_TEST_AUTO_DEFERRED
  CHECK(DobbyPollDeferredHooks() == RS_SUCCESS);
#endif
  CHECK(DobbyQueryDeferredHook(request, &deferred) == RS_SUCCESS && deferred.state == DOBBY_DEFERRED_INSTALLED);
  symbol_name[0] = 'd';
  auto call = reinterpret_cast<Fn>(dlsym(library, spec.symbol));
  CHECK(call && call() == 4242);
  if (deferred.original)
    CHECK(reinterpret_cast<Fn>(deferred.original)() == getpid());
  DobbyPluginState state{sizeof(state), DOBBY_API_VERSION};
  CHECK(DobbyQueryPluginState(&state) == RS_SUCCESS && state.deferred_installed == 1);
  CHECK(DobbyPollDeferredHooks() == RS_SUCCESS);
  CHECK(DobbyCancelDeferredHook(request) == RS_SUCCESS);
  CHECK(call() == getpid());
  CHECK(DobbyCancelDeferredHook(request) == RS_FAILED);
  // A refused commit must release its prepared hook and remain retryable.
  DobbyPatchCoordinator coordinator{sizeof(coordinator), DOBBY_API_VERSION, Enter, Leave, nullptr};
  refuse_target = reinterpret_cast<void *>(call);
  CHECK(DobbySetPatchCoordinator(&coordinator) == RS_SUCCESS);
  CHECK(DobbyRegisterDeferredHook(path.c_str(), &spec, reinterpret_cast<void *>(Replacement), nullptr, &request) ==
        RS_SUCCESS);
  CHECK(DobbyQueryDeferredHook(request, &deferred) == RS_SUCCESS && deferred.state == DOBBY_DEFERRED_FAILED &&
        deferred.error == DOBBY_ERROR_BUSY);
  CHECK(call() == getpid());
  CHECK(DobbySetPatchCoordinator(nullptr) == RS_SUCCESS);
  CHECK(DobbyPollDeferredHooks() == RS_SUCCESS);
  CHECK(DobbyQueryDeferredHook(request, &deferred) == RS_SUCCESS && deferred.state == DOBBY_DEFERRED_INSTALLED);
  CHECK(DobbyCancelDeferredHook(request) == RS_SUCCESS);
  // A foreign owner is never adopted, nor removed by cancellation.
  void *foreign_original = nullptr;
  CHECK(DobbyHook(reinterpret_cast<void *>(call), reinterpret_cast<void *>(Replacement), &foreign_original) ==
        RS_SUCCESS);
  CHECK(DobbyRegisterDeferredHook(path.c_str(), &spec, reinterpret_cast<void *>(Replacement), nullptr, &request) ==
        RS_SUCCESS);
  CHECK(DobbyQueryDeferredHook(request, &deferred) == RS_SUCCESS && deferred.state == DOBBY_DEFERRED_FAILED &&
        deferred.error == DOBBY_ERROR_CONFLICT);
  CHECK(DobbyCancelDeferredHook(request) == RS_FAILED && DobbyGetLastError() == DOBBY_ERROR_BUSY);
  CHECK(call() == 4242);
  CHECK(DobbyDestroy(reinterpret_cast<void *>(call)) == RS_SUCCESS);
  CHECK(DobbyPollDeferredHooks() == RS_SUCCESS); // Cancelled requests must not reinstall while release is pending.
  CHECK(call() == getpid());
  CHECK(DobbyCancelDeferredHook(request) == RS_SUCCESS);
  DobbyModuleHandle module = 0;
  CHECK(DobbyAcquireModule(path.c_str(), &module) == RS_SUCCESS);
  DobbyModuleInfo module_info{sizeof(module_info), DOBBY_API_VERSION};
  CHECK(DobbyQueryModule(module, &module_info) == RS_SUCCESS);
  uint8_t id[64];
  size_t id_size = 0;
  CHECK(DobbyGetModuleBuildId(module, nullptr, 0, &id_size) == RS_SUCCESS && id_size > 0);
  CHECK(DobbyGetModuleBuildId(module, id, 0, &id_size) == RS_FAILED && DobbyGetLastError() == DOBBY_ERROR_PATCH_LIMIT);
  CHECK(DobbyGetModuleBuildId(module, id, sizeof(id), &id_size) == RS_SUCCESS);
  spec.build_id = id;
  spec.build_id_size = id_size;
  void *target = nullptr;
  CHECK(DobbyResolveModuleAddress(module, &spec, &target) == RS_SUCCESS && target == reinterpret_cast<void *>(call));
  id[0] ^= 1;
  CHECK(DobbyResolveModuleAddress(module, &spec, &target) == RS_FAILED && DobbyGetLastError() == DOBBY_ERROR_CONFLICT);
  id[0] ^= 1;
  spec.symbol = nullptr;
  spec.offset = reinterpret_cast<uintptr_t>(call) - reinterpret_cast<uintptr_t>(module_info.base);
  uint8_t prologue[4];
  uintptr_t code = reinterpret_cast<uintptr_t>(call);
#if defined(__arm__)
  code &= ~uintptr_t{1};
#endif
  memcpy(prologue, reinterpret_cast<void *>(code), 4);
  spec.expected_bytes = prologue;
  spec.expected_size = 4;
  CHECK(DobbyResolveModuleAddress(module, &spec, &target) == RS_SUCCESS);
  prologue[0] ^= 1;
  CHECK(DobbyResolveModuleAddress(module, &spec, &target) == RS_FAILED && DobbyGetLastError() == DOBBY_ERROR_CONFLICT);
  spec.expected_size = 0;
  spec.offset = 0;
  CHECK(DobbyResolveModuleAddress(module, &spec, &target) == RS_FAILED);
  // An exported dependency symbol must not resolve outside the chosen module.
  spec.symbol = "getpid";
  CHECK(DobbyResolveModuleAddress(module, &spec, &target) == RS_FAILED);
#ifdef DOBBY_TEST_SYMBOL_RESOLVER
  spec.symbol = "dobby_test_private_function";
  CHECK(DobbyResolveModuleAddress(module, &spec, &target) == RS_SUCCESS);
  auto private_address = reinterpret_cast<void *(*)()>(dlsym(library, "dobby_test_private_address"));
  CHECK(private_address && target == private_address());
  CHECK(reinterpret_cast<Fn>(target)() == 73);
  CHECK(DobbySymbolResolver("missing-module.so", "getpid") == nullptr);
  CHECK(DobbySymbolResolver(path.c_str(), "getpid") == nullptr);
  CHECK(DobbySymbolResolver(nullptr, nullptr) == nullptr && DobbyGetLastError() == DOBBY_ERROR_INVALID_ARGUMENT);
#endif
  DobbyImportOptions imports{sizeof(imports), DOBBY_API_VERSION, "LIBC", DOBBY_IMPORT_GLOB_DAT};
  size_t slots = 0;
  CHECK(DobbyInspectImport(path.c_str(), "getppid", &imports, nullptr, 0, &slots) == RS_SUCCESS && slots > 0);
  DobbyImportHandle hook = 0;
  CHECK(DobbyHookImportEx(path.c_str(), "getppid", reinterpret_cast<void *>(Replacement), &imports, &hook) ==
        RS_SUCCESS);
  auto address = reinterpret_cast<void *(*)()>(dlsym(library, "dobby_test_import_address"));
  CHECK(address && reinterpret_cast<Fn>(address())() == 4242);
  CHECK(DobbyDestroyImport(hook) == RS_SUCCESS);
  CHECK(reinterpret_cast<Fn>(address())() == getppid());
  CHECK(DobbyReleaseModule(module) == RS_SUCCESS);
  // Last pin release executes a foreign destructor that joins a thread using Dobby.
  // Holding the operation lock across dlclose would deadlock this test.
  spec.symbol = "missing_deferred_symbol";
  spec.build_id_size = 0;
  CHECK(DobbyRegisterDeferredHook(path.c_str(), &spec, reinterpret_cast<void *>(Replacement), nullptr, &request) ==
        RS_SUCCESS);
  auto on_unload = reinterpret_cast<void (*)(void (*)())>(dlsym(library, "dobby_test_on_unload"));
  CHECK(on_unload);
  if (on_unload)
    on_unload(OnUnload);
  dlclose(library);
  CHECK(!unloaded);
  CHECK(DobbyCancelDeferredHook(request) == RS_SUCCESS);
  CHECK(unloaded);
  CHECK(DobbyShutdownPlugins() == RS_SUCCESS);
  CHECK(DobbyShutdownPlugins() == RS_SUCCESS);
  CHECK(DobbyCollectRetiredHooks(nullptr) == RS_SUCCESS);
  CHECK(DobbyReleaseCodeMemory(nullptr) == RS_SUCCESS);
  // Ring overflow must be bounded, observable and preserve thread-local errors.
  for (unsigned i = 0; i < 300; ++i)
    DobbyQueryDeferredHook(0, &deferred);
  DobbyDiagnosticEvent events[256];
  size_t count = 0;
  uint64_t dropped = 0;
  const auto error = DobbyGetLastError();
  CHECK(DobbyReadDiagnostics(events, 256, &count, &dropped) == RS_SUCCESS && count == 256 && dropped > 0);
  CHECK(DobbyGetLastError() == error);
  for (size_t i = 1; i < count; ++i)
    CHECK(events[i].sequence > events[i - 1].sequence);
  CHECK(DobbyEnableDiagnostics(0) == RS_SUCCESS);
}
} // namespace
int main(int argc, char **argv) {
  CHECK(early_status == RS_SUCCESS);
  Packed();
  Symbols();
  if (argc > 1 && !strcmp(argv[1], "--parser-only"))
    return failures ? 1 : 0;
  Plugins(argv[0]);
  std::printf("Plugin API: %d failures\n", failures);
  return failures ? 1 : 0;
}
