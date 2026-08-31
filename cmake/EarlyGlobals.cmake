# Early global definitions.
# Put anything here needed for the toolchain
# and include this file before calling project()
#
# NOTE: CMake >= 4.0 only populates CMAKE_SYSTEM_NAME / CMAKE_SYSTEM_PROCESSOR
# during project(), so on modern CMake this file must be included AGAIN after
# project() to pick up the values set by the toolchain file (see CMakeLists.txt).

# The Processor
if(CMAKE_SYSTEM_PROCESSOR MATCHES "amd64.*|x86_64.*|AMD64.*|x64.*")
  set(PROCESSOR.X86_64 1)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i686.*|i386.*|x86.*|amd64.*|AMD64.*")
  set(PROCESSOR.X86 1)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm64.*|^aarch64.*|^AARCH64.*")
  set(PROCESSOR.AARCH64 1)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm.*|ARM.*)")
  set(PROCESSOR.ARM 1)
elseif(CMAKE_SYSTEM_PROCESSOR)
  # empty before project() on CMake >= 4.0; evaluated again after project()
  message(FATAL_ERROR "Processor ${CMAKE_SYSTEM_PROCESSOR} not configured")
endif()

# The System (Android/Linux only)
if(CMAKE_SYSTEM_NAME MATCHES "^Android")
  set(SYSTEM.Android 1)
elseif(CMAKE_SYSTEM_NAME MATCHES "^Linux")
  set(SYSTEM.Linux 1)
elseif(CMAKE_SYSTEM_NAME)
  # empty before project() on CMake >= 4.0; evaluated again after project()
  message(FATAL_ERROR "System ${CMAKE_SYSTEM_NAME} not configured")
endif()
