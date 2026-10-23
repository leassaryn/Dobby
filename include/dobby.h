#ifndef dobby_h
#define dobby_h

// obfuscated interface
#if 0
#define DobbyBuildVersion c343f74888dffad84d9ad08d9c433456
#define DobbyHook c8dc3ffa44f22dbd10ccae213dd8b1f8
#define DobbyInstrument b71e27bca2c362de90c1034f19d839f9
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

void log_set_level(int level);
void log_switch_to_syslog(void);
void log_switch_to_file(const char *path);

typedef enum {
  kMemoryOperationSuccess,
  kMemoryOperationError,
  kNotSupportAllocateExecutableMemory,
  kNotEnough,
  kNone
} MemoryOperationError;

#define PLATFORM_INTERFACE_CODE_PATCH_TOOL_H
MemoryOperationError CodePatch(void *address, uint8_t *buffer, uint32_t buffer_size);

typedef uintptr_t addr_t;
typedef uint32_t addr32_t;
typedef uint64_t addr64_t;

#if defined(__arm64__) || defined(__aarch64__)

#define ARM64_TMP_REG_NDX_0 17

// float register
typedef union _FPReg {
  __int128_t q;
  struct {
    double d1;
    double d2;
  } d;
  struct {
    float f1;
    float f2;
    float f3;
    float f4;
  } f;
} FPReg;

// register context
typedef struct _RegisterContext {
  uint64_t dmmpy_0; // dummy placeholder
  uint64_t sp;

  uint64_t dmmpy_1; // dummy placeholder
  union {
    uint64_t x[29];
    struct {
      uint64_t x0, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16, x17, x18, x19, x20, x21, x22,
          x23, x24, x25, x26, x27, x28;
    } regs;
  } general;

  uint64_t fp;
  uint64_t lr;

  union {
    FPReg q[32];
    struct {
      FPReg q0, q1, q2, q3, q4, q5, q6, q7;
      // [!!! READ ME !!!]
      // for Arm64, can't access q8 - q31, unless you enable full floating-point register pack
      FPReg q8, q9, q10, q11, q12, q13, q14, q15, q16, q17, q18, q19, q20, q21, q22, q23, q24, q25, q26, q27, q28, q29,
          q30, q31;
    } regs;
  } floating;
} RegisterContext;
#elif defined(__arm__)
typedef struct _RegisterContext {
  uint32_t dummy_0;
  uint32_t dummy_1;

  uint32_t dummy_2;
  uint32_t sp;

  union {
    uint32_t r[13];
    struct {
      uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12;
    } regs;
  } general;

  uint32_t lr;
} RegisterContext;
#elif defined(_M_IX86) || defined(__i386__)
typedef struct _RegisterContext {
  uint32_t dummy_0;
  uint32_t esp;

  uint32_t dummy_1;
  uint32_t flags;

  union {
    struct {
      uint32_t eax, ebx, ecx, edx, ebp, esp, edi, esi;
    } regs;
  } general;

} RegisterContext;
#elif defined(_M_X64) || defined(__x86_64__)
typedef struct _RegisterContext {
  uint64_t dummy_0;
  uint64_t rsp;

  union {
    struct {
      uint64_t rax, rbx, rcx, rdx, rbp, rsp, rdi, rsi, r8, r9, r10, r11, r12, r13, r14, r15;
    } regs;
  } general;

  uint64_t dummy_1;
  uint64_t flags;
} RegisterContext;
#endif

#define RT_FAILED -1
#define RT_SUCCESS 0
typedef enum _RetStatus { RS_FAILED = -1, RS_SUCCESS = 0 } RetStatus;

// Hook APIs retain RS_SUCCESS/RS_FAILED. Diagnostics are per-thread; getters do
// not clear them. Successful operations clear previous errors unless documented.
typedef enum {
  DOBBY_ERROR_NONE = 0,
  DOBBY_ERROR_INVALID_ARGUMENT,
  DOBBY_ERROR_NOT_FOUND,
  DOBBY_ERROR_CONFLICT,
  DOBBY_ERROR_FUNCTION_TOO_SHORT,
  DOBBY_ERROR_ALLOCATION_FAILED,
  DOBBY_ERROR_NEAR_ALLOCATION_FAILED,
  DOBBY_ERROR_RELOCATION_FAILED,
  DOBBY_ERROR_PROTECTION_FAILED,
  DOBBY_ERROR_UNSUPPORTED_INSTRUCTION,
  DOBBY_ERROR_BUSY,
  DOBBY_ERROR_PROTECTION_RESTORE_FAILED,
  DOBBY_ERROR_MEMORY_RELEASE_FAILED,
  DOBBY_ERROR_PATCH_LIMIT,
  DOBBY_ERROR_UNSUPPORTED_FORMAT,
  DOBBY_ERROR_ROLLBACK_FAILED
} DobbyError;
DobbyError DobbyGetLastError(void);
int DobbyGetLastSystemError(void);                  // captured errno, or zero
const char *DobbyGetErrorMessage(DobbyError error); // immutable static string

