# AGENTS.md

Dobby: a C/C++ runtime hook framework, **Android-only** (targets arm64-v8a, armeabi-v7a, x86, x86_64). Builds a `dobby` static (default) or shared library from CMake. All Darwin/macOS/iOS/Windows code has been removed. The C++ is **C++23**, the C is **C23** (CMake sets both `CMAKE_CXX_STANDARD=23` / `CMAKE_C_STANDARD=23`).

> "Drop Linux" note: there is **no Linux host/target build**. The POSIX/Linux source files (`platform-posix.cc`, `code-patch-tool-posix.cc`, `ProcessRuntimeUtility/Linux/`) are kept and MUST remain — Android is a Linux-based OS and these are the actual Android implementation. Only the build targets are Android ABIs.

## Build

The whole build hinges on `cmake/EarlyGlobals.cmake`, which derives `PROCESSOR.*`/`SYSTEM.*` from `CMAKE_SYSTEM_PROCESSOR`/`CMAKE_SYSTEM_NAME`. These MUST be passed explicitly when cross-compiling or CMake errors ("Processor ... not configured"). `EarlyGlobals.cmake` is included twice (before AND after `project()`) — this is intentional for CMake >= 4.0; do not "fix" it.

- Host build (this environment, **local dev convenience only — not a supported artifact**): `mkdir build && cd build && cmake .. && make -j4 dobby`
- Always build the `dobby` target directly (`make dobby`); examples are commented out of the top-level build.
- Key options (see `CMakeLists.txt`): `DOBBY_GENERATE_SHARED`, `DOBBY_DEBUG`, `NearBranch`, `DynamicBinaryInstrument`, `FullFloatingPointRegisterPack`, `Plugin.SymbolResolver`, `Plugin.LinkerLoadCallback`, `Plugin.Android.BionicLinkerRestriction`. Note `NearBranch` is force-ON for x86/x64.
- `DOBBY_DEBUG` is auto-enabled unless `CMAKE_BUILD_TYPE=Release`. Debug builds add `-DDOBBY_DEBUG -DLOGGING_DEBUG` and skip release-stripping flags.
- **Android builds (use these, not the generic CMake recipe):**
  - Preferred: `~/.local/bin/build --abi arm64-v8a --no-strip` (a helper that wires up the NDK toolchain + `-DANDROID_STL=c++_static`). For all ABIs: `--abi all`. Run from the repo root.
  - All 4 ABIs + prefab copy: `ANDROID_NDK_HOME=<ndk> build-workspace/auto-build.sh`. Requires `ANDROID_NDK_HOME`, NOT `ANDROID_NDK`. It runs from the repo root and creates `auto-build-workspace/` (build dirs) and `auto-build-output/` (stripped libs) **at the repo root**, then copies `.a` files into `build-workspace/prefab/`. Note: there is also a stale `build-workspace/auto-build-workspace/` that is NOT used.
  - Both build with `Plugin.Android.BionicLinkerRestriction=ON`, `DOBBY_DEBUG=OFF`, `DOBBY_GENERATE_SHARED=OFF`.
  - Requires an NDK that supports C++23 (>= r26 / clang 17).
- Reference build recipes per OS/arch live in `docs/build-documentation.md`.

## Structure

- `include/dobby.h` — public C API (`DobbyHook`, `DobbyHookByName`, `DobbyPrepare`/`DobbyCommit`, `DobbyInstrument`, `DobbySymbolResolver`, `DobbyDestroy`, `DobbyBuildVersion`). This is the only public header. `DobbyHookByName` is implemented in `builtin-plugin/SymbolResolver/elf/`.
- `source/` — core: `core/arch`, `InstructionRelocation/{arm,arm64,x64,x86}`, `InterceptRouting`, `MemoryAllocator`, `TrampolineBridge`, `UserMode`. Per-arch files use suffixes `-{arm,arm64,x64,ia32}` (see `core_arch`/`arch1` vars in CMakeLists).
- `builtin-plugin/` — optional plugins enabled via `Plugin.*` CMake options: `SymbolResolver` (ELF), `BionicLinkerRestriction` (Android-only), `LinkerImageLoadCallback`.
- `external/` — vendored deps (`logging`, `xnucxx`, `misc-helper`) built via `add_subdirectory`.
- `example/` — standalone demos; not part of the main build.

## Conventions

- **C++23 / C23** (see top). The public header is `extern "C"`; do not add C++ to it.
- **STL is allowed for new code** (`std::vector`, `std::unique_ptr`, etc.). Existing hot-path, hand-rolled code (assembler, trampolines, allocators, the no-STL `xnucxx` helpers) is deliberately STL-free and performance/correctness-sensitive — do NOT rewrite it into STL casually. Prefer STL for new convenience/plugin code; keep the core hooking engine as-is unless there is a concrete bug.
- `.clang-format` = LLVM base, 2-space indent, 120 col, `ReflowComments: false`, `SortIncludes: false`. Keep `#include` order untouched.
- Heavy inline platform `#if defined(__arm64__)/__arm__/x86_64/...` guards; register-context code (`RegisterContext` in `dobby.h`) is arch-specific — see the "READ ME" comments about arm64 `q8–q31` needing `FullFloatingPointRegisterPack`.
- Thread-safety: `Interceptor` (hook registry) and both memory arenas (`MemoryArena`, `NearMemoryArena`) use an internal `pthread_mutex` to serialize concurrent hooks/allocations. Hook setup on a hot function is still not atomic w.r.t. other threads executing it — patch at load/idle time where possible.
- ARM64 scratch registers: relocated code and the closure bridge use X16/X17 (`TMP_REG_0`/`TMP_REG_1`) as scratch. DBI handlers must not rely on or clobber X16/X17 (see the note in `dobby.h`).
- `DobbyHook`/`DobbyCommit` are idempotent: re-hooking the same address with the same replacement succeeds as a no-op instead of failing.

## Tests / verification

There is NO CI test step. `.github/workflows/build.yml` is **out of date** — it still builds Windows/macOS, which have been removed from the tree; do not rely on it. The legacy `tests/InstructionRelocation/` CMake project is not part of the main build and is **stale** (its `GenRelocateCodeAndBranch` signature no longer matches) — do not assume `ctest` works.

A working **host smoke test** exists at `tests/core_smoke_test.cc` (builds on Linux/aarch64, links the host `libdobby.a`, exercises `DobbyHook`, `DobbyInstrument`, `DobbySymbolResolver`, `DobbyHookByName`, `DobbyEnumerateHooks`, `DobbyDestroy`). Run it with `sh tests/build_smoke_test.sh`. Note Termux defines `__ANDROID__`, so linking the host lib requires `-llog`.

Verification for a change = a clean Android build (`~/.local/bin/build --abi all --no-strip` or `ANDROID_NDK_HOME=<ndk> build-workspace/auto-build.sh`) for all 4 ABIs, plus `clang-format` conformance on edited files. The build must be warning-free (C++23 turns OR-ing distinct enums into `-Wdeprecated-enum-enum-conversion`; cast to `int` explicitly).
