#pragma once
#include "dobby_internal.h"
#include "UserMode/PlatformUtil/ProcessRuntimeUtility.h"
#include <map>
#include <vector>
#include <limits>

// Shared by the production IA32 and x64 relocation backends.
struct X86RelocationFixups {
  struct Entry {
    size_t displacement;
    size_t next;
    uint64_t target;
    bool branch;
  };
  std::map<uint64_t, size_t> offsets;
  std::vector<Entry> entries;
  std::vector<MemoryRegion> regions = ProcessRuntimeUtility::GetProcessMemoryLayout();
  size_t Readable(uintptr_t address) const {
    for (const auto &region : regions) {
      uintptr_t start = reinterpret_cast<uintptr_t>(region.address);
      if (region.permission == kNoAccess || address < start || address - start >= region.length)
        continue;
      return std::min(size_t{15}, region.length - (address - start));
    }
    return 0;
  }
  bool Resolve(CodeBufferBase *buffer, uint64_t origin, uint64_t end, uint64_t relocated, bool wide) {
    for (const auto &entry : entries) {
      uint64_t target = entry.target;
      if (target >= origin && target < end) {
        auto it = offsets.find(target);
        if (!entry.branch || it == offsets.end()) {
          DobbySetError(DOBBY_ERROR_UNSUPPORTED_INSTRUCTION);
          return false;
        }
        target = relocated + it->second;
      }
      const int64_t delta = static_cast<int64_t>(target) - static_cast<int64_t>(relocated + entry.next);
      if (wide && (delta < INT32_MIN || delta > INT32_MAX)) {
        DobbySetError(DOBBY_ERROR_NEAR_ALLOCATION_FAILED);
        return false;
      }
      const uint32_t encoded = static_cast<uint32_t>(delta);
      memcpy(static_cast<uint8_t *>(buffer->getRawBuffer()) + entry.displacement, &encoded, sizeof(encoded));
    }
    return true;
  }
};
