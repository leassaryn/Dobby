#include <unistd.h>
#include <pthread.h>
__attribute__((visibility("default"), noinline)) int dobby_test_imported_call(void) {
  return (int)getpid();
}

__attribute__((visibility("default"))) void dobby_test_module_callback(const char *name, void *handle) {
  (void)name;
  (void)handle;
}

__attribute__((visibility("default"))) void *dobby_test_import_address(void) {
  return (void *)getppid;
}

__attribute__((visibility("hidden"), noinline)) int dobby_test_private_function(void) {
  return 73;
}
__attribute__((visibility("default"))) void *dobby_test_private_address(void) {
  return (void *)dobby_test_private_function;
}

static void (*unload_callback)(void);
__attribute__((visibility("default"))) void dobby_test_on_unload(void (*callback)(void)) {
  unload_callback = callback;
}
static void *notify_unload(void *unused) {
  (void)unused;
  unload_callback();
  return 0;
}
__attribute__((destructor)) static void unload_fixture(void) {
  if (unload_callback) {
    pthread_t thread;
    if (pthread_create(&thread, 0, notify_unload, 0) == 0)
      pthread_join(thread, 0);
  }
}
