cmake_minimum_required(VERSION 3.14)
project(
  decky_ear_tuner_backend
  VERSION 0.1
  LANGUAGES CXX
)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(EAR_TUNER_ENABLE_RTSAN "Check audio callbacks with RealtimeSanitizer"
       OFF
)
add_library(ear_tuner_realtime INTERFACE)
if(EAR_TUNER_ENABLE_RTSAN)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "RealtimeSanitizer requires a Clang toolchain")
  endif()
  include(CheckCXXSourceCompiles)
  include(CMakePushCheckState)
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=realtime -Werror=unknown-attributes")
  check_cxx_source_compiles(
    "
    #if !__has_feature(realtime_sanitizer)
    #error RealtimeSanitizer is not enabled
    #endif
    void process() [[clang::nonblocking]] {}
    int main() { process(); }
    "
    EAR_TUNER_HAS_RTSAN
  )
  cmake_pop_check_state()
  if(NOT EAR_TUNER_HAS_RTSAN)
    message(
      FATAL_ERROR
        "Clang must support -fsanitize=realtime, clang::nonblocking, and the RTSan runtime"
    )
  endif()
  target_compile_definitions(
    ear_tuner_realtime INTERFACE EAR_TUNER_ENABLE_RTSAN=1
  )
  target_compile_options(
    ear_tuner_realtime INTERFACE -fsanitize=realtime -fno-omit-frame-pointer
  )
  target_link_options(ear_tuner_realtime INTERFACE -fsanitize=realtime)
endif()

include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.16.0
)
FetchContent_MakeAvailable(Catch2)

