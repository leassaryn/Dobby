#ifndef INTERCEPTOR_H
#define INTERCEPTOR_H

#include "dobby_internal.h"

#include "include/list_structure.h"

typedef struct {
  struct list_head list_node;
  HookEntry *entry;
} HookEntryNode;

// Callback invoked for each active hook entry. Return 0 to continue, non-zero
// to stop iteration early.
typedef int (*HookEntryIterator)(void *hook_address, void *userdata);

class Interceptor {
public:
  static Interceptor *SharedInstance();

  static Interceptor *SharedInstanceOriginal();

  HookEntry *FindHookEntry(void *address);

  void AddHookEntry(HookEntry *entry);

  void RemoveHookEntry(void *address);

  int GetHookEntryCount();

  // Invoke `cb` for every registered hook entry while holding the lock.
  void Iterate(HookEntryIterator cb, void *userdata);

  // Protect concurrent hook/destroy from racing on the hook-entry list.
  void Lock();

  void Unlock();

private:
  Interceptor() {
    lock_ = NULL;
  }

  HookEntryNode *find_hook_entry_node(void *address);

private:
  struct list_head hook_entry_list_;

  void *lock_;

  static Interceptor *priv_interceptor_;

  static Interceptor *priv_orig_interceptor_;
};

#endif
