#pragma once
#include <elf.h>
#include <link.h>
#include <cstdint>
#include <cstring>
#include <cstddef>

namespace dobby {
// File offsets are never dereferenced until both the offset and extent are checked.
inline bool FindFileSymbol(const uint8_t *bytes, size_t size, const char *name, uintptr_t &value) {
  if (!bytes || !name || !*name || size < sizeof(ElfW(Ehdr)))
    return false;
  ElfW(Ehdr) header;
  memcpy(&header, bytes, sizeof(header));
  if (memcmp(header.e_ident, ELFMAG, SELFMAG) ||
      header.e_ident[EI_CLASS] != (sizeof(void *) == 8 ? ELFCLASS64 : ELFCLASS32) ||
      header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_ident[EI_VERSION] != EV_CURRENT ||
      (header.e_type != ET_DYN && header.e_type != ET_EXEC) || header.e_shentsize != sizeof(ElfW(Shdr)) ||
      !header.e_shnum)
    return false;
  auto contains = [&](uint64_t offset, uint64_t length) { return offset <= size && length <= size - offset; };
  if (!contains(header.e_shoff, uint64_t(header.e_shnum) * sizeof(ElfW(Shdr))))
    return false;
  auto section = [&](size_t index) {
    ElfW(Shdr) result;
    memcpy(&result, bytes + header.e_shoff + index * sizeof(result), sizeof(result));
    return result;
  };
  for (size_t index = 0; index < header.e_shnum; ++index) {
    const auto symbols = section(index);
    if (symbols.sh_type != SHT_SYMTAB && symbols.sh_type != SHT_DYNSYM)
      continue;
    if (symbols.sh_link >= header.e_shnum || symbols.sh_entsize != sizeof(ElfW(Sym)) ||
        symbols.sh_size % sizeof(ElfW(Sym)) || !contains(symbols.sh_offset, symbols.sh_size))
      return false;
    const auto strings = section(symbols.sh_link);
    if (strings.sh_type != SHT_STRTAB || !contains(strings.sh_offset, strings.sh_size))
      return false;
    for (size_t offset = 0; offset < symbols.sh_size; offset += sizeof(ElfW(Sym))) {
      ElfW(Sym) symbol;
      memcpy(&symbol, bytes + symbols.sh_offset + offset, sizeof(symbol));
      const unsigned type = ELF64_ST_TYPE(symbol.st_info);
      if (symbol.st_shndx == SHN_UNDEF || symbol.st_shndx >= SHN_LORESERVE ||
          (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE))
        continue;
      if (symbol.st_shndx >= header.e_shnum || symbol.st_name >= strings.sh_size)
        return false;
      const char *candidate = reinterpret_cast<const char *>(bytes + strings.sh_offset + symbol.st_name);
      if (!memchr(candidate, 0, strings.sh_size - symbol.st_name))
        return false;
      if (!strcmp(candidate, name)) {
        value = symbol.st_value;
        return true;
      }
    }
  }
  return false;
}
} // namespace dobby
