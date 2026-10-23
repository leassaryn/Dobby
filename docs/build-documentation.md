# Build and verification

Supported artifacts target Android: arm64-v8a, armeabi-v7a, x86 and x86_64.
Use CMake 3.22+, C++23/C23 and NDK r26 or newer. Current validation uses r29.
The NDK toolchain supplies the system/processor values required by
`cmake/EarlyGlobals.cmake`.

## Configure and build

```sh
cmake -S . -B build/arm64-v8a \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release \
  -DDOBBY_GENERATE_SHARED=OFF -DDOBBY_DEBUG=OFF \
  -DPlugin.Android.BionicLinkerRestriction=ON
cmake --build build/arm64-v8a --target dobby --parallel 4
```

Repeat with each ABI and a distinct build directory. Set
`DOBBY_GENERATE_SHARED=ON` for `libdobby.so` instead of `libdobby.a`.
Other options: `NearBranch`, `DynamicBinaryInstrument`,
`FullFloatingPointRegisterPack`, `Plugin.SymbolResolver`,
`Plugin.LinkerLoadCallback`. These default ON; BionicLinkerRestriction defaults
OFF. NearBranch is forced ON for x86/x64. Debug builds enable debug logging.

`ANDROID_NDK_HOME=<ndk> sh build-workspace/auto-build.sh` builds all four static
libraries. Run from the repository root: outputs go to `auto-build-workspace/`
and `auto-build-output/`, with copies in root-level `prefab/`. The script replaces
its summary output directory; it does not strip archives.

The external Termux helper `~/.local/bin/build` can also configure the toolchain.
Its explicit four-ABI selection is `--abi arm64-v8a,armeabi-v7a,x86,x86_64`;
its `--abi all` currently selects only ARM ABIs.

## Integrate with an application

Inside an Android CMake project:

```cmake
add_subdirectory(path/to/Dobby dobby-build)
target_link_libraries(native-lib PRIVATE Dobby::dobby)
```

Or install one ABI/configuration into its own prefix:

```sh
cmake --install build/arm64-v8a --prefix "$PWD/build/install/arm64-v8a"
```

Configure the consuming Android project with
`-DDobby_DIR=<prefix>/lib/cmake/Dobby`. The NDK restricts package searches to its
root paths, so `CMAKE_PREFIX_PATH` alone may not locate an external install:

```cmake
find_package(Dobby CONFIG REQUIRED)
target_link_libraries(native-lib PRIVATE Dobby::dobby)
```

