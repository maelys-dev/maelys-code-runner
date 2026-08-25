cmake_minimum_required(VERSION 3.24)

project(maelys-code-runner VERSION 0.1.0 LANGUAGES C)

include(FetchContent)
include(CTest)
include(GNUInstallDirs)

# Manual population lets third-party trees remain EXCLUDE_FROM_ALL while still
# supporting CMake 3.24. Remove this compatibility policy when the minimum
# version provides FetchContent_Declare(EXCLUDE_FROM_ALL).
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MAELYS_CODE_RUNNER_WARNINGS_AS_ERRORS "Treat project warnings as errors" ON)
option(MAELYS_CODE_RUNNER_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MAELYS_CODE_RUNNER_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(MAELYS_CODE_RUNNER_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(MAELYS_CODE_RUNNER_BUILD_FUZZER "Build the libFuzzer bridge target" OFF)

if(MAELYS_CODE_RUNNER_ENABLE_TSAN AND
   (MAELYS_CODE_RUNNER_ENABLE_ASAN OR MAELYS_CODE_RUNNER_ENABLE_UBSAN))
  message(FATAL_ERROR "TSan must use a separate build directory")
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_CLI_STATIC OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_WERROR OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  quickjs_ng
  GIT_REPOSITORY https://github.com/quickjs-ng/quickjs.git
  GIT_TAG fd0a0210b7be00957751871e7e01b8291268fc29
  GIT_SHALLOW TRUE
)

set(JANSSON_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(JANSSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(JANSSON_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JANSSON_WITHOUT_TESTS ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  jansson
  GIT_REPOSITORY https://github.com/akheron/jansson.git
  GIT_TAG dbb5fb3636e155fccfce4cd215de752779bd6971
  GIT_SHALLOW TRUE
)

FetchContent_GetProperties(quickjs_ng)
if(NOT quickjs_ng_POPULATED)
  FetchContent_Populate(quickjs_ng)
  add_subdirectory(
    ${quickjs_ng_SOURCE_DIR}
    ${quickjs_ng_BINARY_DIR}
    EXCLUDE_FROM_ALL
  )
endif()

FetchContent_GetProperties(jansson)
if(NOT jansson_POPULATED)
  FetchContent_Populate(jansson)
  add_subdirectory(
    ${jansson_SOURCE_DIR}
    ${jansson_BINARY_DIR}
    EXCLUDE_FROM_ALL
  )
endif()

find_package(Threads REQUIRED)

add_executable(maelys-code-runner
  src/bridge.c
  src/main.c
  src/runner.c
)

target_include_directories(maelys-code-runner PRIVATE src)
target_link_libraries(maelys-code-runner PRIVATE qjs jansson Threads::Threads)
target_compile_definitions(maelys-code-runner PRIVATE
  _POSIX_C_SOURCE=200809L
  MAELYS_CODE_RUNNER_VERSION="${PROJECT_VERSION}"
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(maelys-code-runner PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
  )
  if(MAELYS_CODE_RUNNER_WARNINGS_AS_ERRORS)
    target_compile_options(maelys-code-runner PRIVATE -Werror)
  endif()
endif()

set(MAELYS_CODE_RUNNER_SANITIZER_FLAGS)
if(MAELYS_CODE_RUNNER_ENABLE_ASAN)
  list(APPEND MAELYS_CODE_RUNNER_SANITIZER_FLAGS -fsanitize=address)
endif()
if(MAELYS_CODE_RUNNER_ENABLE_UBSAN)
  list(APPEND MAELYS_CODE_RUNNER_SANITIZER_FLAGS -fsanitize=undefined)
endif()
if(MAELYS_CODE_RUNNER_ENABLE_TSAN)
  list(APPEND MAELYS_CODE_RUNNER_SANITIZER_FLAGS -fsanitize=thread)
endif()
if(MAELYS_CODE_RUNNER_SANITIZER_FLAGS)
  foreach(target maelys-code-runner qjs jansson)
    target_compile_options(${target} PRIVATE
      ${MAELYS_CODE_RUNNER_SANITIZER_FLAGS} -fno-omit-frame-pointer)
  endforeach()
  target_link_options(maelys-code-runner PRIVATE ${MAELYS_CODE_RUNNER_SANITIZER_FLAGS})
endif()

install(TARGETS maelys-code-runner RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES
  README.md
  SECURITY.md
  CHANGELOG.md
  VERSION
  docs/code-bridge-v1.md
  docs/executor-integration.md
  docs/quickjs-runner.md
  docs/releasing.md
  docs/release-0.1.0.md
  DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES ${quickjs_ng_SOURCE_DIR}/LICENSE
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses
  RENAME LICENSE.quickjs-ng)
install(FILES ${jansson_SOURCE_DIR}/LICENSE
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses
  RENAME LICENSE.jansson)

if(BUILD_TESTING)
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
  add_test(
    NAME runner-integration
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_runner.py
            $<TARGET_FILE:maelys-code-runner>
  )
  set_tests_properties(runner-integration PROPERTIES TIMEOUT 30)
  add_test(
    NAME code-bridge-v1-conformance
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bridge_conformance.py
            $<TARGET_FILE:maelys-code-runner>
  )
  set_tests_properties(code-bridge-v1-conformance PROPERTIES TIMEOUT 30)
endif()

if(MAELYS_CODE_RUNNER_BUILD_FUZZER)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "The fuzz target currently requires Clang/libFuzzer")
  endif()
  add_executable(fuzz-code-bridge EXCLUDE_FROM_ALL
    fuzz/fuzz_bridge.c
    src/bridge.c
  )
  target_include_directories(fuzz-code-bridge PRIVATE src)
  target_link_libraries(fuzz-code-bridge PRIVATE jansson Threads::Threads)
  target_compile_definitions(fuzz-code-bridge PRIVATE _POSIX_C_SOURCE=200809L)
  target_compile_options(fuzz-code-bridge PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
  )
  target_link_options(fuzz-code-bridge PRIVATE -fsanitize=fuzzer,address,undefined)
  if(APPLE AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    # LLVM 22 object files currently require Apple's classic linker on macOS 15.
    target_link_options(fuzz-code-bridge PRIVATE -Wl,-ld_classic)
  endif()
endif()
