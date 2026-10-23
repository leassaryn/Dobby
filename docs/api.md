# Android hook API

Include `dobby.h` and link `Dobby::dobby`. Existing C entry points and
`RS_SUCCESS` (0) / `RS_FAILED` (-1) return values remain available. The public
header works with C23 and C++23. Plugin APIs require their corresponding build
options; instrumentation requires `DynamicBinaryInstrument=ON`.

## Install and inspect a hook

The replacement and original must have exactly the target's calling convention,
parameter types and return type. This also applies to hidden IL2CPP arguments.
Keep the target module loaded for the entire hook lifetime.

```cpp
#include <dobby.h>
#include <cstdio>

using Target = int (*)(int);
static Target original = nullptr;
static int Replacement(int value) { return original(value) + 1; }

int Install(void *target) {
  void *trampoline = nullptr;
  if (DobbyPrepare(target, reinterpret_cast<void *>(Replacement), &trampoline) != RS_SUCCESS) {
    std::fprintf(stderr, "%s (errno=%d)\n",
                 DobbyGetErrorMessage(DobbyGetLastError()), DobbyGetLastSystemError());
    return RS_FAILED;
  }
  original = reinterpret_cast<Target>(trampoline);
  // Publish the original before redirecting any calls to Replacement.
  return DobbyCommit(target);
}
```

`DobbyHook` combines prepare and commit. `DobbyHookByName` resolves a loaded
symbol and hooks it; a null image name searches loaded modules. Name resolution
does not discover stripped IL2CPP methods. Resolve such addresses using the
application's metadata/runtime information.

A failed commit leaves an inline hook prepared: retry `DobbyCommit`, or call
`DobbyDestroy` to unregister it. Repeating the same replacement/handler is
idempotent. A different replacement, or mixing inline and instrumentation hooks
at the same address, returns `DOBBY_ERROR_CONFLICT`.

Initialize inspection structures before calling the API:

```c
DobbyHookInfo info = {0};
info.struct_size = sizeof(info);
if (DobbyQueryHook(target, &info) == RS_SUCCESS) {
  /* info.state: PREPARED or COMMITTED; patch_address can differ from target. */
}
```

`DobbyQueryHook` returns kind, state, target, actual patch address and size,
replacement/handler, original trampoline, and optional near relay. This is a
snapshot; it does not extend pointer lifetimes. `DobbyEnumerateHooks` visits an
address snapshot outside the registry lock; callbacks may call hook APIs.
Structures reject sizes smaller than the current definition; reserved tail space
in larger structures is not written. Recompile clients if a future release
changes the required structure size.

## Diagnostics

Immediately after an operation, read `DobbyGetLastError()` and
`DobbyGetLastSystemError()`. These are thread-local. Getters do not clear them;
`DobbyGetErrorMessage()` returns an immutable static string. Capture the error
before calling another operation. Hook, inspection and resource APIs clear a
previous error when they succeed, with the exception below. Legacy logging,
plugin toggles, build-version and raw symbol-lookup APIs do not promise to update
these diagnostics.

| Error | Meaning / action |
| --- | --- |
| `INVALID_ARGUMENT` | Null required argument, invalid alignment or structure size. |
| `NOT_FOUND` | No matching hook or symbol. |
| `CONFLICT` | A different hook already owns the address. |
| `FUNCTION_TOO_SHORT` | A long ARM64 patch would cross an early terminal instruction. |
| `ALLOCATION_FAILED`, `NEAR_ALLOCATION_FAILED` | Could not allocate a usable trampoline. |
| `RELOCATION_FAILED`, `UNSUPPORTED_INSTRUCTION` | Cannot safely relocate this prologue. |
| `PROTECTION_FAILED` | Page permissions could not be queried/changed. |
| `PROTECTION_RESTORE_FAILED` | **Bytes were applied**, but restoring page permissions failed. |
| `BUSY` | Resource teardown is forbidden while registered hooks or detected callbacks remain. |
| `MEMORY_RELEASE_FAILED` | Some pool pages could not be unmapped; statistics retain them. |