typedef enum { DOBBY_HOOK_INLINE = 1, DOBBY_HOOK_INSTRUMENT = 2 } DobbyHookKind;
typedef enum { DOBBY_HOOK_PREPARED = 1, DOBBY_HOOK_COMMITTED = 2 } DobbyHookState;
typedef struct {
  size_t struct_size; // initialize to sizeof(DobbyHookInfo)
  DobbyHookKind kind;
  DobbyHookState state;
  void *target;
  void *patch_address;
  void *replacement; // callback address for instrumentation
  void *original;
  size_t patch_size;
  void *relay; // near relay, or NULL for a direct/long patch
} DobbyHookInfo;
// Snapshot under the operation lock. Pointer lifetimes are not extended by query.
int DobbyQueryHook(void *address, DobbyHookInfo *info);

typedef struct {
  size_t struct_size; // initialize to sizeof(DobbyResourceStats)
  size_t registered_hooks;
  size_t retired_hooks;
  size_t retained_code_bytes; // executable/data arena page capacity, retained until explicit release
} DobbyResourceStats;
int DobbyGetResourceStats(DobbyResourceStats *stats);
// Restore/unregister first with DobbyDestroy. Call this ONLY after all hook/DBI
// frames have finished and all saved trampoline pointers are no longer in use.
// Frees retired metadata; pooled executable pages stay mapped until DobbyReleaseCodeMemory.
// Never call from a DBI callback. released_count may be NULL.
int DobbyCollectRetiredHooks(size_t *released_count);
// Explicit quiescent teardown: requires no registered hooks, no in-flight hook/DBI
// frames, and no retained original/relay pointers. Collects retired metadata and
// unmaps pooled code. Invalidates ALL previously returned trampoline pointers.
// The library remains usable; later hooks allocate fresh pools/bridges.
int DobbyReleaseCodeMemory(size_t *released_bytes);

// Versioned additions. Initialize struct_size and api_version for every input/output.
#define DOBBY_API_VERSION 1u
typedef enum { DOBBY_NEAR_AUTO = 0, DOBBY_NEAR_REQUIRED = 1, DOBBY_NEAR_DISABLED = 2 } DobbyNearPolicy;
typedef struct {
  size_t struct_size;
  uint32_t api_version;
  DobbyNearPolicy near_policy;
  size_t max_patch_bytes; // zero: engine default; includes preserved entry prefix
} DobbyHookOptions;
int DobbyPrepareEx(void *target, void *replacement, void **original, const DobbyHookOptions *options);
int DobbyHookEx(void *target, void *replacement, void **original, const DobbyHookOptions *options);
typedef struct {
  void *target;
  void *replacement;
  void **original;
  const DobbyHookOptions *options; // NULL: default per-hook AUTO policy
} DobbyHookRequest;
typedef struct {
  size_t struct_size;
  uint32_t api_version;
  size_t failed_index; // SIZE_MAX on success
  size_t prepared;
  size_t committed;
  size_t rollback_failed;
  DobbyError cause;
  int system_error;
} DobbyBatchResult;
// Owns only new hooks. Rejects existing/duplicate targets. Caller must ensure quiescence.
int DobbyHookBatch(const DobbyHookRequest *requests, size_t count, DobbyBatchResult *result);

typedef struct {
  size_t struct_size;
  uint32_t api_version;
  size_t page_size;
  uint64_t hardware_features; // detected, NOT a runtime compatibility certification
  uint64_t build_features;
  const char *architecture; // static string
} DobbyRuntimeInfo;
#define DOBBY_HW_BTI (UINT64_C(1) << 0)
#define DOBBY_HW_PAC (UINT64_C(1) << 1)
#define DOBBY_HW_MTE (UINT64_C(1) << 2)
#define DOBBY_FEATURE_NEAR (UINT64_C(1) << 0)
#define DOBBY_FEATURE_DBI (UINT64_C(1) << 1)
#define DOBBY_FEATURE_IMPORT_HOOK (UINT64_C(1) << 2)
int DobbyGetRuntimeInfo(DobbyRuntimeInfo *info);

