cmake_minimum_required(VERSION 3.22)
include(cmake/EarlyGlobals.cmake)

project(Dobby LANGUAGES C CXX ASM)

# CMake >= 4.0 populates CMAKE_SYSTEM_NAME / CMAKE_SYSTEM_PROCESSOR only
# during project(); re-run detection to pick up toolchain-file values.
include(cmake/EarlyGlobals.cmake)

include(cmake/Util.cmake)
include(cmake/Globals.cmake)
include(cmake/Macros.cmake)

# C++23 / C23 (modern STL). C++23 requires a recent compiler/NDK (>= r26);
# the project's toolchain and vendored headers are C++17-clean and build
# without warnings under -std=gnu++23.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ===== Handle Option =====
option(DOBBY_GENERATE_SHARED "Build shared library" OFF)

option(DOBBY_DEBUG "Enable debug logging" OFF)

option(NearBranch "Enable Near Branch Trampoline" ON)

option(DynamicBinaryInstrument "Enable Dynamic Binary Instrument" ON)

option(FullFloatingPointRegisterPack "Save and pack all floating-point registers" ON)

option(EnableObfuscation "Enable llvm obfuscation" OFF)

option(Plugin.SymbolResolver "Resolve symbol by [DobbySymbolResolver] " ON)

option(Plugin.LinkerLoadCallback "Register image load callback " ON)

option(Plugin.Android.BionicLinkerRestriction "Enable android bionic linker restriction" OFF)

# Use native assembly bridge to replace the runtime codegen
# if(CLOSURE_BRIDGE_TEMPLATE)
#   SET(CMAKE_ASM_FLAGS "${CMAKE_C_FLAGS}")
#   enable_language(ASM)
#   add_definitions(-DENABLE_CLOSURE_BRIDGE_TEMPLATE)
# endif()

# Enable debug will log more information
if ((NOT DEFINED CMAKE_BUILD_TYPE) OR (CMAKE_BUILD_TYPE STREQUAL "Debug"))
  message(STATUS "[Dobby] Enabling debug logging because CMAKE_BUILD_TYPE is Debug or undefined")
  set(DOBBY_DEBUG ON)
endif()
if(DOBBY_DEBUG)
  add_definitions(-DDOBBY_DEBUG)
  add_definitions(-DLOGGING_DEBUG)
  message(STATUS "[Dobby] Enable debug logging")
endif()

# Enable full floating point register pack
# for arm64, allow access q8 - q31
if(FullFloatingPointRegisterPack)
  add_definitions(-DFULL_FLOATING_POINT_REGISTER_PACK)
  message(STATUS "[Dobby] Save and pack all floating-point registers")
endif()

# Android is the release target; Linux is accepted only for local smoke tests.
if(SYSTEM.Android)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fomit-frame-pointer")
  if(NOT DOBBY_DEBUG)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffunction-sections -fdata-sections")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections -Wl,--exclude-libs,ALL")
  endif()
elseif(SYSTEM.Linux)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
else()
  message(FATAL_ERROR "[!] Dobby only supports Android/Linux (got ${CMAKE_SYSTEM_NAME})")
endif()

if(COMPILER.Clang)
  if(NOT DOBBY_DEBUG)
    # set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-rtti -fvisibility=hidden -fvisibility-inlines-hidden")
  endif()
  if(PROCESSOR.ARM)
    set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -arch armv7 -x assembler-with-cpp")
  elseif(PROCESSOR.AARCH64)
    set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -arch arm64 -x assembler-with-cpp")
  endif()
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS}")

message(STATUS "CMAKE_C_COMPILER: ${CMAKE_C_COMPILER}")
message(STATUS "CMAKE_CXX_COMPILER: ${CMAKE_CXX_COMPILER}")
message(STATUS "CMAKE_C_FLAGS: ${CMAKE_C_FLAGS}")
message(STATUS "CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")
message(STATUS "CMAKE_SHARED_LINKER_FLAGS: ${CMAKE_SHARED_LINKER_FLAGS}")

