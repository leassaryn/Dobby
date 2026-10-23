#include "dobby.h"
#include <dlfcn.h>
#include <cstdio>

namespace {
int calls = 0;
DobbyCallbackHandle registration = 0;
bool rejected_self_removal = false;
void Loaded(const char *, void *) {
  ++calls;
  rejected_self_removal =
      DobbyUnregisterImageLoadCallback(registration) == RS_FAILED && DobbyGetLastError() == DOBBY_ERROR_BUSY;
  // Reentry must not deadlock or add a duplicate callback.
  dobby_register_image_load_callback(Loaded);
}
} // namespace

int main() {
  DobbyResourceStats stats{};
  stats.struct_size = sizeof(stats);
  if (DobbyGetResourceStats(&stats) != RS_SUCCESS || stats.registered_hooks != 0)
    return 1; // Loading the library must not install an unsolicited hook.
  dobby_register_image_load_callback(nullptr);
  if (DobbyGetLastError() != DOBBY_ERROR_INVALID_ARGUMENT)
    return 2;
  DobbyRegisterImageLoadCallback(Loaded, &registration);
  if (DobbyGetLastError() != DOBBY_ERROR_NONE) {
    std::fprintf(stderr, "registration: %s\n", DobbyGetErrorMessage(DobbyGetLastError()));
    return 3;
  }
  dobby_register_image_load_callback(Loaded);
  void *handle = dlopen("libm.so", RTLD_NOW | RTLD_LOCAL);
  if (!handle || calls != 1)
    return 4;
  dlclose(handle);
  handle = dlopen("libm.so", RTLD_NOW | RTLD_LOCAL);
  if (!handle || calls != 2)
    return 5;
  dlclose(handle);
  handle = dlopen("/dobby-nonexistent-library.so", RTLD_NOW);
  if (handle || calls != 2)
    return 6;
  if (!rejected_self_removal || DobbyUnregisterImageLoadCallback(registration) != RS_SUCCESS)
    return 7;
  handle = dlopen("libm.so", RTLD_NOW);
  if (!handle || calls != 2)
    return 8;
  dlclose(handle);
  if (DobbyStopImageLoadMonitor() != RS_SUCCESS || DobbyReleaseCodeMemory(nullptr) != RS_SUCCESS)
    return 9;
  if (DobbyRegisterImageLoadCallback(Loaded, &registration) != RS_SUCCESS)
    return 10;
  handle = dlopen("libm.so", RTLD_NOW);
  if (!handle || calls != 3)
    return 11;
  dlclose(handle);
  if (DobbyUnregisterImageLoadCallback(registration) != RS_SUCCESS || DobbyStopImageLoadMonitor() != RS_SUCCESS ||
      DobbyReleaseCodeMemory(nullptr) != RS_SUCCESS)
    return 12;
  return 0;
}
