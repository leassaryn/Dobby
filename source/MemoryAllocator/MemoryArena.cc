#include "MemoryAllocator/MemoryArena.h"

#include "dobby_internal.h"

#if defined(__linux__) || defined(__APPLE__) || defined(__ANDROID__)
#include <pthread.h>
#endif

LiteMutableArray *MemoryArena::page_chunks = NULL;
void *MemoryArena::lock_ = NULL;

void MemoryArena::Lock() {
#if defined(__linux__) || defined(__APPLE__) || defined(__ANDROID__)
  if (lock_ == NULL) {
    pthread_mutex_t *mutex = new pthread_mutex_t;
    pthread_mutex_init(mutex, NULL);
    lock_ = mutex;
  }
  pthread_mutex_lock((pthread_mutex_t *)lock_);
#endif
}

void MemoryArena::Unlock() {
#if defined(__linux__) || defined(__APPLE__) || defined(__ANDROID__)
  if (lock_) {
    pthread_mutex_unlock((pthread_mutex_t *)lock_);
  }
#endif
}

void MemoryArena::Destroy(AssemblyCodeChunk *cchunk) {
  return;
}

MemoryChunk *MemoryArena::AllocateChunk(int alloc_size, MemoryPermission permission) {
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
      if ((page->page_cursor + alloc_size) < ((addr_t)page->page.address + page->page.length)) {
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
      Unlock();
      return NULL;
    }

    PageChunk *newPage = new PageChunk;
    newPage->page.address = pageAddress;
    newPage->page.length = pageSize;
    newPage->page_cursor = (addr_t)pageAddress;
    newPage->permission = permission;
    newPage->chunks = new LiteMutableArray(8);
    MemoryArena::page_chunks->pushObject(reinterpret_cast<LiteObject *>(newPage));
    page = newPage;
  }

  MemoryChunk *chunk = NULL;
  if (page) {
    chunk = new MemoryChunk;
    chunk->address = (void *)page->page_cursor;
    chunk->length = alloc_size;

    // update page cursor
    page->chunks->pushObject(reinterpret_cast<LiteObject *>(chunk));
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