All names above have the `DOBBY_ERROR_` prefix. Captured system error is zero
unless an underlying OS error was recorded. A successful patch can return
`RS_SUCCESS` with `PROTECTION_RESTORE_FAILED`: the redirect is live, so its
metadata must remain valid. Do not treat this warning as an uninstalled hook.

## ARM64 routing and instrumentation

With `NearBranch=ON` (default), ARM64 tries a four-byte direct branch or an owned
near relay automatically. A long patch is a fallback only when its conservative
terminal-instruction check permits it. This reduces overwrites of adjacent short
IL2CPP methods; it is not a complete function-boundary decoder.

Explicit BTI landing instructions are preserved. PACIASP/PACIBSP entry sequences
retain a landing pad and relocate signing. Relocation prefers direct branches
back to guarded code; unsupported far transfers fail. ARM64 LDR S/D/Q literal loads preserve their vector width; reserved vector-literal
encodings return `DOBBY_ERROR_UNSUPPORTED_INSTRUCTION`. These paths still require
runtime verification on the application's actual BTI/PAC-enabled device and
prologues. `dobby_disable_near_branch_trampoline` disables the automatic relay
policy; enable restores it. x86/x64 require the near allocator at build time.

`DobbyInstrument` invokes a `DBICallTy` handler with register context. On ARM64,
X16/X17 are scratch registers; handlers must follow the header's restrictions.
Access to q8–q31 requires `FullFloatingPointRegisterPack=ON`.

## Unhook and release resources

1. At an application-controlled safe point, call `DobbyDestroy(target)`. This
   restores bytes and removes the entry from the registry. Metadata and generated
   code remain retained so already-entered callbacks are not immediately freed.
2. After all hook/DBI frames have finished and saved trampoline pointers are no
   longer used, call `DobbyCollectRetiredHooks(&count)`. This frees retired
   metadata, including closure descriptors, but keeps arena pages mapped.
3. For complete teardown, after destroying **all** hooks and establishing the
   same quiescence, call `DobbyReleaseCodeMemory(&bytes)`. It collects metadata,
   clears bridge/stub caches and unmaps pools. Every old trampoline pointer is
   invalid afterward. Later hooks allocate fresh resources.

Count outputs are optional (pass null). `DobbyGetResourceStats` accepts a
`DobbyResourceStats` initialized with `struct_size = sizeof(stats)`. It reports
registered/retired hook counts and retained executable/data arena capacity,
including unused space; this is not a total-process heap measurement.

Dobby detects active DBI callbacks, but cannot prove that another thread is
approaching a trampoline or holding an original pointer. **The caller must
establish quiescence.** Do not collect/release from a DBI callback. Internal locks
serialize management operations; they do not make instruction patching atomic
against threads executing the target. Install/remove hooks at load or idle time.

## Linker callbacks

`dobby_register_image_load_callback` installs its loader hook lazily on the first
registration. Repeated registration of the same callback is a no-op; null is an
invalid argument. Check thread-local diagnostics after this legacy void API.
Callbacks run from a snapshot outside the callback-list mutex and receive only
successful loads with a non-null filename. This plugin requires
`Plugin.SymbolResolver`. New code can use `DobbyRegisterImageLoadCallback` to
receive a handle. Duplicate functions return the same handle, not a reference
count. `DobbyUnregisterImageLoadCallback` returns `BUSY` while that callback is
executing (including self-unregistration); successful removal prevents future
invocation. Keep its code loaded until removal succeeds and its frames finish.

After unregistering every callback and establishing loader-frame quiescence,
call `DobbyStopImageLoadMonitor` to remove the internal hook. Then normal
collection/pool teardown is available. Registration can restart monitoring.
Internal module pin operations do not generate user load notifications.

## Versioned per-hook options

Initialize every new options/result/info structure with `struct_size = sizeof(value)`
and `api_version = DOBBY_API_VERSION`. Version 1 rejects undersized structures and
unknown versions. Legacy entry points remain available.

```c
DobbyHookOptions options = {
    .struct_size = sizeof(options),
    .api_version = DOBBY_API_VERSION,
    .near_policy = DOBBY_NEAR_REQUIRED,
    .max_patch_bytes = 4,
};
void *original = NULL;
int status = DobbyHookEx(target, replacement, &original, &options);
```

