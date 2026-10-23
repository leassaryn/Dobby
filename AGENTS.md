# AGENTS.md

Dobby: a C/C++ runtime hook framework, **Android-only** (targets arm64-v8a, armeabi-v7a, x86, x86_64). Builds a `dobby` static (default) or shared library from CMake. All Darwin/macOS/iOS/Windows code has been removed. The C++ is **C++23**, the C is **C23** (CMake sets both `CMAKE_CXX_STANDARD=23` / `CMAKE_C_STANDARD=23`).

> Android is the supported artifact target. CMake also accepts Linux for local host development and smoke tests; this is not a supported release target. Preserve the POSIX/Linux implementations (`platform-posix.cc`, `code-patch-tool-posix.cc`, `source/UserMode/PlatformUtil/Linux/`) because Android uses them.

## Build

The build hinges on `cmake/EarlyGlobals.cmake`, which derives `PROCESSOR.*`/`SYSTEM.*` from `CMAKE_SYSTEM_PROCESSOR`/`CMAKE_SYSTEM_NAME`. Use the NDK toolchain to populate these when cross-compiling; custom toolchains must provide recognized values. `EarlyGlobals.cmake` is included twice (before AND after `project()`) intentionally for CMake >= 4.0; preserve both includes.

- Host build (this environment, **local dev convenience only — not a supported artifact**): `mkdir build && cd build && cmake .. && make -j4 dobby`
- Always build the `dobby` target directly (`make dobby`); examples are commented out of the top-level build.
- Key options (see `CMakeLists.txt`): `DOBBY_GENERATE_SHARED`, `DOBBY_DEBUG`, `NearBranch`, `DynamicBinaryInstrument`, `FullFloatingPointRegisterPack`, `Plugin.SymbolResolver`, `Plugin.LinkerLoadCallback`, `Plugin.Android.BionicLinkerRestriction`. Note `NearBranch` is force-ON for x86/x64.
- `DOBBY_DEBUG` is auto-enabled when `CMAKE_BUILD_TYPE` is undefined or exactly `Debug`; other build types retain the option value. Debug logging adds `-DDOBBY_DEBUG -DLOGGING_DEBUG` and skips some release optimization/visibility flags.
- **Android builds:** use `python3 scripts/build-android.py --abi all --prefab` with `ANDROID_NDK_HOME` set, or pass `--ndk`. Requires NDK r26+ and CMake 3.22+; `--cmake` selects a working SDK CMake on Termux.
  - `all` means all four ABIs. Output names are always `arm64-v8a`, `armeabi-v7a`, `x86`, `x86_64`; aliases only affect input parsing.
  - Build roots are `build/android/<configuration>/<static|shared>/<abi>`, outputs are `build-output/<configuration>/<static|shared>/android/<abi>`. Configuration names are lowercase. `--tests` builds device executables; `--clean-first` cleans selected CMake targets. Default is Release/static/API 21 with BionicLinkerRestriction enabled.
  - Workflows use this builder. Legacy `build-workspace/auto-build.sh` only forwards to Python with Prefab enabled; old output trees are left untouched. See `docs/build-documentation.md` for packaging, custom roots, archives and cleanup.
- For manual Android and Android Studio integration, read `docs/build-documentation.md`; cross-check recipes against CMake and scripts.

## Structure

- `include/dobby.h` — public C API (`DobbyHook`, `DobbyHookByName`, `DobbyPrepare`/`DobbyCommit`, `DobbyInstrument`, `DobbyEnumerateHooks`, `DobbySymbolResolver`, `DobbyDestroy`, `DobbyBuildVersion`, `DobbyQueryHook`, `DobbyGetLastError`, `DobbyGetResourceStats`, `DobbyCollectRetiredHooks`, `DobbyReleaseCodeMemory`). This is the library's installed public header. `DobbyHookByName` is implemented in `builtin-plugin/SymbolResolver/elf/`.
- `source/` — core: `core/arch`, `InstructionRelocation/{arm,arm64,x64,x86}`, `InterceptRouting`, `MemoryAllocator`, `TrampolineBridge`, `UserMode`. Per-arch files use suffixes `-{arm,arm64,x64,ia32}` (see `core_arch`/`arch1` vars in CMakeLists).
- `builtin-plugin/` — optional plugins enabled via `Plugin.*` CMake options: `SymbolResolver` (ELF), `BionicLinkerRestriction` (Android-only), `LinkerImageLoadCallback`.
- `external/` — vendored deps (`logging`, `xnucxx`, `misc-helper`) built via `add_subdirectory`.
- `example/` — standalone demos; not part of the main build.

## Conventions

