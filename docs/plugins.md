# Generic hooking plugins

These APIs operate on ELF modules and native addresses. They contain no IL2CPP,
Unity, Java or game-specific resolution logic. Public declarations live in
`include/dobby.h` and remain C-compatible.

## Lifecycle and ownership

`DobbyQueryPluginState` reports deferred requests, module/import handles and loader
callback/monitor state. Registries initialize on first use, including calls from
application constructors. Linker lookup and final pin release run outside the
hook-operation mutex; concurrent deferred mutations return BUSY while a polling
pass is in progress. Load notifications arriving during a pass request another
pass rather than silently disappearing. `DobbyShutdownPlugins` requires all deferred requests
cancelled, import hooks destroyed, module handles released, and user loader
callbacks unregistered. It removes the internal deferred subscription, stops the
image monitor and disables an owned namespace bypass. A busy/conflicting resource
causes failure; retry after resolving it. Shutdown can make partial progress,
never destroys an unrelated inline hook, and is idempotent after success.

Shutdown is not a substitute for caller-proven quiescence. Finish in-flight
loader calls and hook/trampoline frames first. Retired hooks and executable pools
still use the separate collect/release APIs. Caller-owned replacement code and
userdata must remain loaded. Module-release checks include pending deferred
replacement addresses, while installed hooks use the existing registry checks.

## Pinned address resolution

Acquire a module with `DobbyAcquireModule`, then use `DobbyGetModuleBuildId` and
`DobbyResolveModuleAddress`. An address specification chooses an exact symbol or
a load-bias-relative offset. On ARM32 an offset retains its Thumb bit. Optional
GNU build ID bytes and expected prologue bytes reject mismatches with `CONFLICT`.
Both guards are bounded to 64 bytes. Returned code must lie in a readable,
executable segment of the selected module, including all expected bytes.
An exported symbol found in a dependency is rejected. Keep the module pinned
while using the address; guards do not protect against concurrent external writes. Address resolution may enter the linker and
is rejected from patch-coordinator callbacks.

The legacy `DobbySymbolResolver` now honors a supplied module name: exact path or
unique basename, with relative filesystem paths normalized. It no longer falls
back to an unrelated module/global symbol. With NULL image, global lookup remains
available. Private symbol fallback reads an owned, bounded file buffer (up to
256 MiB), validates ELF section/symbol/string extents, and skips undefined,
TLS and IFUNC file symbols. Native `dlsym` still handles exported symbols.
File fallback requires the loaded backing file to remain unchanged and accessible;
APK-contained libraries without a standalone file and removed section tables may
only resolve exported symbols. This is not a pattern scanner or an ELF loader.

## Deferred hooks

```c
DobbyAddressSpec spec = {
    .struct_size = sizeof(spec), .api_version = DOBBY_API_VERSION,
    .symbol = "native_entry",
};
DobbyDeferredHandle request = 0;
int result = DobbyRegisterDeferredHook("libexample.so", &spec, replacement,
                                      NULL, &request);
```

Registration copies the module/symbol strings and byte guards. A successful
registration returns an owned handle, not a promise that the hook was installed.
Query `DobbyQueryDeferredHook` for PENDING, INSTALLED or FAILED, the attempt error,
target and borrowed original trampoline. Registration attempts resolution
immediately. When `Plugin.LinkerLoadCallback` is enabled it subscribes to the
existing dlopen monitor, which retries after successful loader calls. With that
plugin disabled, use `DobbyPollDeferredHooks` explicitly. Polling also discovers
modules loaded through paths the monitor does not intercept, such as an
unmonitored `android_dlopen_ext` call. Constructors have already run when a load
notification arrives; this cannot intercept constructor-time calls retroactively.

The installing thread runs prepare/commit and the application's patch coordinator.
Arrange safe points for every possible installation, including notifications.
Polling retries missing/failed requests and skips successful installations.
An existing hook is a conflict even if its replacement is identical: deferred
requests never adopt another owner's hook.

`DobbyCancelDeferredHook` restores its own installed hook and releases its pin.
Conflicts or a busy module retain the handle for retry. Once cancellation starts,
a subsequent poll cannot reinstall that request, even if pin release was blocked
by another hook. Cancel again after removing the blocker. Do not independently
call `DobbyDestroy` on a deferred-owned target. Original trampoline pointers expire
according to the normal retire/collect contract, and their module must stay loaded
until execution has quiesced.

## Packed import relocations

GLOB_DAT inspection/hooking now reads both standard REL/RELA and Android APS2
`DT_ANDROID_REL`/`DT_ANDROID_RELA` streams. Decoding follows
[Bionic's packed iterator format](https://android.googlesource.com/platform/bionic/+/master/linker/linker_reloc_iterators.h).
Input extents, SLEB128 width, group count/flags, signed deltas and address widths
are checked. Work is bounded to 1,048,576 relocations per stream. Malformed streams
fail before slot writes. PLT JUMP_SLOT behavior, symbol-version matching, module
pins, RELRO restoration and rollback ownership remain unchanged. RELR describes
relative relocations without named imported symbols; it is not an import-hook
source. Symbol-definition version selection through DT_VERDEF remains unsupported.

## Structured diagnostics

Enable the optional fixed 256-event queue with `DobbyEnableDiagnostics(1)`.
`DobbyReadDiagnostics` drains into caller storage; NULL storage/capacity zero
queries the pending count. Sequence numbers are monotonic, and the dropped count
reports oldest events overwritten when the queue fills. Enabling/disabling clears
the queue/drop counter. Successful diagnostic reads/configuration preserve the
calling thread's last error. No user callback executes in hook or loader internals.

Events include errors, prepared hooks, live patch attempts, resolved addresses and
deferred attempts. PREPARE's detail is 1 for an owned relay, otherwise 0. PATCH's
detail is `(operation << 1) | applied`; it includes denied/failed writes. Events
are diagnostic snapshots, not ownership handles, and may contain repeated errors
from nested operations. Collection is opt-in and is not intended for async signal
handlers or per-instruction tracing.

## Bionic linker restrictions

`Plugin.Android.BionicLinkerRestriction` now explicitly requires SymbolResolver.
Linker path selection reads the Android SDK level, not the release-name string.
`DobbySetLinkerNamespaceBypass` only hooks an exact known private accessibility
function and owns that hook through disable/retry. Missing symbols return
`UNSUPPORTED_FORMAT`; hook conflicts and relocation failures retain their actual
errors. An enabled flag is never inferred from merely finding a symbol.

Guessed soinfo list traversal and fixed-offset namespace writes were removed.
The legacy `linker_iterate_soinfo` entry point returns via last-error reporting
with `UNSUPPORTED_FORMAT`; it does not invoke its callback. Legacy opaque-soinfo
accessors check arguments/symbol availability but still require caller-valid
objects. Use public module APIs for discovery. Private linker ABI support remains
best-effort and requires device-specific validation; compiling this plugin is not
proof that bypass is available on an Android version.

## Verification

`dobby_plugin_api_test` exercises early-constructor module lifecycle, automatic
and manual deferred installation, cancellation, retry after denied patches,
foreign ownership, build-ID/prologue rejection, module-scoped/private resolution,
real packed GLOB_DAT hooks, last-error preservation and diagnostic overflow.
A fixture destructor joins a second thread querying Dobby to detect lock-order
regressions during final pin release. Parser checks include grouped
REL/RELA forms, signed deltas, every truncation boundary, malformed ELF sections
and 10,000 deterministic malformed packed streams. `--parser-only` skips hooking.
The device runner and Python builder include the new executable/packed fixture.
The packed fixture requires a runtime loader supporting APS2 (Android API 23+).