include(CTest)
include(Catch)
enable_testing()
set(EAR_TUNER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src")
add_subdirectory(${EAR_TUNER_SOURCE_DIR}/dsp ${CMAKE_CURRENT_BINARY_DIR}/dsp)

add_library(
  ear_tuner_core STATIC
  ${EAR_TUNER_SOURCE_DIR}/native/core/ear_tuner/control_state.cpp
)
target_compile_features(ear_tuner_core PUBLIC cxx_std_17)
target_include_directories(
  ear_tuner_core PUBLIC ${EAR_TUNER_SOURCE_DIR} ${EAR_TUNER_SOURCE_DIR}/native
                        ${EAR_TUNER_SOURCE_DIR}/native/core
)
target_link_libraries(ear_tuner_core PUBLIC ear_tuner_dsp ear_tuner_realtime)

add_executable(test_core ${EAR_TUNER_SOURCE_DIR}/native/tests/test_core.cpp)
target_compile_features(test_core PRIVATE cxx_std_17)
target_link_libraries(test_core PRIVATE ear_tuner_core Catch2::Catch2WithMain)
catch_discover_tests(test_core)

option(EAR_TUNER_BUILD_BENCHMARKS "Build DSP performance benchmarks" OFF)
if(EAR_TUNER_BUILD_BENCHMARKS)
  add_executable(
    bench_dsp ${EAR_TUNER_SOURCE_DIR}/native/benchmarks/bench_dsp.cpp
  )
  target_compile_features(bench_dsp PRIVATE cxx_std_17)
  target_link_libraries(bench_dsp PRIVATE ear_tuner_core Catch2::Catch2WithMain)
endif()

if(EAR_TUNER_ENABLE_RTSAN)
  add_executable(
    test_realtime ${EAR_TUNER_SOURCE_DIR}/native/tests/test_realtime.cpp
  )
  target_link_libraries(test_realtime PRIVATE ear_tuner_core)
  add_test(NAME realtime_processing COMMAND test_realtime)
  add_test(
    NAME realtime_allocation_guard
    COMMAND ${CMAKE_COMMAND} "-DTEST_EXECUTABLE=$<TARGET_FILE:test_realtime>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_rtsan_guard.cmake"
  )
  set_tests_properties(
    realtime_processing realtime_allocation_guard
    PROPERTIES
      ENVIRONMENT
      "RTSAN_OPTIONS=halt_on_error=true:abort_on_error=false:exitcode=86"
      TIMEOUT 60
  )
endif()

if(UNIX)
  find_package(Threads REQUIRED)
  add_library(
    ear_tuner_posix_http STATIC
    ${EAR_TUNER_SOURCE_DIR}/native/platform/posix/http/http_server.cpp
  )
  target_compile_features(ear_tuner_posix_http PUBLIC cxx_std_17)
  target_include_directories(
    ear_tuner_posix_http
    PUBLIC ${EAR_TUNER_SOURCE_DIR} ${EAR_TUNER_SOURCE_DIR}/native
           ${EAR_TUNER_SOURCE_DIR}/native/core
  )
  target_link_libraries(ear_tuner_posix_http PUBLIC ear_tuner_core)

  add_executable(
    test_http_server ${EAR_TUNER_SOURCE_DIR}/native/tests/test_http_server.cpp
  )
  target_compile_features(test_http_server PRIVATE cxx_std_17)
  target_link_libraries(
    test_http_server PRIVATE ear_tuner_posix_http Catch2::Catch2WithMain
                             Threads::Threads
  )
  catch_discover_tests(test_http_server)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3)
  endif()

  if(PIPEWIRE_FOUND AND TARGET ear_tuner_posix_http)
    add_library(
      ear_tuner_linux_pipewire STATIC
      ${EAR_TUNER_SOURCE_DIR}/native/platform/linux/pipewire/pipewire_filter.cpp
    )
    target_compile_features(ear_tuner_linux_pipewire PUBLIC cxx_std_17)
    target_include_directories(
      ear_tuner_linux_pipewire
      PUBLIC ${EAR_TUNER_SOURCE_DIR} ${EAR_TUNER_SOURCE_DIR}/native
             ${EAR_TUNER_SOURCE_DIR}/native/core
    )
    target_link_libraries(
      ear_tuner_linux_pipewire PUBLIC ear_tuner_core PkgConfig::PIPEWIRE
    )

    add_executable(
      test_pipewire_filter
      ${EAR_TUNER_SOURCE_DIR}/native/tests/test_pipewire_filter.cpp
    )
    target_compile_features(test_pipewire_filter PRIVATE cxx_std_17)
    target_include_directories(
      test_pipewire_filter
      PRIVATE ${EAR_TUNER_SOURCE_DIR} ${EAR_TUNER_SOURCE_DIR}/native
              ${EAR_TUNER_SOURCE_DIR}/native/core
    )
    target_link_libraries(
      test_pipewire_filter PRIVATE ear_tuner_linux_pipewire
                                   Catch2::Catch2WithMain
    )
    catch_discover_tests(test_pipewire_filter)

    add_executable(
      decky_ear_tuner_backend
      ${EAR_TUNER_SOURCE_DIR}/native/platform/linux/main.cpp
    )
    target_compile_features(decky_ear_tuner_backend PRIVATE cxx_std_17)
    target_include_directories(
      decky_ear_tuner_backend
      PRIVATE ${EAR_TUNER_SOURCE_DIR} ${EAR_TUNER_SOURCE_DIR}/native
              ${EAR_TUNER_SOURCE_DIR}/native/core
    )
    target_link_libraries(
      decky_ear_tuner_backend PRIVATE ear_tuner_linux_pipewire
                                      ear_tuner_posix_http Threads::Threads
    )
    add_test(
      NAME decky_ear_tuner_backend_requires_pipewire
      COMMAND ${CMAKE_COMMAND} -E env PIPEWIRE_REMOTE=ear-tuner-test-no-server
              $<TARGET_FILE:decky_ear_tuner_backend>
    )
    set_tests_properties(
      decky_ear_tuner_backend_requires_pipewire PROPERTIES WILL_FAIL TRUE
                                                           TIMEOUT 10
    )
  else()
    message(WARNING "PipeWire was not found; skipping Linux runtime executable")
  endif()
endif()
