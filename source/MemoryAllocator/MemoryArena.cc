#include "MemoryAllocator/MemoryArena.h"

#include "dobby_internal.h"

#if defined(__linux__) || defined(__APPLE__) || defined(__ANDROID__)
#include <pthread.h>
#endif

LiteMutableArray *MemoryArena::page_chunks = NULL;
pthread_mutex_t MemoryArena::lock_ = PTHREAD_MUTEX_INITIALIZER;

void MemoryArena::Lock() {
  pthread_mutex_lock(&lock_);
}

void MemoryArena::Unlock() {
  pthread_mutex_unlock(&lock_);
}

void MemoryArena::Destroy(AssemblyCodeChunk *cchunk) {
  delete cchunk;
}

MemoryChunk *MemoryArena::AllocateChunk(int alloc_size, MemoryPermission permission) {
  if (alloc_size <= 0 || alloc_size > OSMemory::PageSize()) {
    DobbySetError(DOBBY_ERROR_ALLOCATION_FAILED);
    return nullptr;
  }
  MemoryChunk *result = NULL;

  Lock();

  if (page_chunks == NULL) {
    page_chunks = new LiteMutableArray(8);
  }

  LiteCollectionIterator iter(page_chunks);
  PageChunk *page = NULL;
  while ((page = reinterpret_cast<PageChunk *>(iter.getNextObject())) != NULL) {
    if (page->permission == permission) {
      // check the page remain space is enough for the new chunk
      if ((page->page_cursor + alloc_size) <= ((addr_t)page->page.address + page->page.length)) {
        break;
      }
    }
  }

  // alloc a new executable page.
  if (!page) {
    int pageSize = OSMemory::PageSize();
    void *pageAddress = OSMemory::Allocate(NULL, pageSize, permission);
    if (pageAddress == NULL) {
      ERROR_LOG("Failed to alloc page");
      DobbySetError(DOBBY_ERROR_ALLOCATION_FAILED);
      Unlock();
      return NULL;
    }

    PageChunk *newPage = new PageChunk;
    newPage->page.address = pageAddress;
    newPage->page.length = pageSize;
    newPage->page_cursor = (addr_t)pageAddress;
    newPage->permission = permission;
    MemoryArena::page_chunks->pushObject(reinterpret_cast<LiteObject *>(newPage));
    page = newPage;
  }

  MemoryChunk *chunk = NULL;
  if (page) {
    chunk = new MemoryChunk;
    chunk->address = (void *)page->page_cursor;
    chunk->length = alloc_size;

    // update page cursor
    page->page_cursor += alloc_size;
  }

  Unlock();

  result = chunk;
  return result;
}

AssemblyCodeChunk *MemoryArena::AllocateCodeChunk(int alloc_size) {
  return MemoryArena::AllocateChunk(alloc_size, kReadExecute);
}

WritableDataChunk *MemoryArena::AllocateDataChunk(int alloc_size) {
  return MemoryArena::AllocateChunk(alloc_size, kReadWrite);
}

// UserMode
// Search code cave from MemoryLayout
// MemoryRegion *CodeChunk::SearchCodeCave(uword pos, uword alloc_range, size_t size) {}

size_t MemoryArena::RetainedBytes() {
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

bool MemoryArena::ReleaseAll(size_t &released_bytes) {
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
