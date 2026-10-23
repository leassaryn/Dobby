#include "dobby_internal.h"
#include "Interceptor.h"
#include "HookLifecycle.h"
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <limits>
#include <algorithm>

namespace {
struct Module {
  void *pin = nullptr;
  uintptr_t base = 0;
  std::string path;
  std::vector<ElfW(Phdr)> headers;
  ~Module() {
    InternalLoaderScope scope;
    if (pin)
      dlclose(pin);
  }
  bool Contains(uintptr_t address, size_t length) const {
    if (length > UINTPTR_MAX - address)
      return false;
    for (const auto &h : headers) {
      if (h.p_type != PT_LOAD || !(h.p_flags & PF_R) || h.p_vaddr > UINTPTR_MAX - base)
        continue;
      const uintptr_t start = base + h.p_vaddr;
      if (h.p_memsz > UINTPTR_MAX - start)
        continue;
      if (address >= start && address + length <= start + h.p_memsz)
        return true;
    }
    return false;
  }
  uintptr_t Pointer(uintptr_t offset, size_t length) const {
    if (offset <= UINTPTR_MAX - base && Contains(base + offset, length))
      return base + offset;
    // Some native-development ELF loaders relocate DT_* pointers in place.
    return Contains(offset, length) ? offset : 0;
  }
};
struct Search {
  const char *name;
  Module *module;
  size_t matches = 0;
};
int Find(struct dl_phdr_info *info, size_t, void *context) {
  auto &search = *static_cast<Search *>(context);
  const char *path = info->dlpi_name;
  if (!path || !*path)
    return 0;
  const char *basename = strrchr(path, '/');
  basename = basename ? basename + 1 : path;
  if (strcmp(search.name, path) && (strchr(search.name, '/') || strcmp(search.name, basename)))
    return 0;
  ++search.matches;
  search.module->base = info->dlpi_addr;
  search.module->path = path;
  search.module->headers.assign(info->dlpi_phdr, info->dlpi_phdr + info->dlpi_phnum);
  return 0;
}
std::shared_ptr<Module> Acquire(const char *name) {
  if (!name || !*name) {
    DobbySetError(DOBBY_ERROR_INVALID_ARGUMENT);
    return {};
  }
  auto module = std::make_shared<Module>();
  Search search{name, module.get()};
  dl_iterate_phdr(Find, &search);
  if (search.matches != 1) {
    DobbySetError(search.matches ? DOBBY_ERROR_CONFLICT : DOBBY_ERROR_NOT_FOUND);
    return {};
  }
  InternalLoaderScope loader_scope;
  module->pin = dlopen(module->path.c_str(), RTLD_NOW | RTLD_NOLOAD);
  if (!module->pin) {
    DobbySetError(DOBBY_ERROR_NOT_FOUND);
    return {};
  }
  // Refresh the identity while pinned, before dereferencing any dynamic data.
  const std::string exact = module->path;
  Search refresh{exact.c_str(), module.get()};
  dl_iterate_phdr(Find, &refresh);
  if (refresh.matches != 1) {
    DobbySetError(DOBBY_ERROR_NOT_FOUND);
    return {};
  }
  return module;
}
struct Slot {
  void **address;
  void *original;
  bool applied = false;
};
struct Import {
  std::shared_ptr<Module> module;
  void *replacement;
  std::vector<Slot> slots;
};
std::unordered_map<uint64_t, std::shared_ptr<Module>> modules;
std::unordered_map<uint64_t, Import> imports;
uint64_t next_module = 1, next_import = 1;

bool Slots(const Module &module, const char *name, std::vector<Slot> &slots) {
  const ElfW(Dyn) *dynamic = nullptr;
  size_t count = 0;
  for (const auto &h : module.headers) {
    if (h.p_type == PT_DYNAMIC) {
      const auto address = module.Pointer(h.p_vaddr, h.p_memsz);
      if (!address)
        return false;
      dynamic = reinterpret_cast<const ElfW(Dyn) *>(address);
      count = h.p_memsz / sizeof(*dynamic);
      break;
    }
  }
  if (!dynamic)
    return false;
  uintptr_t strings = 0, symbols = 0, relocations = 0;
  size_t string_size = 0, relocation_size = 0, symbol_size = 0;
  ElfW(Sxword) kind = 0;
  bool terminated = false;
  for (size_t i = 0; i < count; ++i) {
    const auto &d = dynamic[i];
    if (d.d_tag == DT_NULL) {
      terminated = true;
      break;
    }
    switch (d.d_tag) {
    case DT_STRTAB:
      strings = d.d_un.d_ptr;
      break;
    case DT_STRSZ:
      string_size = d.d_un.d_val;
      break;
    case DT_SYMTAB:
      symbols = d.d_un.d_ptr;
      break;
    case DT_SYMENT:
      symbol_size = d.d_un.d_val;
      break;
    case DT_JMPREL:
      relocations = d.d_un.d_ptr;
      break;
    case DT_PLTRELSZ:
      relocation_size = d.d_un.d_val;
      break;
    case DT_PLTREL:
      kind = d.d_un.d_val;
      break;
    }
  }
  if (terminated && !relocations && !relocation_size)
    return true;
  if (!terminated || symbol_size != sizeof(ElfW(Sym)) || (kind != DT_REL && kind != DT_RELA))
    return false;
  strings = module.Pointer(strings, string_size);
  symbols = module.Pointer(symbols, sizeof(ElfW(Sym)));
  relocations = module.Pointer(relocations, relocation_size);
  const size_t stride = kind == DT_RELA ? sizeof(ElfW(Rela)) : sizeof(ElfW(Rel));
  if (!strings || !symbols || !relocations || !string_size || relocation_size % stride)
    return false;
  for (size_t i = 0; i < relocation_size / stride; ++i) {
    ElfW(Addr) offset;
    ElfW(Xword) info;
    if (kind == DT_RELA) {
      auto &r = reinterpret_cast<const ElfW(Rela) *>(relocations)[i];
      offset = r.r_offset;
      info = r.r_info;
    } else {
      auto &r = reinterpret_cast<const ElfW(Rel) *>(relocations)[i];
      offset = r.r_offset;
      info = r.r_info;
    }
#if __SIZEOF_POINTER__ == 8
    const size_t symbol_index = ELF64_R_SYM(info);
    const auto type = ELF64_R_TYPE(info);
#else
    const size_t symbol_index = ELF32_R_SYM(info);
    const auto type = ELF32_R_TYPE(info);
#endif
#if defined(__aarch64__)
    constexpr auto jump_slot = R_AARCH64_JUMP_SLOT;
#elif defined(__arm__)
    constexpr auto jump_slot = R_ARM_JUMP_SLOT;
#elif defined(__x86_64__)
    constexpr auto jump_slot = R_X86_64_JUMP_SLOT;
#else
    constexpr auto jump_slot = R_386_JMP_SLOT;
#endif
    if (type != jump_slot)
      continue;
    if (symbol_index > (UINTPTR_MAX - symbols) / sizeof(ElfW(Sym)))
      return false;
    const uintptr_t address = symbols + symbol_index * sizeof(ElfW(Sym));
    if (!module.Contains(address, sizeof(ElfW(Sym))))
      return false;
    const auto &symbol = *reinterpret_cast<const ElfW(Sym) *>(address);
    if (symbol.st_name >= string_size)
      return false;
    const char *symbol_name = reinterpret_cast<const char *>(strings + symbol.st_name);
    if (!memchr(symbol_name, 0, string_size - symbol.st_name))
      return false;
    if (strcmp(name, symbol_name))
      continue;
    if (offset > UINTPTR_MAX - module.base)
      return false;
    auto *slot = reinterpret_cast<void **>(module.base + offset);
    if (reinterpret_cast<uintptr_t>(slot) % alignof(void *) ||
        !module.Contains(reinterpret_cast<uintptr_t>(slot), sizeof(void *)))
      return false;
    if (std::any_of(slots.begin(), slots.end(), [&](const Slot &s) { return s.address == slot; }))
      continue;
    slots.push_back({slot, __atomic_load_n(slot, __ATOMIC_ACQUIRE), false});
  }
  return true;
}

bool Patch(Slot &slot, void *expected, void *value) {
  if (__atomic_load_n(slot.address, __ATOMIC_ACQUIRE) != expected) {
    DobbySetError(DOBBY_ERROR_CONFLICT);
    return false;
  }
  // Management is serialized; caller must keep importing code quiescent.
  return CodePatch(slot.address, reinterpret_cast<uint8_t *>(&value), sizeof(value)) == kMemoryOperationSuccess;
}
} // namespace

