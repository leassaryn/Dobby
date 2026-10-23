
#ifndef MemoryAllocator_NearMemoryArena_h
#define MemoryAllocator_NearMemoryArena_h

#include "MemoryAllocator/MemoryArena.h"

class NearMemoryArena : public MemoryArena {
public:
  NearMemoryArena();

  static MemoryChunk *AllocateChunk(addr_t position, size_t alloc_range, int alloc_size, MemoryPermission permission);

  static WritableDataChunk *AllocateDataChunk(addr_t position, size_t alloc_range, int alloc_size);

  static AssemblyCodeChunk *AllocateCodeChunk(addr_t position, size_t alloc_range, int alloc_size);

  static int PushPage(addr_t page_addr, MemoryPermission permission);

  static void Destroy(MemoryChunk *chunk);
  static size_t RetainedBytes();
  static bool ReleaseAll(size_t &released_bytes);

  // Serialize concurrent allocations on the shared near-page pool.
  static void Lock();

  static void Unlock();

private:
  static LiteMutableArray *page_chunks;

  static pthread_mutex_t lock_;
};

#endif
