#ifndef DOBBY_INTERNAL_H
#define DOBBY_INTERNAL_H

#include "dobby.h"
#include "DobbyError.h"

#include "logging/logging.h"
#include "logging/check_logging.h"

#include "xnucxx/LiteMemOpt.h"
#include "xnucxx/LiteMutableArray.h"
#include "xnucxx/LiteMutableBuffer.h"
#include "xnucxx/LiteIterator.h"

#include "UnifiedInterface/platform.h"

#include "PlatformUnifiedInterface/StdMemory.h"
#include "PlatformUnifiedInterface/ExecMemory/CodePatchTool.h"
#include "PlatformUnifiedInterface/ExecMemory/ClearCacheTool.h"

#include "MemoryAllocator/MemoryArena.h"
#include "MemoryAllocator/AssemblyCodeBuilder.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

bool DobbyInPatchCoordinator();
bool DobbyCoordinatorUsesRange(uintptr_t start, size_t size);

typedef struct _AssemblyCodeChunkBuffer {
  AssemblyCodeChunk chunk;
  uint8_t chunk_buffer[64];
} AssemblyCodeChunkBuffer;

typedef enum { kFunctionWrapper, kFunctionInlineHook, kDynamicBinaryInstrument } HookEntryType;

typedef struct HookEntry {
  int id;
  int type;

  union {
    void *target_address;
    void *function_address;
    void *instruction_address;
  };

  void *route;
  void *patch_address;
  bool needs_bti_prefix;
  int near_policy; // -1: legacy global policy
  size_t max_patch_bytes;

  // fixed-instructions which we relocated(patched)
  union {
    void *relocated_origin_instructions;
    void *relocated_origin_function;
  };

  bool is_committed;
  HookEntry *retired_next;

  AssemblyCodeChunkBuffer origin_chunk_;
} HookEntry;

#endif