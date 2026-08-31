#include "Interceptor.h"

#include "dobby_internal.h"

#if defined(__linux__) || defined(__APPLE__) || defined(__ANDROID__)
#include <pthread.h>
#endif

Interceptor *Interceptor::priv_interceptor_ = nullptr;
Interceptor *Interceptor::priv_orig_interceptor_ = nullptr;

Interceptor *Interceptor::SharedInstance() {
  if (Interceptor::priv_interceptor_ == nullptr) {
    Interceptor::priv_interceptor_ = new Interceptor();
    INIT_LIST_HEAD(&Interceptor::priv_interceptor_->hook_entry_list_);
  }
  return Interceptor::priv_interceptor_;
}

Interceptor *Interceptor::SharedInstanceOriginal() {
  if (Interceptor::priv_orig_interceptor_ == nullptr) {
    Interceptor::priv_orig_interceptor_ = new Interceptor();
    INIT_LIST_HEAD(&Interceptor::priv_orig_interceptor_->hook_entry_list_);
  }
  return Interceptor::priv_orig_interceptor_;
}

void Interceptor::Lock() {
#if defined(__linux__) || defined(__APPLE__) || defined(__ANDROID__)
  if (lock_ == NULL) {
    pthread_mutex_t *mutex = new pthread_mutex_t;
    pthread_mutex_init(mutex, NULL);
    lock_ = mutex;
  }
  pthread_mutex_lock((pthread_mutex_t *)lock_);
#endif
}

void Interceptor::Unlock() {
#if defined(__linux__) || defined(__APPLE__) || defined(__ANDROID__)
  if (lock_) {
    pthread_mutex_unlock((pthread_mutex_t *)lock_);
  }
#endif
}

HookEntryNode *Interceptor::find_hook_entry_node(void *address) {
  HookEntryNode *entry_node = nullptr;
#if defined(_MSC_VER)
#if 0 // only valid if offsetof(HookEntryNode, list_node) == 0
  for(entry_node = (HookEntryNode *)hook_entry_list_.next; &entry_node->list_node != &hook_entry_list_; entry_node = (HookEntryNode *)entry_node->list_node.next);
#endif
  struct list_head *list_node = nullptr;
  for (list_node = hook_entry_list_.next; list_node != &hook_entry_list_; list_node = list_node->next) {
    entry_node = (HookEntryNode *)((char *)list_node - offsetof(HookEntryNode, list_node));
#else
  list_for_each_entry(entry_node, &hook_entry_list_, list_node) {
#endif
    HookEntry *entry = entry_node->entry;
    if (entry->instruction_address == address) {
      return entry_node;
    }
  }
  return nullptr;
}

HookEntry *Interceptor::FindHookEntry(void *address) {
  HookEntryNode *entry_node = nullptr;
  Lock();
  entry_node = find_hook_entry_node(address);
  Unlock();
  if (entry_node) {
    return entry_node->entry;
  }

  return nullptr;
}

void Interceptor::AddHookEntry(HookEntry *entry) {
  HookEntryNode *entry_node = new HookEntryNode;
  entry_node->entry = entry;

  Lock();
  list_add(&entry_node->list_node, &hook_entry_list_);
  Unlock();
}

void Interceptor::RemoveHookEntry(void *address) {
  Lock();
  if (HookEntryNode *entry_node = find_hook_entry_node(address)) {
    list_del(&entry_node->list_node);
    delete entry_node;
  }
  Unlock();
}

int Interceptor::GetHookEntryCount() {
  int count = 0;
  Lock();
  HookEntryNode *entry_node = nullptr;
#if defined(_MSC_VER)
  struct list_head *list_node = nullptr;
  for (list_node = hook_entry_list_.next; list_node != &hook_entry_list_; list_node = list_node->next) {
    entry_node = (HookEntryNode *)((char *)list_node - offsetof(HookEntryNode, list_node));
#else
  list_for_each_entry(entry_node, &hook_entry_list_, list_node) {
#endif
    count += 1;
  }
  Unlock();
  return count;
}

void Interceptor::Iterate(HookEntryIterator cb, void *userdata) {
  if (cb == NULL)
    return;

  Lock();
  HookEntryNode *entry_node = nullptr;
  list_for_each_entry(entry_node, &hook_entry_list_, list_node) {
    HookEntry *entry = entry_node->entry;
    if (cb(entry->instruction_address, userdata) != 0)
      break;
  }
  Unlock();
}