`DobbyPrepareEx` separates preparation from `DobbyCommit`; `DobbyHookEx` does both.
`AUTO` and null options select the per-hook automatic policy independently of the
legacy global toggle. `REQUIRED` requires the ARM64 near feature and refuses a
long fallback. `DISABLED` disables near relays; a directly reachable branch is
still permitted. `max_patch_bytes = 0` means the engine default. A nonzero limit
bounds the affected prologue span, including a preserved BTI prefix and complete
relocated instructions. It is not a guarantee that arbitrary source pointers are
readable. Conflicting options on an already registered target are rejected.

## Batch installation

`DobbyHookBatch(requests, count, &result)` accepts 1–4096 `DobbyHookRequest` entries.
It rejects duplicate/existing targets before taking ownership, prepares every
hook, then commits every hook. A failure destroys owned entries in reverse order.
It never removes a pre-existing hook. This is management rollback, **not atomic
publication to threads executing code**; establish quiescence first.

Initialize `DobbyBatchResult` with size/version. `failed_index` is `SIZE_MAX` on
success; `prepared` and `committed` count work completed before rollback.
`cause`/`system_error` preserve the triggering failure. `rollback_failed` reports
failed destroy operations; `DOBBY_ERROR_ROLLBACK_FAILED` requires inspection and
retry of remaining registered hooks. Permission-restoration warnings require
attention even when byte restoration succeeds. Discard all returned original
pointers after a failed batch, then collect retired resources at a safe point.

## ELF imports and module ownership

`DobbyHookImport(importing_image, symbol, replacement, &handle)` patches matching
standard ELF PLT `JUMP_SLOT` entries (REL or RELA) in one already loaded importing
module. It supports full RELRO by temporarily changing permissions and restoring
them. This backend does not rewrite the provider's function body. It does not
implement non-PLT `GLOB_DAT`, packed relocation tables or symbol-version selection.
Missing imports fail rather than falling back to inline hooking. The table format
follows [Bionic's ELF relocation handling](https://android.googlesource.com/platform/bionic/+/bcc04d0069a919a6ac4a2b378e15dd0a50c46aec/linker/linker.cpp).

Use `DobbyGetImportOriginal` to obtain the original resolved function and
`DobbyDestroyImport` to restore slots. Existing owned slots and conflicting
external changes are rejected. If installation rollback is incomplete, the
nonzero output handle remains owned even on failure; retain it and retry destroy.
Import handles are separate from inline hook enumeration/resource statistics.
Install/remove imports while their callers are quiescent. Keep replacement and
provider code alive while callable; destroy does not invalidate already entered
frames safely by itself.

`DobbyAcquireModule` pins an already loaded module using `RTLD_NOLOAD`. Names must
be an exact path or an unambiguous basename. It never loads an absent module.
`DobbyQueryModule` returns the base/path; the path is valid until releasing that
handle. `DobbyReleaseModule` rejects handles whose module contains active inline
hook targets/replacements, import replacements or registered callback functions.
An import hook independently pins its importer until destruction. Arbitrary
external `dlclose` calls are not intercepted. Checks cannot detect saved pointers
or retired frames: establish quiescence before releasing the final reference.

## Runtime information and measurements

`DobbyGetRuntimeInfo` reports architecture, actual kernel page size, detected
BTI/PAC/MTE hardware bits and compiled near/DBI/import features. Capability
presence is not evidence that a particular prologue or hardware enforcement
configuration was tested. `DOBBY_BUILD_BENCHMARKS=ON` builds
`dobby_hook_benchmark`; see the build guide for measured call/setup overhead.

## Migration

Existing applications can keep using `DobbyHook` and `DobbyDestroy`. Check return
values, add diagnostics and inspect prepared state after failed commits. Adopt
explicit collection only when the application can prove quiescence. Automatic
ARM64 near routing is now the default; tests relying on a long patch can disable
it explicitly. Use the exported CMake target instead of copying internal include
directories or depending on private headers.
