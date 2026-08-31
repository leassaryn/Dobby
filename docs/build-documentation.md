# Build

Dobby targets **Android only** (ABIs: arm64-v8a, armeabi-v7a, x86, x86_64). It builds with **C++23** (`.cpp`) and **C23** (`.c`).

## CMake build options

```
option(DOBBY_GENERATE_SHARED "Build shared library" OFF)

option(DOBBY_DEBUG "Enable debug logging" OFF)

option(NearBranch "Enable Near Branch Trampoline" ON)

option(DynamicBinaryInstrument "Enable Dynamic Binary Instrument" ON)

option(FullFloatingPointRegisterPack "Save and pack all floating-point registers" ON)

option(Plugin.SymbolResolver "Resolve symbol by [DobbySymbolResolver] " ON)

option(Plugin.LinkerLoadCallback "Register image load callback " ON)

option(Plugin.Android.BionicLinkerRestriction "Enable android bionic linker restriction" OFF)
```

## Build script (preferred for Android)

`build-workspace/auto-build.sh` builds static libs for all 4 ABIs
(arm64-v8a, armeabi-v7a, x86, x86_64) and copies them into `build-workspace/prefab/`.

Requires `ANDROID_NDK_HOME` (not `ANDROID_NDK`), and an NDK >= r26 for C++23:

```
ANDROID_NDK_HOME=<ndk> build-workspace/auto-build.sh
```

## Build for Android (manual)

```
export ANDROID_NDK=/path/to/android-sdk/ndk/<version>

cd Dobby && mkdir build_for_android_arm64 && cd build_for_android_arm64

cmake .. \
-DCMAKE_BUILD_TYPE=Release \
-DCMAKE_SYSTEM_NAME=Android -DCMAKE_ANDROID_ARCH_ABI="arm64-v8a" -DCMAKE_ANDROID_NDK=$ANDROID_NDK -DCMAKE_SYSTEM_VERSION=21 -DCMAKE_ANDROID_NDK_TOOLCHAIN_VERSION=clang

make -j4 dobby
```

For ARM (32-bit) replace `arm64-v8a` with `armeabi-v7a` (and `-DCMAKE_SYSTEM_VERSION=16`).

## Android Studio CMake

```
if(NOT TARGET dobby)
set(DOBBY_DIR /path/to/Dobby)
macro(SET_OPTION option value)
  set(${option} ${value} CACHE INTERNAL "" FORCE)
endmacro()
SET_OPTION(DOBBY_DEBUG OFF)
SET_OPTION(DOBBY_GENERATE_SHARED OFF)
add_subdirectory(${DOBBY_DIR} dobby)
get_property(DOBBY_INCLUDE_DIRECTORIES
  TARGET dobby
  PROPERTY INCLUDE_DIRECTORIES)
include_directories(
  .
  ${DOBBY_INCLUDE_DIRECTORIES}
  $<TARGET_PROPERTY:dobby,INCLUDE_DIRECTORIES>
)
endif()

add_library(native-lib SHARED
  ${DOBBY_DIR}/example/android_common_api.cc

  native-lib.cpp)
```
