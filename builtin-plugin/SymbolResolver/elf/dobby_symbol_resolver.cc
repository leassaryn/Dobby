#include "SymbolResolver/dobby_symbol_resolver.h"
#include "dobby.h"
#include "DobbyError.h"
#include "common_header.h"
#include "HookLifecycle.h"
#include "PluginSupport/ElfSymbols.h"

#include <elf.h>
#include <dlfcn.h>
#include <link.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {
struct Image {
  uintptr_t base;
  std::string path;
  std::vector<ElfW(Phdr)> headers;
  bool Contains(uintptr_t address) const {
#if defined(__arm__)
    address &= ~uintptr_t{1};
#endif
    for (const auto &header : headers)
      if (header.p_type == PT_LOAD && header.p_vaddr <= UINTPTR_MAX - base && address >= base + header.p_vaddr &&
          address - (base + header.p_vaddr) < header.p_memsz)
        return true;
    return false;
  }
};
std::vector<Image> Images() {
  std::vector<Image> images;
  dl_iterate_phdr(
      [](dl_phdr_info *info, size_t, void *context) {
        if (info->dlpi_name && *info->dlpi_name)
          static_cast<std::vector<Image> *>(context)->push_back(
              {info->dlpi_addr, info->dlpi_name, {info->dlpi_phdr, info->dlpi_phdr + info->dlpi_phnum}});
        return 0;
      },
      &images);
  return images;
}
bool Match(const std::string &name, const std::string &path) {
  const auto slash = path.find_last_of('/');
  return name == path ||
         (name.find('/') == std::string::npos && name == path.substr(slash == std::string::npos ? 0 : slash + 1));
}
void *FileSymbol(const Image &image, const char *name) {
  const int fd = open(image.path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return nullptr;
  struct Close {
    int fd;
    ~Close() {
      close(fd);
    }
  } close_fd{fd};
  struct stat status{};
  // Reading into owned storage also avoids SIGBUS if the backing file is truncated.
  if (fstat(fd, &status) || status.st_size <= 0 || uint64_t(status.st_size) > 256 * 1024 * 1024)
    return nullptr;
  std::vector<uint8_t> bytes(static_cast<size_t>(status.st_size));
  size_t cursor = 0;
  while (cursor < bytes.size()) {
    const auto length = read(fd, bytes.data() + cursor, bytes.size() - cursor);
    if (length < 0 && errno == EINTR)
      continue;
    if (length <= 0)
      return nullptr;
    cursor += static_cast<size_t>(length);
  }
  uintptr_t offset = 0;
  if (!dobby::FindFileSymbol(bytes.data(), bytes.size(), name, offset) || offset > UINTPTR_MAX - image.base)
    return nullptr;
  const auto address = image.base + offset;
  return image.Contains(address) ? reinterpret_cast<void *>(address) : nullptr;
}
void *Resolve(const char *image_name, const char *symbol_name, bool exported) {
  if (!symbol_name || !*symbol_name || (image_name && !*image_name)) {
    DobbySetError(DOBBY_ERROR_INVALID_ARGUMENT);
    return nullptr;
  }
  InternalLoaderScope scope;
  if (!image_name && exported) {
    if (void *result = dlsym(RTLD_DEFAULT, symbol_name)) {
      DobbySetError(DOBBY_ERROR_NONE);
      return result;
    }
  }
  std::string selected = image_name ? image_name : "";
  if (image_name && strchr(image_name, '/')) {
    std::unique_ptr<char, decltype(&free)> canonical(realpath(image_name, nullptr), &free);
    if (canonical)
      selected = canonical.get();
  }
  const auto images = Images();
  size_t matches = 0;
  if (image_name) {
    for (const auto &image : images)
      matches += Match(selected, image.path);
    if (matches != 1) {
      DobbySetError(matches ? DOBBY_ERROR_CONFLICT : DOBBY_ERROR_NOT_FOUND);
      return nullptr;
    }
  }
  for (const auto &snapshot : images) {
    auto image = snapshot;
    if (image_name && !Match(selected, image.path))
      continue;
    void *pin = dlopen(image.path.c_str(), RTLD_NOW | RTLD_NOLOAD);
    struct Close {
      void *pin;
      ~Close() {
        if (pin)
          dlclose(pin);
      }
    } close_pin{pin};
    // The linker is process-lifetime but is not a dlopen-able shared library.
    const bool linker = image.path == "/system/bin/linker" || image.path == "/system/bin/linker64" ||
                        image.path == "/apex/com.android.runtime/bin/linker" ||
                        image.path == "/apex/com.android.runtime/bin/linker64";
    if (!pin && !linker)
      continue;
    if (pin) {
      bool refreshed = false;
      for (const auto &current : Images())
        if (current.path == image.path) {
          image = current;
          refreshed = true;
          break;
        }
      if (!refreshed)
        continue;
    }
    if (pin && exported) {
      if (void *result = dlsym(pin, symbol_name); result && image.Contains(reinterpret_cast<uintptr_t>(result))) {
        DobbySetError(DOBBY_ERROR_NONE);
        return result;
      }
    }
    if (void *result = FileSymbol(image, symbol_name)) {
      DobbySetError(DOBBY_ERROR_NONE);
      return result;
    }
  }
  DobbySetError(DOBBY_ERROR_NOT_FOUND);
  return nullptr;
}
} // namespace
void *resolve_elf_internal_symbol(const char *image, const char *symbol) {
  return Resolve(image, symbol, false);
}
PUBLIC void *DobbySymbolResolver(const char *image, const char *symbol) {
  return Resolve(image, symbol, true);
}
PUBLIC int DobbyHookByName(const char *image, const char *symbol, void *replacement, void **original) {
  if (!replacement)
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  void *address = DobbySymbolResolver(image, symbol);
  return address ? DobbyHook(address, replacement, original) : RS_FAILED;
}