- **C++23 / C23** (see top). The public header is `extern "C"`; do not add C++ to it.
- **STL is allowed for new code** (`std::vector`, `std::unique_ptr`, etc.). Existing hot-path, hand-rolled code (assembler, trampolines, allocators, the no-STL `xnucxx` helpers) is deliberately STL-free and performance/correctness-sensitive — do NOT rewrite it into STL casually. Prefer STL for new convenience/plugin code; keep the core hooking engine as-is unless there is a concrete bug.
- `.clang-format` = LLVM base, 2-space indent, 120 col, `ReflowComments: false`, `SortIncludes: false`. Keep `#include` order untouched.
- Heavy inline platform `#if defined(__arm64__)/__arm__/x86_64/...` guards; register-context code (`RegisterContext` in `dobby.h`) is arch-specific — see the "READ ME" comments about arm64 `q8–q31` needing `FullFloatingPointRegisterPack`.
- Thread-safety: a recursive operation mutex serializes hook setup, commit, destroy, and near-plugin toggles. Both memory arenas use statically initialized mutexes. Enumeration invokes callbacks on an address snapshot outside the lock. Patching is still not atomic against threads executing target code; patch at load/idle time.
- ARM64 scratch registers: relocated code and the closure bridge use X16/X17 (`TMP_REG_0`/`TMP_REG_1`) as scratch. DBI handlers must not rely on or clobber X16/X17 (see the note in `dobby.h`).
- ARM64 automatically tries a four-byte direct branch or owned near relay when NearBranch is enabled. Explicit BTI landing pads are preserved; PAC entry signing is relocated. Longer patches reject early RET/BR/B/authenticated returns conservatively; this is not a full boundary decoder. Near allocation uses owned pages only, never zero-filled regions in other mappings.
- `DobbyDestroy` retires metadata; collection requires caller-proven quiescence. Pool memory stays mapped until explicit `DobbyReleaseCodeMemory`, which requires no registered hooks or in-flight frames and invalidates all old trampoline pointers. Preserve/reset architecture caches during teardown. See `docs/api.md`.
- Preserve idempotence: `DobbyHook`/`DobbyPrepare` accept the same address and replacement; `DobbyCommit` accepts an already committed entry; `DobbyInstrument` accepts the same address and handler.

## Tests / verification

CI definitions are `.github/workflows/android-build.yml` (push/PR/manual builds) and `android-release.yml` (`v*` tags and release uploads). Build CI covers all four ABIs, Debug/Release and static/shared libraries, including device-test compilation. `android-device-tests.yml` manually runs tests on a self-hosted Android-device runner and uploads measured JSON. Hosted builds do not run Android binaries. Configuration files alone are not evidence that CI passes. The legacy `tests/CMakelists.txt` and `tests/InstructionRelocation/` project are stale and outside the main build; do not assume `ctest` works.

A **host smoke test** exists at `tests/core_smoke_test.cc` (local aarch64/Termux, links the host `libdobby.a`, exercises `DobbyHook`, `DobbyInstrument`, `DobbySymbolResolver`, `DobbyHookByName`, `DobbyEnumerateHooks`, `DobbyDestroy`). Run it with `sh tests/build_smoke_test.sh`. The script only builds the host library when `build/libdobby.a` is absent: rebuild the `dobby` target first after source edits to avoid testing a stale archive. It links `-llog` for Termux and is not an unchanged generic Linux recipe.

For API/lifecycle checks on all ABIs, build `dobby_api_compatibility_test` and `dobby_public_api_c_test` with `DOBBY_BUILD_TESTS=ON`. Run via `scripts/run-android-tests.py` (adb or `--local`). Use `--abi arm64-v8a --require-16k --require-bti --require-pac` only on a matching device; unavailable features otherwise report skipped.

For ARM64 regressions, configure with `-DDOBBY_BUILD_TESTS=ON` and build `dobby_hook_safety_test` (requires `DynamicBinaryInstrument=ON` and `NearBranch=ON`). Run the executable on an Android ARM64 device; the CTest entry is usable when the build host can execute Android ARM64 binaries. See `docs/build-documentation.md` for commands. Tests cover short-function boundaries, internal branches, concurrent setup, callback reentry, patch failures, and page permissions.

For code/build changes, verify a clean Android build for all four ABIs using the Python builder with `--abi all --clean-first` or fresh build directories. Check `clang-format` conformance on edited C/C++ files. Builds must be warning-free (C++23 warns on OR-ing distinct enums with `-Wdeprecated-enum-enum-conversion`; cast to `int` explicitly). For documentation-only changes, verify referenced paths and commands against their sources and run `git diff --check`; a rebuild is unnecessary. Report checks actually run and any unavailable verification.
