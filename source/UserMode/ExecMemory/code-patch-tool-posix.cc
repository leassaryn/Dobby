
#include "dobby_internal.h"
#include "core/arch/Cpu.h"

#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#if !defined(__APPLE__)

// Query the page-aligned protection of `address` from /proc/self/maps.
// Returns 0 on failure, otherwise one of the PROT_* flags (or -1 if a
// malformed entry was matched, treated as "keep default").
static int GetPageProtection(uintptr_t address) {
  int prot = 0;
  FILE *fp = fopen("/proc/self/maps", "r");
  if (fp == NULL)
    return 0;

  char line[512];
  while (fgets(line, sizeof(line), fp) != NULL) {
    uintptr_t start, end;
    char perms[5] = {0};
    if (sscanf(line, "%" PRIxPTR "-%" PRIxPTR " %4c", &start, &end, perms) != 3)
      continue;
    if (address < start || address >= end)
      continue;

    if (perms[0] == 'r')
      prot |= PROT_READ;
    if (perms[1] == 'w')
      prot |= PROT_WRITE;
    if (perms[2] == 'x')
      prot |= PROT_EXEC;
    break;
  }
  fclose(fp);
  return prot;
}

PUBLIC MemoryOperationError CodePatch(void *address, uint8_t *buffer, uint32_t buffer_size) {

  int page_size = (int)sysconf(_SC_PAGESIZE);
  uintptr_t patch_start = (uintptr_t)address;
  uintptr_t patch_end = patch_start + buffer_size;
  uintptr_t page_align_address = ALIGN_FLOOR(address, page_size);
  uintptr_t page_align_end = ALIGN_FLOOR((void *)(patch_end - 1), page_size);
  size_t protect_size = page_align_end - page_align_address + page_size;
  int offset = patch_start - page_align_address;

#if defined(__ANDROID__) || defined(__linux__)

  // preserve the original page permission and restore it afterwards,
  // instead of unconditionally forcing RX (which could break pages that
  // were originally mapped non-executable or writable-only).
  int original_prot = GetPageProtection(page_align_address);
  int tmp_prot = PROT_READ | PROT_WRITE | PROT_EXEC;
  if (original_prot == 0)
    original_prot = PROT_READ | PROT_EXEC;

  if (mprotect((void *)page_align_address, protect_size, tmp_prot) != 0) {
    return kMemoryOperationError;
  }

  // patch buffer
  memcpy((void *)((addr_t)page_align_address + offset), buffer, buffer_size);

  // restore page permission
  mprotect((void *)page_align_address, protect_size, original_prot);
#endif

  addr_t clear_start_ = (addr_t)page_align_address + offset;
  ClearCache((void *)clear_start_, (void *)(clear_start_ + buffer_size));

  return kMemoryOperationSuccess;
}

#endif