PUBLIC int DobbyAcquireModule(const char *name, DobbyModuleHandle *handle) {
  DobbySetError(DOBBY_ERROR_NONE);
  if (!handle)
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  *handle = 0;
  auto module = Acquire(name);
  if (!module)
    return RS_FAILED;
  HookOperationGuard guard;
  if (!next_module)
    return DobbyFailure(DOBBY_ERROR_BUSY);
  *handle = next_module++;
  modules.emplace(*handle, std::move(module));
  return DobbySuccess();
}
PUBLIC int DobbyQueryModule(DobbyModuleHandle handle, DobbyModuleInfo *info) {
  DobbySetError(DOBBY_ERROR_NONE);
  HookOperationGuard guard;
  if (!info || info->struct_size < sizeof(*info) || info->api_version != DOBBY_API_VERSION)
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  const auto it = modules.find(handle);
  if (it == modules.end())
    return DobbyFailure(DOBBY_ERROR_NOT_FOUND);
  *info = {sizeof(*info), DOBBY_API_VERSION, reinterpret_cast<void *>(it->second->base), it->second->path.c_str()};
  return DobbySuccess();
}
PUBLIC int DobbyReleaseModule(DobbyModuleHandle handle) {
  DobbySetError(DOBBY_ERROR_NONE);
  std::shared_ptr<Module> module;
  {
    HookOperationGuard guard;
    auto it = modules.find(handle);
    if (it == modules.end())
      return DobbyFailure(DOBBY_ERROR_NOT_FOUND);
    struct Check {
      const Module *module;
      bool busy;
    } check{it->second.get(), false};
    Interceptor::SharedInstance()->Iterate(
        [](void *target, void *data) {
          auto &check = *static_cast<Check *>(data);
          DobbyHookInfo info{};
          info.struct_size = sizeof(info);
          if (DobbyQueryHook(target, &info) == RS_SUCCESS &&
              (check.module->Contains(reinterpret_cast<uintptr_t>(info.target) & ~uintptr_t{1}, 1) ||
               check.module->Contains(reinterpret_cast<uintptr_t>(info.replacement) & ~uintptr_t{1}, 1)))
            check.busy = true;
          return 0;
        },
        &check);
    for (const auto &[id, entry] : imports)
      if (it->second->Contains(reinterpret_cast<uintptr_t>(entry.replacement) & ~uintptr_t{1}, 1))
        check.busy = true;
#ifdef DOBBY_HAS_LINKER_CALLBACK
    extern bool DobbyCallbacksUseRange(uintptr_t start, size_t size);
    for (const auto &h : it->second->headers)
      if (h.p_type == PT_LOAD && DobbyCallbacksUseRange(it->second->base + h.p_vaddr, h.p_memsz))
        check.busy = true;
#endif
    if (check.busy)
      return DobbyFailure(DOBBY_ERROR_BUSY);
    module = std::move(it->second);
    modules.erase(it);
  }
  module.reset();
  return DobbySuccess();
}
PUBLIC int DobbyHookImport(const char *name, const char *symbol, void *replacement, DobbyImportHandle *handle) {
  DobbySetError(DOBBY_ERROR_NONE);
  if (!symbol || !*symbol || !replacement || !handle)
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  *handle = 0;
  auto module = Acquire(name);
  if (!module)
    return RS_FAILED;
  HookOperationGuard guard;
  Import entry{module, replacement, {}};
  if (!Slots(*module, symbol, entry.slots))
    return DobbyFailure(DOBBY_ERROR_UNSUPPORTED_FORMAT);
  if (entry.slots.empty())
    return DobbyFailure(DOBBY_ERROR_NOT_FOUND);
  for (const auto &slot : entry.slots) {
    if (!slot.original || slot.original != entry.slots.front().original || slot.original == replacement)
      return DobbyFailure(DOBBY_ERROR_CONFLICT);
    for (const auto &[id, active] : imports)
      for (const auto &other : active.slots)
        if (other.address == slot.address)
          return DobbyFailure(DOBBY_ERROR_CONFLICT);
  }
  if (!next_import)
    return DobbyFailure(DOBBY_ERROR_BUSY);
  const uint64_t id = next_import++;
  auto [it, inserted] = imports.emplace(id, std::move(entry));
  (void)inserted;
  DobbyError warning = DOBBY_ERROR_NONE;
  int warning_errno = 0;
  for (auto &slot : it->second.slots) {
    if (!Patch(slot, slot.original, replacement)) {
      const auto cause = DobbyGetLastError();
      const int system_error = DobbyGetLastSystemError();
      bool rollback = true;
      for (auto &previous : it->second.slots) {
        if (previous.applied) {
          if (Patch(previous, replacement, previous.original))
            previous.applied = false;
          else
            rollback = false;
        }
      }
      if (rollback)
        imports.erase(it);
      else
        *handle = id; // retain ownership for a retry of DobbyDestroyImport
      return DobbyFailure(rollback ? cause : DOBBY_ERROR_ROLLBACK_FAILED, system_error);
    }
    slot.applied = true;
    if (DobbyGetLastError() != DOBBY_ERROR_NONE) {
      warning = DobbyGetLastError();
      warning_errno = DobbyGetLastSystemError();
    }
  }
  *handle = id;
  DobbySetError(warning, warning_errno);
  return RS_SUCCESS;
}
PUBLIC int DobbyGetImportOriginal(DobbyImportHandle handle, void **original) {
  DobbySetError(DOBBY_ERROR_NONE);
  HookOperationGuard guard;
  if (!original)
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  auto it = imports.find(handle);
  if (it == imports.end())
    return DobbyFailure(DOBBY_ERROR_NOT_FOUND);
  *original = it->second.slots.front().original;
  return DobbySuccess();
}
PUBLIC int DobbyDestroyImport(DobbyImportHandle handle) {
  DobbySetError(DOBBY_ERROR_NONE);
  std::shared_ptr<Module> module;
  DobbyError warning = DOBBY_ERROR_NONE;
  int warning_errno = 0;
  {
    HookOperationGuard guard;
    auto it = imports.find(handle);
    if (it == imports.end())
      return DobbyFailure(DOBBY_ERROR_NOT_FOUND);
    for (auto &slot : it->second.slots) {
      if (!slot.applied)
        continue;
      if (!Patch(slot, it->second.replacement, slot.original))
        return RS_FAILED;
      slot.applied = false;
      if (DobbyGetLastError() != DOBBY_ERROR_NONE) {
        warning = DobbyGetLastError();
        warning_errno = DobbyGetLastSystemError();
      }
    }
    module = std::move(it->second.module);
    imports.erase(it);
  }
  module.reset();
  DobbySetError(warning, warning_errno);
  return RS_SUCCESS;
}
