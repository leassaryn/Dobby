
#include "dobby_internal.h"
#include "core/arch/Cpu.h"

#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <memory>
#include <new>
#include <errno.h>

#if !defined(__APPLE__)

// Query the page-aligned protection of `address` from /proc/self/maps.
// Returns -1 if unavailable; PROT_NONE (zero) is a valid mapped permission.
static int GetPageProtection(uintptr_t address) {
  int prot = -1;
#if defined(__aarch64__)
  FILE *fp = fopen("/proc/self/smaps", "r");
#else
  FILE *fp = fopen("/proc/self/maps", "r");
#endif
  if (fp == NULL)
    return -1;

  char line[512];
  while (fgets(line, sizeof(line), fp) != NULL) {
#if defined(__aarch64__)
    if (prot >= 0 && strncmp(line, "VmFlags:", 8) == 0) {
#ifdef PROT_BTI
      if (strstr(line, " bt ") || strstr(line, " bt\n"))
        prot |= PROT_BTI;
#endif
#ifdef PROT_MTE
      if (strstr(line, " mt ") || strstr(line, " mt\n"))
        prot |= PROT_MTE;
#endif
      break;
    }
#endif
    uintptr_t start, end;
    char perms[5] = {0};
    if (sscanf(line, "%" PRIxPTR "-%" PRIxPTR " %4c", &start, &end, perms) != 3)
      continue;
    if (address < start || address >= end)
      continue;

    prot = PROT_NONE;
    if (perms[0] == 'r')
      prot |= PROT_READ;
    if (perms[1] == 'w')
      prot |= PROT_WRITE;
    if (perms[2] == 'x')
      prot |= PROT_EXEC;
#if !defined(__aarch64__)
    break;
#endif
  }
  fclose(fp);
  return prot;
}

bool DobbyIsBtiGuarded(void *address) {
#if defined(__aarch64__) && defined(PROT_BTI)
  const int protection = GetPageProtection(reinterpret_cast<uintptr_t>(address));
  return protection >= 0 && (protection & PROT_BTI) != 0;
#else
  return false;
#endif
}

PUBLIC MemoryOperationError CodePatch(void *address, uint8_t *buffer, uint32_t buffer_size) {
  DobbySetError(DOBBY_ERROR_NONE);
  if (address == nullptr || buffer == nullptr || buffer_size == 0 ||
      reinterpret_cast<uintptr_t>(address) > UINTPTR_MAX - buffer_size) {
    DobbySetError(DOBBY_ERROR_INVALID_ARGUMENT);
    return kMemoryOperationError;
  }

  int page_size = (int)sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    DobbySetError(DOBBY_ERROR_PROTECTION_FAILED, errno);
    return kMemoryOperationError;
  }
  uintptr_t patch_start = (uintptr_t)address;
  uintptr_t patch_end = patch_start + buffer_size;
  uintptr_t page_align_address = ALIGN_FLOOR(address, page_size);
  uintptr_t page_align_end = ALIGN_FLOOR((void *)(patch_end - 1), page_size);
  const size_t page_count = (page_align_end - page_align_address) / page_size + 1;
  std::unique_ptr<int[]> permissions(new (std::nothrow) int[page_count]);
  if (!permissions) {
    DobbySetError(DOBBY_ERROR_ALLOCATION_FAILED);
    return kMemoryOperationError;
  }

  // Capture every page before changing any of them. Adjacent pages may belong
  // to different mappings and must not inherit the first page's permissions.
  for (size_t i = 0; i < page_count; ++i) {
    permissions[i] = GetPageProtection(page_align_address + i * page_size);
    if (permissions[i] < 0) {
      DobbySetError(DOBBY_ERROR_PROTECTION_FAILED, errno);
      return kMemoryOperationError;
    }
  }
  auto RestorePermissions = [&](size_t count) {
    for (size_t i = 0; i < count; ++i) {
      if (mprotect(reinterpret_cast<void *>(page_align_address + i * page_size), page_size, permissions[i]) != 0) {
        DobbySetError(DOBBY_ERROR_PROTECTION_RESTORE_FAILED, errno);
        ERROR_LOG("failed to restore page permissions at %p",
                  reinterpret_cast<void *>(page_align_address + i * page_size));
      }
    }
  };

  for (size_t i = 0; i < page_count; ++i) {
    if (mprotect(reinterpret_cast<void *>(page_align_address + i * page_size), page_size,
                 permissions[i] | PROT_READ | PROT_WRITE) != 0) {
      const int system_error = errno;
      RestorePermissions(i);
      DobbySetError(DOBBY_ERROR_PROTECTION_FAILED, system_error);
      return kMemoryOperationError;
    }
  }

  memcpy(address, buffer, buffer_size);
  ClearCache(address, reinterpret_cast<void *>(patch_end));
  RestorePermissions(page_count);
  // Once bytes are applied, report that fact even if permission restoration
  // fails. Callers must retain the live hook's metadata rather than free it.

  return kMemoryOperationSuccess;
}

#endif
