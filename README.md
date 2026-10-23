# Dobby for Android

An Android-focused fork of [Dobby](https://github.com/jmpews/Dobby), a runtime
hook framework for **arm64-v8a, armeabi-v7a, x86 and x86_64**. It builds a static
library by default, with a shared-library option, using C++23/C23.

- Inline hooks, staged prepare/commit and register-context instrumentation.
- Automatic ARM64 near trampolines for short methods, with conservative fallback checks.
- Versioned per-hook options, target analysis, patch coordination and batch rollback.
- ELF PLT/GOT import hooks with version selection/slot inspection, module pins and removable callbacks.
- Thread-local diagnostics, runtime capabilities, hook inspection and resource teardown.
- Capstone/Unicorn differential relocation tests, seeded fuzzing/replay and an optional benchmark.
- Optional ELF symbol resolution, linker-load callbacks and Bionic linker integration.
- CMake `Dobby::dobby` target and installable `find_package(Dobby CONFIG)` package.

## Build

Use CMake 3.22+ and a C++23-capable Android NDK (r26 or newer; validation uses r29):

```sh
cmake -S . -B build/arm64-v8a \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release
cmake --build build/arm64-v8a --target dobby --parallel 4
```

See [build and device tests](docs/build-documentation.md),
[API and migration guide](docs/api.md), [engine guide](docs/engine.md), [verification matrix](docs/compatibility.md),
and the public [header](include/dobby.h).
Android is the supported release target. The Linux CMake path is only a local
development convenience; POSIX/Linux implementation files are required by Android.

Install/remove hooks at a safe point when other threads are not executing the
patched bytes. A successful build alone does not establish runtime compatibility
with every Android version, page size, BTI/PAC configuration or IL2CPP prologue.

## Credits

Upstream Dobby and its contributors, plus projects that informed its implementation:
[frida-gum](https://github.com/frida/frida-gum),
[minhook](https://github.com/TsudaKageyu/minhook),
[substrate](https://github.com/jevinskie/substrate),
[V8](https://github.com/v8/v8), [Dart](https://github.com/dart-lang/sdk), and
[VIXL](https://github.com/Linaro/vixl).
