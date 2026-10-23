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
  uint32_t type = 0;
  uint16_t version = 0;
};
struct Import {
  std::shared_ptr<Module> module;
  void *replacement;
  std::vector<Slot> slots;
};
std::unordered_map<uint64_t, std::shared_ptr<Module>> modules;
std::unordered_map<uint64_t, Import> imports;
uint64_t next_module = 1, next_import = 1;

bool Slots(const Module &module, const char *name, const char *version, uint32_t flags, std::vector<Slot> &slots) {
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
  uintptr_t rel = 0, rela = 0, versym = 0, verneed = 0;
  size_t rel_size = 0, rela_size = 0, verneed_count = 0;
  size_t relent = sizeof(ElfW(Rel)), relaent = sizeof(ElfW(Rela));
  bool terminated = false;
  for (size_t i = 0; i < count; ++i) {
    const auto &d = dynamic[i];
    if (d.d_tag == DT_NULL) {
      terminated = true;
      break;
    }
    switch (d.d_tag) {
    case DT_REL:
      rel = d.d_un.d_ptr;
      break;
    case DT_RELSZ:
      rel_size = d.d_un.d_val;
      break;
    case DT_RELENT:
      relent = d.d_un.d_val;
      break;
    case DT_RELA:
      rela = d.d_un.d_ptr;
      break;
    case DT_RELASZ:
      rela_size = d.d_un.d_val;
      break;
    case DT_RELAENT:
      relaent = d.d_un.d_val;
      break;
    case DT_VERSYM:
      versym = d.d_un.d_ptr;
      break;
    case DT_VERNEED:
      verneed = d.d_un.d_ptr;
      break;
    case DT_VERNEEDNUM:
      verneed_count = d.d_un.d_val;
      break;
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
  if (!terminated || symbol_size != sizeof(ElfW(Sym)))
    return false;
  strings = module.Pointer(strings, string_size);
  symbols = module.Pointer(symbols, sizeof(ElfW(Sym)));
  if (!strings || !symbols || !string_size)
    return false;
  std::unordered_map<uint16_t, std::string> versions;
  if (versym) {
    versym = module.Pointer(versym, sizeof(ElfW(Half)));
    if (!versym)
      return false;
  }
  if (verneed_count) {
    if (verneed_count > 65535)
      return false;
    uintptr_t need = module.Pointer(verneed, sizeof(ElfW(Verneed)));
    for (size_t n = 0; n < verneed_count; ++n) {
      if (!module.Contains(need, sizeof(ElfW(Verneed))))
        return false;
      const auto &v = *reinterpret_cast<const ElfW(Verneed) *>(need);
      if (v.vn_version != 1 || v.vn_aux > UINTPTR_MAX - need)
        return false;
      uintptr_t aux = need + v.vn_aux;
      for (size_t k = 0; k < v.vn_cnt; ++k) {
        if (!module.Contains(aux, sizeof(ElfW(Vernaux))))
          return false;
        const auto &a = *reinterpret_cast<const ElfW(Vernaux) *>(aux);
        if (a.vna_name >= string_size)
          return false;
        const char *name = reinterpret_cast<const char *>(strings + a.vna_name);
        if (!memchr(name, 0, string_size - a.vna_name))
          return false;
        versions[a.vna_other & 0x7fff] = name;
        if (k + 1 < v.vn_cnt && (!a.vna_next || a.vna_next > UINTPTR_MAX - aux))
          return false;
        aux += a.vna_next;
      }
      if (n + 1 < verneed_count && (!v.vn_next || v.vn_next > UINTPTR_MAX - need))
        return false;
      need += v.vn_next;
    }
  }
  struct Table {
    uintptr_t address;
    size_t size;
    bool rela;
    bool plt;
  };
  std::vector<Table> tables;
  if ((flags & DOBBY_IMPORT_PLT) && relocation_size) {
    if (kind != DT_REL && kind != DT_RELA)
      return false;
    tables.push_back({relocations, relocation_size, kind == DT_RELA, true});
  }
  if (flags & DOBBY_IMPORT_GLOB_DAT) {
    if (rel_size) {
      if (relent != sizeof(ElfW(Rel)))
        return false;
      tables.push_back({rel, rel_size, false, false});
    }
    if (rela_size) {
      if (relaent != sizeof(ElfW(Rela)))
        return false;
      tables.push_back({rela, rela_size, true, false});
    }
  }
  for (auto table : tables) {
    const size_t stride = table.rela ? sizeof(ElfW(Rela)) : sizeof(ElfW(Rel));
    relocations = module.Pointer(table.address, table.size);
    if (!relocations || table.size % stride)
      return false;
    for (size_t i = 0; i < table.size / stride; ++i) {
      ElfW(Addr) offset;
      ElfW(Xword) info;
      if (table.rela) {
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
      constexpr auto jump_slot = R_AARCH64_JUMP_SLOT, glob_dat = R_AARCH64_GLOB_DAT;
#elif defined(__arm__)
      constexpr auto jump_slot = R_ARM_JUMP_SLOT, glob_dat = R_ARM_GLOB_DAT;
#elif defined(__x86_64__)
      constexpr auto jump_slot = R_X86_64_JUMP_SLOT, glob_dat = R_X86_64_GLOB_DAT;
#else
      constexpr auto jump_slot = R_386_JMP_SLOT, glob_dat = R_386_GLOB_DAT;
#endif
      if (type != (table.plt ? jump_slot : glob_dat))
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
      uint16_t version_index = 0;
      if (versym) {
        if (symbol_index > (UINTPTR_MAX - versym) / sizeof(ElfW(Half)))
          return false;
        const auto vaddr = versym + symbol_index * sizeof(ElfW(Half));
        if (!module.Contains(vaddr, sizeof(ElfW(Half))))
          return false;
        version_index = *reinterpret_cast<const ElfW(Half) *>(vaddr) & 0x7fff;
      }
      if (version) {
        const auto v = versions.find(version_index);
        if (v == versions.end() || v->second != version)
          continue;
      }
      if (offset > UINTPTR_MAX - module.base)
        return false;
      auto *slot = reinterpret_cast<void **>(module.base + offset);
      if (reinterpret_cast<uintptr_t>(slot) % alignof(void *) ||
          !module.Contains(reinterpret_cast<uintptr_t>(slot), sizeof(void *)))
        return false;
      if (std::any_of(slots.begin(), slots.end(), [&](const Slot &s) { return s.address == slot; }))
        continue;
      slots.push_back(
          {slot, __atomic_load_n(slot, __ATOMIC_ACQUIRE), false, static_cast<uint32_t>(type), version_index});
    }
  }
  return true;
}

bool Patch(Slot &slot, void *expected, void *value) {
  PatchScope patch(slot.address, sizeof(void *), DOBBY_PATCH_IMPORT);
  if (!patch.ready())
    return false;
  if (__atomic_load_n(slot.address, __ATOMIC_ACQUIRE) != expected) {
    DobbySetError(DOBBY_ERROR_CONFLICT);
    return false;
  }
  // Management is serialized; caller must keep importing code quiescent.
  const bool success =
      CodePatch(slot.address, reinterpret_cast<uint8_t *>(&value), sizeof(value)) == kMemoryOperationSuccess;
  if (success)
    patch.Applied();
  return success;
}
} // namespace

PUBLIC int DobbyAcquireModule(const char *name, DobbyModuleHandle *handle) {
  if (DobbyInPatchCoordinator())
    return DobbyFailure(DOBBY_ERROR_BUSY);
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
  if (DobbyInPatchCoordinator())
    return DobbyFailure(DOBBY_ERROR_BUSY);
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
    for (const auto &h : it->second->headers)
      if (h.p_type == PT_LOAD && DobbyCoordinatorUsesRange(it->second->base + h.p_vaddr, h.p_memsz))
        check.busy = true;
    if (check.busy)
      return DobbyFailure(DOBBY_ERROR_BUSY);
    module = std::move(it->second);
    modules.erase(it);
  }
  module.reset();
  return DobbySuccess();
}
PUBLIC int DobbyHookImportEx(const char *name, const char *symbol, void *replacement, const DobbyImportOptions *options,
                             DobbyImportHandle *handle) {
  if (DobbyInPatchCoordinator())
    return DobbyFailure(DOBBY_ERROR_BUSY);
  DobbySetError(DOBBY_ERROR_NONE);
  if (!symbol || !*symbol || !replacement || !handle)
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  *handle = 0;
  if (options && (options->struct_size < sizeof(*options) || options->api_version != DOBBY_API_VERSION ||
                  !options->flags || (options->flags & ~(DOBBY_IMPORT_PLT | DOBBY_IMPORT_GLOB_DAT)) ||
                  (options->symbol_version && !*options->symbol_version)))
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  auto module = Acquire(name);
  if (!module)
    return RS_FAILED;
  HookOperationGuard guard;
  Import entry{module, replacement, {}};
  if (!Slots(*module, symbol, options ? options->symbol_version : nullptr, options ? options->flags : DOBBY_IMPORT_PLT,
             entry.slots))
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
  if (DobbyInPatchCoordinator())
    return DobbyFailure(DOBBY_ERROR_BUSY);
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

PUBLIC int DobbyHookImport(const char *name, const char *symbol, void *replacement, DobbyImportHandle *handle) {
  return DobbyHookImportEx(name, symbol, replacement, nullptr, handle);
}
PUBLIC int DobbyInspectImport(const char *name, const char *symbol, const DobbyImportOptions *options,
                              DobbyImportSlotInfo *output, size_t capacity, size_t *count) {
  DobbySetError(DOBBY_ERROR_NONE);
  if (!symbol || !*symbol || !count || (!output && capacity) ||
      (options && (options->struct_size < sizeof(*options) || options->api_version != DOBBY_API_VERSION ||
                   !options->flags || (options->flags & ~(DOBBY_IMPORT_PLT | DOBBY_IMPORT_GLOB_DAT)) ||
                   (options->symbol_version && !*options->symbol_version))))
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  *count = 0;
  auto module = Acquire(name);
  if (!module)
    return RS_FAILED;
  HookOperationGuard guard;
  std::vector<Slot> slots;
  if (!Slots(*module, symbol, options ? options->symbol_version : nullptr, options ? options->flags : DOBBY_IMPORT_PLT,
             slots))
    return DobbyFailure(DOBBY_ERROR_UNSUPPORTED_FORMAT);
  *count = slots.size();
  if (slots.empty())
    return DobbyFailure(DOBBY_ERROR_NOT_FOUND);
  if (!output && !capacity)
    return DobbySuccess();
  if (capacity < slots.size())
    return DobbyFailure(DOBBY_ERROR_PATCH_LIMIT);
  for (size_t i = 0; i < slots.size(); ++i) {
    const auto &slot = slots[i];
    output[i] = {slot.address, slot.original, nullptr, 0, slot.type, slot.version, 0};
    for (const auto &[id, entry] : imports)
      for (const auto &owned : entry.slots)
        if (owned.address == slot.address) {
          output[i].owner = id;
          output[i].original = owned.original;
          output[i].conflict = owned.applied && slot.original != entry.replacement;
        }
  }
  return DobbySuccess();
}
