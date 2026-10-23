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
  DobbyTargetAnalysis analysis = {.struct_size = sizeof(analysis), .api_version = DOBBY_API_VERSION};
  if (DobbyAnalyzeTarget(NULL, NULL, NULL, &analysis) != RS_FAILED)
    return 4;
  if (DobbySetPatchCoordinator(NULL) != RS_SUCCESS)
    return 5;
  DobbyImportOptions imports = {
      .struct_size = sizeof(imports), .api_version = DOBBY_API_VERSION, .flags = DOBBY_IMPORT_GLOB_DAT};
  size_t count = 0;
  if (DobbyInspectImport(NULL, NULL, &imports, NULL, 0, &count) != RS_FAILED)
    return 6;
  DobbyPluginState plugins = {.struct_size = sizeof(plugins), .api_version = DOBBY_API_VERSION};
  if (DobbyQueryPluginState(&plugins) != RS_SUCCESS || plugins.deferred_installed)
    return 7;
  DobbyAddressSpec spec = {.struct_size = sizeof(spec), .api_version = DOBBY_API_VERSION};
  void *address = NULL;
  if (DobbyResolveModuleAddress(0, &spec, &address) != RS_FAILED)
    return 8;
  if (DobbyEnableDiagnostics(0) != RS_SUCCESS || DobbyReadDiagnostics(NULL, 0, &count, NULL) != RS_SUCCESS)
    return 9;
  DobbyResourceStats stats = {.struct_size = sizeof(DobbyResourceStats)};
  return DobbyGetResourceStats(&stats) == RS_SUCCESS ? 0 : 1;
}
