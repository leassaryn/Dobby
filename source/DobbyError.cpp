#include "DobbyError.h"
#include "dobby_internal.h"

namespace {
thread_local DobbyError last_error = DOBBY_ERROR_NONE;
thread_local int last_system_error = 0;
} // namespace

void DobbySetError(DobbyError error, int system_error) {
  last_error = error;
  last_system_error = system_error;
}

int DobbyFailure(DobbyError error, int system_error) {
  DobbySetError(error, system_error);
  return RS_FAILED;
}

int DobbySuccess() {
  if (last_error != DOBBY_ERROR_PROTECTION_RESTORE_FAILED)
    DobbySetError(DOBBY_ERROR_NONE);
  return RS_SUCCESS;
}

PUBLIC DobbyError DobbyGetLastError(void) {
  return last_error;
}

PUBLIC int DobbyGetLastSystemError(void) {
  return last_system_error;
}

PUBLIC const char *DobbyGetErrorMessage(DobbyError error) {
  switch (error) {
  case DOBBY_ERROR_PATCH_LIMIT:
    return "patch exceeds caller byte limit";
  case DOBBY_ERROR_UNSUPPORTED_FORMAT:
    return "unsupported format or feature";
  case DOBBY_ERROR_ROLLBACK_FAILED:
    return "rollback incomplete; inspect retained hooks";
  case DOBBY_ERROR_NONE:
    return "success";
  case DOBBY_ERROR_INVALID_ARGUMENT:
    return "invalid argument or structure size";
  case DOBBY_ERROR_NOT_FOUND:
    return "hook or symbol not found";
  case DOBBY_ERROR_CONFLICT:
    return "address already has a different hook";
  case DOBBY_ERROR_FUNCTION_TOO_SHORT:
    return "patch would cross a function boundary";
  case DOBBY_ERROR_ALLOCATION_FAILED:
    return "executable memory allocation failed";
  case DOBBY_ERROR_NEAR_ALLOCATION_FAILED:
    return "no executable memory in branch range";
  case DOBBY_ERROR_RELOCATION_FAILED:
    return "instruction relocation failed";
  case DOBBY_ERROR_PROTECTION_FAILED:
    return "memory permission change failed";
  case DOBBY_ERROR_UNSUPPORTED_INSTRUCTION:
    return "instruction is unsupported for relocation";
  case DOBBY_ERROR_MEMORY_RELEASE_FAILED:
    return "failed to unmap a code pool";
  case DOBBY_ERROR_BUSY:
    return "operation requires quiescent hook callbacks";
  case DOBBY_ERROR_PROTECTION_RESTORE_FAILED:
    return "patch applied, but page permission restoration failed";
  }
  return "unknown Dobby error";
}