typedef uint64_t DobbyModuleHandle;
typedef struct {
  size_t struct_size;
  uint32_t api_version;
  void *base;
  const char *path; // valid until this module handle is released
} DobbyModuleInfo;
// Pins an already loaded module. No implicit load of an absent module.
int DobbyAcquireModule(const char *image_name, DobbyModuleHandle *handle);
int DobbyQueryModule(DobbyModuleHandle handle, DobbyModuleInfo *info);
int DobbyReleaseModule(DobbyModuleHandle handle);

typedef uint64_t DobbyImportHandle;
// Hooks matching JUMP_SLOT entries in one pinned importing ELF module.
// Standard REL/RELA PLT tables only. Full RELRO permissions are restored.
int DobbyHookImport(const char *image_name, const char *symbol, void *replacement, DobbyImportHandle *handle);
int DobbyDestroyImport(DobbyImportHandle handle);
int DobbyGetImportOriginal(DobbyImportHandle handle, void **original);

typedef struct _HookEntryInfo {
  int hook_id;
  union {
    void *target_address;
    void *function_address;
    void *instruction_address;
  };
} HookEntryInfo;

// DobbyWrap <==> DobbyInstrument, so use DobbyInstrument instead of DobbyWrap
#if 0
// wrap function with pre_call and post_call
typedef void (*PreCallTy)(RegisterContext *ctx, const HookEntryInfo *info);
typedef void (*PostCallTy)(RegisterContext *ctx, const HookEntryInfo *info);
int DobbyWrap(void *function_address, PreCallTy pre_call, PostCallTy post_call);
#endif

// return dobby build date
const char *DobbyBuildVersion(void);

// Hook and commit a function. A different replacement on an existing entry is
// rejected; destroy the entry first. ARM64 rejects long patches crossing an early
// terminal instruction. The caller must keep target code mapped and quiescent.
int DobbyHook(void *address, void *replace_call, void **origin_call);

// resolve a symbol by name and hook it in one call.
// `image_name` may be NULL to search every loaded module.
int DobbyHookByName(const char *image_name, const char *symbol_name, void *replace_call, void **origin_call);

// prepare trampoline
int DobbyPrepare(void *address, void *replace_call, void **origin_call);

// commit hook
int DobbyCommit(void *address);

// dynamic binary instrument for instruction
// [!!! READ ME !!!]
// for Arm64, can't access q8 - q31, unless you enable full floating-point register pack
//
// [!!! READ ME (ARM64) !!!]
// The instrumented function runs inside a closure bridge that uses X17 (and X16
// as the branch target) as scratch registers. Your DBICallTy handler MUST NOT
// assume X16/X17 keep their incoming values, and it MUST NOT leave X16/X17
// modified if it expects the relocated code (next-hop) to run — Dobby restores
// the next-hop branch target via X17.
typedef void (*DBICallTy)(RegisterContext *ctx, const HookEntryInfo *info);
// Idempotent: instrumenting the same address with the same handler succeeds as
// a no-op instead of failing.
int DobbyInstrument(void *address, DBICallTy dbi_call);

// Restore and unregister. Retain metadata/code for in-flight calls until explicit
// quiescent collection; this does not make concurrent instruction patching atomic.
int DobbyDestroy(void *address);

// Enumerate a snapshot of registered hook/instrument addresses, including prepared
// entries. Callbacks run without the registry lock and may call Dobby APIs. Entries
// may change after the snapshot. Return 0 to continue, non-zero to stop early.
typedef int (*dobby_hook_iterator_t)(void *hook_address, void *userdata);
int DobbyEnumerateHooks(dobby_hook_iterator_t cb, void *userdata);

// iterate symbol table and find symbol
void *DobbySymbolResolver(const char *image_name, const char *symbol_name);

// [!!! READ ME !!!]
// for arm, Arm64, dobby will use b xxx instead of ldr absolute indirect branch
// for x64, dobby always use absolute indirect jump
#if defined(__arm__) || defined(__arm64__) || defined(__aarch64__) || defined(_M_X64) || defined(__x86_64__)
void dobby_enable_near_branch_trampoline(void);
void dobby_disable_near_branch_trampoline(void);
#endif

typedef void (*linker_load_callback_t)(const char *image_name, void *handle);
typedef uint64_t DobbyCallbackHandle;
int DobbyRegisterImageLoadCallback(linker_load_callback_t callback, DobbyCallbackHandle *handle);
// BUSY if this callback is executing; success prevents future invocation.
int DobbyUnregisterImageLoadCallback(DobbyCallbackHandle handle);
// Requires an empty callback list and caller-proven loader-frame quiescence.
int DobbyStopImageLoadMonitor(void);
// Register once per callback; lazily installs loader monitoring. Check last error.
// Callback code must remain loaded. Unregister all callbacks before stopping the monitor.
void dobby_register_image_load_callback(linker_load_callback_t func);

#ifdef __cplusplus
}
#endif

#endif