# arch prefix
if(PROCESSOR.ARM)
  set(ARCH1 ARM)
  set(arch1 arm)
  set(core_arch arm)
elseif(PROCESSOR.AARCH64)
  set(ARCH1 ARM64)
  set(arch1 arm64)
  set(core_arch arm64)
elseif(PROCESSOR.X86)
  set(ARCH1 X86)
  set(arch1 x86)
  set(core_arch ia32)
elseif(PROCESSOR.X86_64)
  set(ARCH1 X64)
  set(arch1 x64)
  set(core_arch x64)
else()
endif()

# system prefix (Android/Linux only)
if(SYSTEM.Linux OR SYSTEM.Android)
  set(platform1 posix)
  set(platform2 Linux)
else()
  message(FATAL_ERROR "[!] Dobby only supports Android/Linux (got ${CMAKE_SYSTEM_NAME})")
endif()

include_directories(
  .

  ./include

  ./source
  ./source/include
  ./source/UserMode

  ./external
  ./external/logging
  ./external/xnucxx
  ./external/misc-helper

  builtin-plugin
)

set(DOBBY_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(dobby.SOURCE_FILE_LIST ${dobby.SOURCE_FILE_LIST}
  # cpu
  source/core/arch/CpuFeature.cc
  source/core/arch/CpuRegister.cc

  # assembler
  source/core/modules/assembler/assembler.cc
  source/core/modules/assembler/assembler-${core_arch}.cc

  # codegen
  source/core/modules/codegen/codegen-${core_arch}.cc

  # memory kit
  source/MemoryAllocator/CodeBuffer/CodeBufferBase.cc
  source/MemoryAllocator/CodeBuffer/code-buffer-${arch1}.cc
  source/MemoryAllocator/AssemblyCodeBuilder.cc
  source/MemoryAllocator/MemoryArena.cc

  # instruction relocation
  source/InstructionRelocation/${arch1}/${ARCH1}InstructionRelocation.cc

  # intercept routing
  source/InterceptRouting/InterceptRouting.cpp

  # intercept routing trampoline
  source/TrampolineBridge/Trampoline/${arch1}/trampoline-${arch1}.cc

  # intercept routing plugin (buildin)
  source/InterceptRouting/Routing/FunctionInlineReplace/function-inline-replace.cc
  source/InterceptRouting/Routing/FunctionInlineReplace/FunctionInlineReplaceExport.cc

  # plugin register
  source/InterceptRouting/RoutingPlugin/RoutingPlugin.cc

  # platform util
  source/UserMode/PlatformUtil/${platform2}/ProcessRuntimeUtility.cc

  # user mode - platform interface
  source/UserMode/UnifiedInterface/platform-${platform1}.cc

  # user mode - executable memory
  source/UserMode/ExecMemory/code-patch-tool-${platform1}.cc
  source/UserMode/ExecMemory/clear-cache-tool-all.c

  # main
  source/ModernAPI.cpp
  source/TargetAnalysis.cpp
  source/ModuleImport.cpp
  source/HookLifecycle.cpp
  source/DobbyError.cpp
  source/dobby.cpp
  source/Interceptor.cpp
  )

if (PROCESSOR.X86_64 OR PROCESSOR.X86)
  list(APPEND dobby.SOURCE_FILE_LIST source/InstructionRelocation/x86/x86_insn_decode/x86_insn_decode.c)
  set(NearBranch ON)
  if (PROCESSOR.X86_64)
    add_definitions(-DDETOURS_X64)
  endif()
endif()

if(FunctionWrapper OR DynamicBinaryInstrument)
  set(dobby.SOURCE_FILE_LIST ${dobby.SOURCE_FILE_LIST}
    # closure trampoline bridge
    source/TrampolineBridge/ClosureTrampolineBridge/common-bridge-handler.cc
    source/TrampolineBridge/ClosureTrampolineBridge/${arch1}/helper-${arch1}.cc
    source/TrampolineBridge/ClosureTrampolineBridge/${arch1}/closure-bridge-${arch1}.cc
    source/TrampolineBridge/ClosureTrampolineBridge/${arch1}/${ARCH1}AssemblyClosureTrampoline.cc

    # user mode - multi thread support
    # source/UserMode/MultiThreadSupport/ThreadSupport.cpp
    # source/UserMode/Thread/PlatformThread.cc
    # source/UserMode/Thread/platform-thread-${platform1}.cc
    )
endif()

if(FunctionWrapper)
  message(FATAL_ERROR "[!] FunctionWrapper plugin is not supported")
endif()

if(DynamicBinaryInstrument)
  add_definitions(-DDOBBY_HAS_DBI)
  message(STATUS "[Dobby] Enable dynamic binary instrument(hook instruction with register context)")
  set(dobby.SOURCE_FILE_LIST ${dobby.SOURCE_FILE_LIST}
    source/InterceptRouting/Routing/DynamicBinaryInstrument/dynamic-binary-instrument.cc
    source/InterceptRouting/Routing/DynamicBinaryInstrument/DynamicBinaryInstrumentExport.cc
    source/InterceptRouting/Routing/DynamicBinaryInstrument/intercept_routing_handler.cc
    )
endif()

if(NearBranch)
  add_definitions(-DDOBBY_HAS_NEAR_BRANCH)
  message(STATUS "[Dobby] Enable near branch trampoline(trampoline within single instruction)")
  set(dobby.SOURCE_FILE_LIST ${dobby.SOURCE_FILE_LIST}
    source/InterceptRouting/RoutingPlugin/NearBranchTrampoline/NeaBranchTrampoline.cc
    source/MemoryAllocator/NearMemoryArena.cc)
endif()

add_subdirectory(external/misc-helper)
get_target_property(misc_helper.SOURCE_FILE_LIST misc_helper SOURCES)

# add logging library
add_subdirectory(external/logging)
get_target_property(logging.SOURCE_FILE_LIST logging SOURCES)

# add xnucxx library
add_subdirectory(external/xnucxx)
get_target_property(xnucxx.SOURCE_FILE_LIST xnucxx SOURCES)

if(Plugin.SymbolResolver)
  message(STATUS "[Dobby] Enable symbol resolver")

  include_directories(builtin-plugin/SymbolResolver)
  add_subdirectory(builtin-plugin/SymbolResolver)
  get_target_property(symbol_resolver.SOURCE_FILE_LIST symbol_resolver SOURCES)
  set(dobby.plugin.SOURCE_FILE_LIST ${dobby.plugin.SOURCE_FILE_LIST}
    ${symbol_resolver.SOURCE_FILE_LIST}
    )
endif()

if(Plugin.Android.BionicLinkerRestriction)
  if(NOT SYSTEM.Android)
    message(FATAL_ERROR "[!] Plugin.Android.BionicLinkerRestriction only works on Android.")
  endif()
  message(STATUS "[Dobby] Enable Plugin.Android.BionicLinkerRestriction")
  set(dobby.plugin.SOURCE_FILE_LIST ${dobby.plugin.SOURCE_FILE_LIST}
    builtin-plugin/BionicLinkerRestriction/bionic_linker_restriction.cc
  )
endif()

if(Plugin.LinkerLoadCallback AND NOT Plugin.SymbolResolver)
  message(FATAL_ERROR "Plugin.LinkerLoadCallback requires Plugin.SymbolResolver")
endif()

if(Plugin.LinkerLoadCallback)
  add_definitions(-DDOBBY_HAS_LINKER_CALLBACK)
  set(dobby.plugin.SOURCE_FILE_LIST ${dobby.plugin.SOURCE_FILE_LIST}
    builtin-plugin/LinkerImageLoadCallback/linker_load_callback.cc
    )
endif()

set(dobby.HEADER_FILE_LIST
  include/dobby.h
  )

# add build version
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
  execute_process(
    COMMAND git rev-parse --short --verify HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE VERSION_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(VERSION_COMMIT_HASH)
    set(VERSION_REVISION "${VERSION_COMMIT_HASH}")
  endif()
endif()
set(DOBBY_BUILD_VERSION "Dobby${VERSION_REVISION}")
add_definitions(-D__DOBBY_BUILD_VERSION__="${DOBBY_BUILD_VERSION}")
message(STATUS "[Dobby] ${DOBBY_BUILD_VERSION}")

if(DOBBY_GENERATE_SHARED)
  message(STATUS "[Dobby] Generate shared library")
  set(DOBBY_LIBRARY_TYPE SHARED)
else()
  message(STATUS "[Dobby] Generate static library")
  set(DOBBY_LIBRARY_TYPE STATIC)
endif()
add_library(dobby ${DOBBY_LIBRARY_TYPE} ${dobby.HEADER_FILE_LIST} ${dobby.SOURCE_FILE_LIST} ${logging.SOURCE_FILE_LIST} ${xnucxx.SOURCE_FILE_LIST} ${dobby.plugin.SOURCE_FILE_LIST})

add_library(Dobby::dobby ALIAS dobby)
include(GNUInstallDirs)
target_include_directories(dobby PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(dobby PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(SYSTEM.Android AND (PROCESSOR.AARCH64 OR PROCESSOR.X86_64))
  # PUBLIC also propagates to consumers of the static archive. Their final ELF,
  # not the archive itself, must satisfy Android's 16 KB loader requirements.
  target_link_options(dobby PUBLIC "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
endif()

if(EnableObfuscation)
set(linker_flags "${linker_flags} -Wl,-mllvm -Wl,-obfuscator-conf=all")
endif()
set_target_properties(dobby
  PROPERTIES LINK_FLAGS "${linker_flags}"
  )
if(SYSTEM.Android)
  target_link_libraries(dobby log)
endif()

if(SYSTEM.Linux)
  target_link_libraries(dobby dl)
endif()

install(TARGETS dobby EXPORT DobbyTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/dobby.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT DobbyTargets NAMESPACE Dobby:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Dobby)
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/DobbyConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/DobbyConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Dobby)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/DobbyConfig.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Dobby)

option(DOBBY_BUILD_TESTS "Build on-device Android regression tests" OFF)
if(DOBBY_BUILD_TESTS)
  enable_testing()
  find_package(Threads REQUIRED)
  add_library(dobby_test_importer SHARED tests/import_fixture.c)
  target_link_options(dobby_test_importer PRIVATE -Wl,-z,relro,-z,now -Wl,--pack-dyn-relocs=none)
  add_executable(dobby_modern_api_test tests/modern_api_test.cc)
  target_link_libraries(dobby_modern_api_test PRIVATE dobby dl)
  if(NOT DOBBY_GENERATE_SHARED)
    target_compile_definitions(dobby_modern_api_test PRIVATE DOBBY_TEST_PATCH_FAILURES)
    target_link_options(dobby_modern_api_test PRIVATE -Wl,--wrap=mprotect)
  endif()
  add_dependencies(dobby_modern_api_test dobby_test_importer)
  add_test(NAME dobby_modern_api COMMAND dobby_modern_api_test)
  set_tests_properties(dobby_modern_api PROPERTIES TIMEOUT 60)
  add_executable(dobby_api_compatibility_test tests/api_compatibility_test.cc)
  target_link_libraries(dobby_api_compatibility_test PRIVATE dobby Threads::Threads dl)
  target_compile_options(dobby_api_compatibility_test PRIVATE -Wall -Wextra -Wpedantic)
  if(DynamicBinaryInstrument)
    target_compile_definitions(dobby_api_compatibility_test PRIVATE DOBBY_TEST_DBI)
  endif()
  if(NearBranch)
    target_compile_definitions(dobby_api_compatibility_test PRIVATE DOBBY_TEST_NEAR)
  endif()
  add_test(NAME dobby_api_compatibility COMMAND dobby_api_compatibility_test)
  add_executable(dobby_public_api_c_test tests/public_api_c_test.c)
  target_link_libraries(dobby_public_api_c_test PRIVATE dobby)
  add_test(NAME dobby_public_api_c COMMAND dobby_public_api_c_test)
  set_tests_properties(dobby_api_compatibility dobby_public_api_c PROPERTIES TIMEOUT 60)
  if(Plugin.LinkerLoadCallback)
    add_executable(dobby_linker_callback_test tests/linker_callback_test.cc)
    target_link_libraries(dobby_linker_callback_test PRIVATE dobby dl)
    add_test(NAME dobby_linker_callback COMMAND dobby_linker_callback_test)
    set_tests_properties(dobby_linker_callback PROPERTIES TIMEOUT 60)
  endif()
  if(PROCESSOR.AARCH64 AND DynamicBinaryInstrument AND NearBranch)
    add_executable(dobby_hook_safety_test tests/hook_safety_test.cc)
    target_link_libraries(dobby_hook_safety_test PRIVATE dobby Threads::Threads dl)
    add_test(NAME dobby_hook_safety COMMAND dobby_hook_safety_test)
    set_tests_properties(dobby_hook_safety PROPERTIES TIMEOUT 60)
  endif()
endif()

option(DOBBY_BUILD_RELOCATION_EMULATOR_TESTS "Build native ARM64 Capstone/Unicorn differential tests" OFF)
if(DOBBY_BUILD_RELOCATION_EMULATOR_TESTS)
  if(NOT PROCESSOR.AARCH64 OR DOBBY_GENERATE_SHARED OR CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Relocation emulator tests currently require a native ARM64 static Dobby build")
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(CAPSTONE REQUIRED IMPORTED_TARGET capstone>=5)
  pkg_check_modules(UNICORN REQUIRED IMPORTED_TARGET unicorn>=2)
  enable_testing()
  add_executable(dobby_relocation_emulator_test tests/relocation/arm64_emulator_test.cc)
  target_link_libraries(dobby_relocation_emulator_test PRIVATE dobby PkgConfig::CAPSTONE PkgConfig::UNICORN)
  # A Termux native compiler defines __ANDROID__ even in the local Linux CMake path.
  include(CheckCXXSymbolExists)
  check_cxx_symbol_exists(__ANDROID__ "" DOBBY_EMULATOR_ANDROID)
  if(DOBBY_EMULATOR_ANDROID)
    target_link_libraries(dobby_relocation_emulator_test PRIVATE log)
  endif()
  if(NOT NearBranch)
    target_compile_definitions(dobby_relocation_emulator_test PRIVATE DOBBY_EMULATOR_FAR)
  endif()
  target_compile_options(dobby_relocation_emulator_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME dobby_relocation_emulator COMMAND dobby_relocation_emulator_test)
  set_tests_properties(dobby_relocation_emulator PROPERTIES TIMEOUT 120)
endif()

option(DOBBY_BUILD_BENCHMARKS "Build on-device hook overhead/resource benchmark" OFF)
if(DOBBY_BUILD_BENCHMARKS)
  add_executable(dobby_hook_benchmark tests/hook_benchmark.cc)
  target_link_libraries(dobby_hook_benchmark PRIVATE dobby)
  if(DynamicBinaryInstrument)
    target_compile_definitions(dobby_hook_benchmark PRIVATE DOBBY_BENCH_DBI)
  endif()
endif()

if(DOBBY_BUILD_RELOCATION_EMULATOR_TESTS)
  foreach(isa IN ITEMS x86 x64)
    if(isa STREQUAL "x86")
      set(test_arch ia32)
      set(test_arch_upper X86)
      set(test_arch_define DOBBY_TEST_TARGET_IA32)
    else()
      set(test_arch x64)
      set(test_arch_upper X64)
      set(test_arch_define DOBBY_TEST_TARGET_X64)
    endif()
    add_executable(dobby_relocation_${isa}_test
      tests/relocation/x86_emulator_test.cc
      source/DobbyError.cpp
      source/core/arch/CpuRegister.cc
      source/core/modules/assembler/assembler.cc
      source/core/modules/assembler/assembler-${test_arch}.cc
      source/core/modules/codegen/codegen-${test_arch}.cc
      source/MemoryAllocator/AssemblyCodeBuilder.cc
      source/MemoryAllocator/MemoryArena.cc
      source/MemoryAllocator/NearMemoryArena.cc
      source/MemoryAllocator/CodeBuffer/CodeBufferBase.cc
      source/MemoryAllocator/CodeBuffer/code-buffer-${isa}.cc
      source/InstructionRelocation/${isa}/${test_arch_upper}InstructionRelocation.cc
      source/InstructionRelocation/x86/x86_insn_decode/x86_insn_decode.c
      source/UserMode/PlatformUtil/Linux/ProcessRuntimeUtility.cc
      source/UserMode/UnifiedInterface/platform-posix.cc
      source/UserMode/ExecMemory/code-patch-tool-posix.cc
      source/UserMode/ExecMemory/clear-cache-tool-all.c
      ${logging.SOURCE_FILE_LIST} ${xnucxx.SOURCE_FILE_LIST})
    target_compile_definitions(dobby_relocation_${isa}_test PRIVATE ${test_arch_define})
    target_link_libraries(dobby_relocation_${isa}_test PRIVATE PkgConfig::CAPSTONE PkgConfig::UNICORN dl)
    if(DOBBY_EMULATOR_ANDROID)
      target_link_libraries(dobby_relocation_${isa}_test PRIVATE log)
    endif()
    add_test(NAME dobby_relocation_${isa} COMMAND dobby_relocation_${isa}_test)
    set_tests_properties(dobby_relocation_${isa} PROPERTIES TIMEOUT 60)
  endforeach()
endif()

if(DOBBY_BUILD_RELOCATION_EMULATOR_TESTS)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_program(DOBBY_LLVM_MC llvm-mc REQUIRED)
  add_executable(dobby_assembler_test tests/relocation/assembler_test.cc)
  target_link_libraries(dobby_assembler_test PRIVATE dobby)
  if(DOBBY_EMULATOR_ANDROID)
    target_link_libraries(dobby_assembler_test PRIVATE log)
  endif()
  add_test(NAME dobby_assembler_llvm COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/relocation/check_llvm_encoding.py
    $<TARGET_FILE:dobby_assembler_test> --llvm-mc ${DOBBY_LLVM_MC})
endif()
if(DOBBY_BUILD_RELOCATION_EMULATOR_TESTS)
  add_executable(dobby_relocation_arm_test tests/relocation/arm_emulator_test.cc
    source/DobbyError.cpp source/core/arch/CpuRegister.cc
    source/core/modules/assembler/assembler.cc source/core/modules/assembler/assembler-arm.cc
    source/core/modules/codegen/codegen-arm.cc source/MemoryAllocator/AssemblyCodeBuilder.cc
    source/MemoryAllocator/MemoryArena.cc source/MemoryAllocator/CodeBuffer/CodeBufferBase.cc
    source/MemoryAllocator/CodeBuffer/code-buffer-arm.cc
    source/InstructionRelocation/arm/ARMInstructionRelocation.cc
    source/UserMode/PlatformUtil/Linux/ProcessRuntimeUtility.cc
    source/UserMode/UnifiedInterface/platform-posix.cc
    source/UserMode/ExecMemory/code-patch-tool-posix.cc source/UserMode/ExecMemory/clear-cache-tool-all.c
    ${logging.SOURCE_FILE_LIST} ${xnucxx.SOURCE_FILE_LIST})
  target_compile_definitions(dobby_relocation_arm_test PRIVATE DOBBY_TEST_TARGET_ARM)
  target_link_libraries(dobby_relocation_arm_test PRIVATE PkgConfig::CAPSTONE PkgConfig::UNICORN dl)
  if(DOBBY_EMULATOR_ANDROID)
    target_link_libraries(dobby_relocation_arm_test PRIVATE log)
  endif()
  add_test(NAME dobby_relocation_arm COMMAND dobby_relocation_arm_test)
  set_tests_properties(dobby_relocation_arm PROPERTIES TIMEOUT 60)
endif()
