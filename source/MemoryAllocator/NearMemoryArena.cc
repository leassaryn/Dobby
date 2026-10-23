#include "./NearMemoryArena.h"

#include "dobby_internal.h"

#include "UserMode/PlatformUtil/ProcessRuntimeUtility.h"

#include <iostream>
#include <vector>

#if defined(__linux__) || defined(__APPLE__) || defined(__ANDROID__)
#include <pthread.h>
#endif

using namespace zz;

#define KB (1024uLL)
#define MB (1024uLL * KB)
#define GB (1024uLL * MB)

LiteMutableArray *NearMemoryArena::page_chunks;
pthread_mutex_t NearMemoryArena::lock_ = PTHREAD_MUTEX_INITIALIZER;

void NearMemoryArena::Lock() {
  pthread_mutex_lock(&lock_);
}

void NearMemoryArena::Unlock() {
  pthread_mutex_unlock(&lock_);
}

static addr_t search_near_blank_page(addr_t position, size_t range, MemoryPermission permission) {
  const addr_t page_size = OSMemory::PageSize();
  const addr_t lower = position > range ? position - range : page_size;
  const addr_t upper = position < UINTPTR_MAX - range ? position + range : UINTPTR_MAX;
  const addr_t low = (lower + page_size - 1) & ~(page_size - 1);
  const addr_t high = upper & ~(page_size - 1);
  if (low >= high)
    return 0;

  const auto regions = ProcessRuntimeUtility::GetProcessMemoryLayout();
  if (regions.empty())
    return 0;
  auto TryGap = [&](addr_t begin, addr_t end) -> addr_t {
    if (begin >= end || end - begin < page_size)
      return 0;
    addr_t hint = position & ~(page_size - 1);
    if (hint < begin)
      hint = begin;
    if (hint > end - page_size)
      hint = end - page_size;
    void *memory = OSMemory::Allocate(reinterpret_cast<void *>(hint), page_size, permission);
    const addr_t actual = reinterpret_cast<addr_t>(memory);
    if (memory && (actual < low || actual > high - page_size)) {
      OSMemory::Free(memory, page_size);
      return 0;
    }
    return actual;
  };
  addr_t cursor = low;
  for (const auto &region : regions) {
    const addr_t begin = reinterpret_cast<addr_t>(region.address);
    const addr_t end = begin + region.length;
    if (end <= cursor)
      continue;
    if (begin > cursor) {
      if (addr_t result = TryGap(cursor, begin < high ? begin : high))
        return result;
    }
    if (end >= high)
      return 0;
    cursor = end > cursor ? end : cursor;
  }
  return TryGap(cursor, high);
}

void NearMemoryArena::Destroy(MemoryChunk *chunk) {
  // Executable storage may still be in use. Only its caller-owned descriptor is released.
  delete chunk;
}

NearMemoryArena::NearMemoryArena() {
}

#define NEAR_PAGE_ARRAYLEN 8

int NearMemoryArena::PushPage(addr_t page_addr, MemoryPermission permission) {
  PageChunk *newPage = new PageChunk;
  newPage->page.address = (void *)page_addr;
  newPage->page.length = OSMemory::PageSize();
  newPage->page_cursor = page_addr;
  newPage->permission = permission;
  NearMemoryArena::page_chunks->pushObject(reinterpret_cast<LiteObject *>(newPage));
  return RT_SUCCESS;
}

WritableDataChunk *NearMemoryArena::AllocateDataChunk(addr_t position, size_t alloc_range, int alloc_size) {
  return NearMemoryArena::AllocateChunk(position, alloc_range, alloc_size, kReadWrite);
}

AssemblyCodeChunk *NearMemoryArena::AllocateCodeChunk(addr_t position, size_t alloc_range, int alloc_size) {
  return NearMemoryArena::AllocateChunk(position, alloc_range, alloc_size, kReadExecute);
}

MemoryChunk *NearMemoryArena::AllocateChunk(addr_t position, size_t alloc_range, int alloc_size,
                                            MemoryPermission permission) {

  if (alloc_size <= 0 || alloc_size > OSMemory::PageSize() || alloc_range == 0)
    return nullptr;
  Lock();

  if (page_chunks == NULL) {
    page_chunks = new LiteMutableArray(NEAR_PAGE_ARRAYLEN);
  }

search_once_more:
  LiteCollectionIterator iter(NearMemoryArena::page_chunks);
  PageChunk *page = NULL;
  while ((page = reinterpret_cast<PageChunk *>(iter.getNextObject())) != NULL) {
    if (page->permission == permission) {
      if (llabs((intptr_t)(page->page_cursor - position)) < alloc_range) {
        if ((page->page_cursor + alloc_size) <= ((addr_t)page->page.address + page->page.length)) {
          break;
        }
      }
    }
  }

  MemoryChunk *chunk = NULL;
  if (page) {
    chunk = new MemoryChunk;
    chunk->address = (void *)page->page_cursor;
    chunk->length = alloc_size;

    // update page cursor
    page->page_cursor += alloc_size;
    Unlock();
    return chunk;
  }

  addr_t blank_page_addr = 0;
  blank_page_addr = search_near_blank_page(position, alloc_range, permission);
  if (blank_page_addr) {
    NearMemoryArena::PushPage(blank_page_addr, permission);
    goto search_once_more;
  }

  // Zero-filled bytes in somebody else's mapping are not free storage. If no
  // owned page is available, let the routing layer fail or select a safe fallback.
  Unlock();
  return NULL;
}

size_t NearMemoryArena::RetainedBytes() {
  Lock();
  size_t bytes = 0;
  if (page_chunks) {
    LiteCollectionIterator iterator(page_chunks);
    while (auto *page = reinterpret_cast<PageChunk *>(iterator.getNextObject()))
      bytes += page->page.length;
  }
  Unlock();
  return bytes;
}

bool NearMemoryArena::ReleaseAll(size_t &released_bytes) {
  Lock();
  if (!page_chunks) {
    Unlock();
    return true;
  }
  auto *remaining = new LiteMutableArray(8);
  LiteCollectionIterator iterator(page_chunks);
  while (auto *page = reinterpret_cast<PageChunk *>(iterator.getNextObject())) {
    if (OSMemory::Free(page->page.address, page->page.length)) {
      released_bytes += page->page.length;
      delete page;
    } else {
      remaining->pushObject(reinterpret_cast<LiteObject *>(page));
    }
  }
  delete page_chunks;
  page_chunks = remaining;
  const bool success = remaining->getCount() == 0;
  if (success) {
    delete page_chunks;
    page_chunks = nullptr;
  }
  Unlock();
  return success;
}
