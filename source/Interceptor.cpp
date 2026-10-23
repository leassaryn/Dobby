#include "Interceptor.h"

#include "dobby_internal.h"

#include <vector>

#if defined(__linux__) || defined(__APPLE__) || defined(__ANDROID__)
#include <pthread.h>
#endif

namespace {
struct HookOperationMutex {
  pthread_mutex_t mutex;
  HookOperationMutex() {
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
  }
};

pthread_mutex_t *OperationMutex() {
  static HookOperationMutex state;
  return &state.mutex;
}
} // namespace

HookOperationGuard::HookOperationGuard() {
  pthread_mutex_lock(OperationMutex());
}

HookOperationGuard::~HookOperationGuard() {
  pthread_mutex_unlock(OperationMutex());
}

Interceptor *Interceptor::SharedInstance() {
  static Interceptor interceptor;
  return &interceptor;
}

void Interceptor::Lock() {
  pthread_mutex_lock(OperationMutex());
}

void Interceptor::Unlock() {
  pthread_mutex_unlock(OperationMutex());
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
  HookEntry *entry = entry_node ? entry_node->entry : nullptr;
  Unlock();
  return entry;
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

  std::vector<void *> addresses;
  {
    HookOperationGuard operation;
    HookEntryNode *entry_node = nullptr;
    list_for_each_entry(entry_node, &hook_entry_list_, list_node) {
      addresses.push_back(entry_node->entry->instruction_address);
    }
  }
  for (void *address : addresses) {
    if (cb(address, userdata) != 0)
      break;
  }
}
