# Getting Started

## create native project and update CMakeLists.txt

```
if(NOT TARGET dobby)
set(DOBBY_DIR /Users/jmpews/Workspace/Project.wrk/Dobby)
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

## replace hook function

```
extern "C" {
  extern int DobbyHook(void *function_address, void *replace_call, void **origin_call);
}

size_t (*origin_fread)(void *ptr, size_t size, size_t nitems, FILE *stream);

size_t fake_fread(void *ptr, size_t size, size_t nitems, FILE *stream) {
    // Do what you want.
    return origin_fread(ptr, size, nitems, stream);
}

DobbyHook((void *)fread, (void *)fake_fread, (void **)&origin_fread);
```

## instrument function

```
// read the first argument from the register context (arch-specific)
uintptr_t getCallFirstArg(RegisterContext *ctx) {
#if defined(__arm64__) || defined(__aarch64__)
  return ctx->general.regs.x0;
#elif defined(__arm__)
  return ctx->general.regs.r0;
#elif defined(__x86_64__)
  return ctx->general.regs.rdi;
#else
#error "Not Support Architecture."
#endif
}

void malloc_handler(RegisterContext *ctx, const HookEntryInfo *info) {
  // NOTE: printf calls malloc internally and will recurse; use puts instead.
  puts("malloc called");
}

DobbyInstrument((void *)malloc, malloc_handler)
```

## Android Linker Restriction

impl at `builtin-plugin/BionicLinkerRestriction/bionic_linker_restriction.cc`

```c
// impl at SymbolResolver/elf/dobby_symbol_resolver.cc
void *__loader_dlopen = DobbySymbolResolver(NULL, "__loader_dlopen");
DobbyHook((void *)__loader_dlopen, (void *)fake_loader_dlopen, (void **)&orig_loader_dlopen);
```

```c
// impl at BionicLinkerRestriction/bionic_linker_restriction.cc
linker_disable_namespace_restriction();
void *handle = NULL;
handle       = dlopen(lib, RTLD_LAZY);
vm           = dlsym(handle, "_ZN7android14AndroidRuntime7mJavaVME");
```