The target exports the public header directory and link requirements. Do not copy
Dobby's private include paths. The 64-bit target adds a 16 KB maximum ELF page
alignment link option, also propagated to consumers. APK packaging and all other
native dependencies still need independent checks; see Android's
[16 KB page-size guide](https://developer.android.com/guide/practices/page-sizes)
and the [NDK r28 changes](https://github.com/android/ndk/wiki/Changelog-r28).

## Runtime tests

Add `-DDOBBY_BUILD_TESTS=ON` and build these targets:

- `dobby_api_compatibility_test`: real hook/original calls, thread-local errors,
  prepared/committed snapshots, retirement, teardown and reinstall. ARM includes
  ARM and Thumb fixtures; ARM64 with NearBranch includes short adjacent methods,
  loader stubs, BTI landing pads and PAC prologues.
- `dobby_linker_callback_test`: lazy registration, callback deduplication, reentry
  and successful/failed real `dlopen` calls (when the plugin is enabled).
- `dobby_public_api_c_test`: C23 header and diagnostic/resource API linkage.
- `dobby_hook_safety_test`: ARM64 with DBI and NearBranch; internal branches,
  inaccessible page boundaries, conflicting/concurrent setup, callback reentry,
  patch failures and restoration of different page permissions.

Build executable targets in `build/<abi>` directories, then run on an attached
authorized Android device:

```sh
python3 scripts/run-android-tests.py --build-root build \
  --output build/android-test-results.json
```

Use `--local` inside Android/Termux or `--serial <adb-serial>` with adb. The runner
executes supported ABIs and reports unavailable ones as skipped. An explicitly
requested unsupported `--abi` fails. JSON records actual page size, feature
coverage, exit status and logs. Cross-compiling executables does not run them.
CTest is useful only where those Android executables can execute locally.

Require actual feature coverage on a suitable ARM64 device:

```sh
python3 scripts/run-android-tests.py --build-root build --abi arm64-v8a \
  --require-16k --require-bti --require-pac \
  --output build/android-required-features.json
```

These flags fail when required capabilities are absent. Without them, unavailable
BTI/PAC tests are reported as skipped. Hardware capability discovery follows
[Linux ARM64 HWCAP](https://www.kernel.org/doc/html/v6.13/arch/arm64/elf_hwcaps.html).
Patching preserves BTI/MTE flags reported by
[`/proc/self/smaps`](https://www.kernel.org/doc/html/v6.8/filesystems/proc.html).
A passing fixture covers the sequences tested, not every compiler-generated
prologue or instruction extension.

CI build workflow covers all ABIs, Debug/Release and static/shared libraries and
builds device executables. The manual `android-device-tests.yml` workflow requires
a self-hosted runner labeled `android-device`, adb, `ANDROID_NDK_HOME`, CMake,
Python and an attached device. It uploads measured JSON. Hosted build jobs do not
claim runtime coverage. The legacy `tests/InstructionRelocation/` project remains
outside this supported test path.

## Sanitizers

Use a separate ARM64 build with these additional CMake options:

```sh
-DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize=function -fno-omit-frame-pointer"
-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize=function -fno-omit-frame-pointer"
-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined
```

Run the compatibility and safety executables with the NDK ASan runtime directory
on `LD_LIBRARY_PATH`, `ASAN_OPTIONS=detect_leaks=0` and
`UBSAN_OPTIONS=halt_on_error=1`. Generated code has no compiler function-type
metadata, so the function-type check is excluded. LeakSanitizer is unsupported
on the Android environment used here; this is memory-access/UB validation, not a
heap leak certification. Resource tests separately verify pool counters return
to zero and hooks work again after explicit teardown.

See [API contracts](api.md) for thread safety, pointer lifetimes, error handling
and safe collection requirements.

## Capstone + Unicorn relocation checks

`DOBBY_BUILD_RELOCATION_EMULATOR_TESTS=ON` adds a native ARM64 differential
test against the real relocation generator, with Capstone/Unicorn discovered via
`pkg-config`. It compares full relevant CPU state and data/stack memory, checks
internal branches against poisoned original bytes, and includes a mutation
control. Build with NearBranch ON and OFF for near/far placement coverage.
See [the relocation test guide](../tests/relocation/README.md). This test-only
option is OFF by default and is separate from the NDK/device build matrix.


## Modern API tests and benchmark

The normal test build includes `dobby_modern_api_test` and
`libdobby_test_importer.so`. Keep the fixture beside the executable when deploying;
the device runner copies it automatically. Coverage includes versioned options,
batch prepare/commit rollback, module pins, real RELRO import slots and callback
ownership. Static tests additionally inject commit/rollback mprotect failures.

Configure `-DDOBBY_BUILD_BENCHMARKS=ON`, build `dobby_hook_benchmark`, and execute it
on the target Android device. It outputs JSON with median baseline/inline/DBI
call time, installation lifecycle time, near allocation outcomes and retained
pool capacity. Results depend on device load, compiler and power state; they are
measurements, not performance guarantees. Capability detection is also available
through `DobbyGetRuntimeInfo` without running a benchmark.

The optional native emulator configuration also builds the x86/x64 relocation
tests. Run CTest for the fixed corpus and use `--cases`/`--seed`/`--replay` on the
ARM64 executable for reproducible fuzzing; see its README. These dependencies
remain absent from production library linkage.


The optional emulator configuration now also requires Python and `llvm-mc` for
the ARM64 encoding oracle, and builds an ARM/Thumb guest test. Production library
linkage is unchanged. See [engine contracts and coverage](engine.md).
