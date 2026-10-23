#include "dobby_internal.h"
#include "Interceptor.h"
#include <vector>
#include <unordered_set>
#include <unistd.h>
#include <sys/auxv.h>
#if defined(__aarch64__)
#include <asm/hwcap.h>
#endif

PUBLIC int DobbyHookBatch(const DobbyHookRequest *requests, size_t count, DobbyBatchResult *result) {
  if (DobbyInPatchCoordinator())
    return DobbyFailure(DOBBY_ERROR_BUSY);
  DobbySetError(DOBBY_ERROR_NONE);
  HookOperationGuard guard;
  if (!requests || !count || count > 4096 || !result || result->struct_size < sizeof(*result) ||
      result->api_version != DOBBY_API_VERSION)
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  *result = {sizeof(*result), DOBBY_API_VERSION, SIZE_MAX, 0, 0, 0, DOBBY_ERROR_NONE, 0};
  std::unordered_set<void *> seen;
  for (size_t i = 0; i < count; ++i) {
    if (!requests[i].target || !requests[i].replacement || !seen.insert(requests[i].target).second ||
        Interceptor::SharedInstance()->FindHookEntry(requests[i].target)) {
      result->failed_index = i;
      result->cause =
          !requests[i].target || !requests[i].replacement ? DOBBY_ERROR_INVALID_ARGUMENT : DOBBY_ERROR_CONFLICT;
      return DobbyFailure(result->cause);
    }
  }
  for (size_t i = 0; i < count; ++i) {
    if (requests[i].original)
      *requests[i].original = nullptr;
  }
  auto Rollback = [&](size_t failed) {
    result->failed_index = failed;
    result->cause = DobbyGetLastError();
    result->system_error = DobbyGetLastSystemError();
    for (size_t i = result->prepared; i > 0; --i) {
      if (DobbyDestroy(requests[i - 1].target) != RS_SUCCESS)
        ++result->rollback_failed;
    }
    return DobbyFailure(result->rollback_failed ? DOBBY_ERROR_ROLLBACK_FAILED : result->cause, result->system_error);
  };
  DobbyError warning = DOBBY_ERROR_NONE;
  int warning_errno = 0;
  for (size_t i = 0; i < count; ++i) {
    if (DobbyPrepareEx(requests[i].target, requests[i].replacement, requests[i].original, requests[i].options) !=
        RS_SUCCESS)
      return Rollback(i);
    ++result->prepared;
  }
  for (size_t i = 0; i < count; ++i) {
    if (DobbyCommit(requests[i].target) != RS_SUCCESS)
      return Rollback(i);
    ++result->committed;
    if (DobbyGetLastError() != DOBBY_ERROR_NONE) {
      warning = DobbyGetLastError();
      warning_errno = DobbyGetLastSystemError();
    }
  }
  result->cause = warning;
  result->system_error = warning_errno;
  DobbySetError(warning, warning_errno);
  return RS_SUCCESS;
}

PUBLIC int DobbyGetRuntimeInfo(DobbyRuntimeInfo *info) {
  DobbySetError(DOBBY_ERROR_NONE);
  if (!info || info->struct_size < sizeof(*info) || info->api_version != DOBBY_API_VERSION)
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  DobbyRuntimeInfo value{};
  value.struct_size = sizeof(value);
  value.api_version = DOBBY_API_VERSION;
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    return DobbyFailure(DOBBY_ERROR_UNSUPPORTED_FORMAT);
  value.page_size = static_cast<size_t>(page_size);
#if defined(__aarch64__)
  value.architecture = "arm64-v8a";
  if (getauxval(AT_HWCAP2) & HWCAP2_BTI)
    value.hardware_features |= DOBBY_HW_BTI;
  if (getauxval(AT_HWCAP) & HWCAP_PACA)
    value.hardware_features |= DOBBY_HW_PAC;
#ifdef HWCAP2_MTE
  if (getauxval(AT_HWCAP2) & HWCAP2_MTE)
    value.hardware_features |= DOBBY_HW_MTE;
#endif
#elif defined(__arm__)
  value.architecture = "armeabi-v7a";
#elif defined(__x86_64__)
  value.architecture = "x86_64";
#else
  value.architecture = "x86";
#endif
#ifdef DOBBY_HAS_NEAR_BRANCH
  value.build_features |= DOBBY_FEATURE_NEAR;
#endif
#ifdef DOBBY_HAS_DBI
  value.build_features |= DOBBY_FEATURE_DBI;
#endif
  value.build_features |= DOBBY_FEATURE_IMPORT_HOOK;
  *info = value;
  return DobbySuccess();
}
