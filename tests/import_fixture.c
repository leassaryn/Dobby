#include <unistd.h>
__attribute__((visibility("default"), noinline)) int dobby_test_imported_call(void) {
  return (int)getpid();
}

__attribute__((visibility("default"))) void dobby_test_module_callback(const char *name, void *handle) {
  (void)name;
  (void)handle;
}
