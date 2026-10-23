#include "dobby.h"

int main(void) {
  DobbyHookInfo info = {.struct_size = sizeof(DobbyHookInfo)};
  if (DobbyQueryHook(NULL, &info) != RS_FAILED || DobbyGetLastError() != DOBBY_ERROR_INVALID_ARGUMENT)
    return 1;
  DobbyRuntimeInfo runtime = {.struct_size = sizeof(DobbyRuntimeInfo), .api_version = DOBBY_API_VERSION};
  if (DobbyGetRuntimeInfo(&runtime) != RS_SUCCESS || !runtime.page_size || !runtime.architecture)
    return 2;
  DobbyHookOptions options = {
      .struct_size = sizeof(DobbyHookOptions), .api_version = DOBBY_API_VERSION, .near_policy = DOBBY_NEAR_AUTO};
  if (DobbyPrepareEx(NULL, NULL, NULL, &options) != RS_FAILED)
    return 3;
  DobbyResourceStats stats = {.struct_size = sizeof(DobbyResourceStats)};
  return DobbyGetResourceStats(&stats) == RS_SUCCESS ? 0 : 1;
}
