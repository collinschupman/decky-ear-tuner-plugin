add_library(
  ear_tuner_windows_audio STATIC
  ${EAR_TUNER_SOURCE_DIR}/native/platform/windows/audio_bridge.cpp
)
target_link_libraries(ear_tuner_windows_audio PUBLIC ear_tuner_core)
add_executable(
  test_windows_audio
  ${EAR_TUNER_SOURCE_DIR}/native/tests/test_windows_audio.cpp
)
target_link_libraries(
  test_windows_audio PRIVATE ear_tuner_windows_audio Catch2::Catch2WithMain
)
catch_discover_tests(test_windows_audio)

if(WIN32)
  target_compile_definitions(
    ear_tuner_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
  )
  add_library(
    ear_tuner_windows_http STATIC
    ${EAR_TUNER_SOURCE_DIR}/native/platform/posix/http/http_server.cpp
  )
  target_link_libraries(ear_tuner_windows_http PUBLIC ear_tuner_core ws2_32)
  add_executable(
    ear_tuner_windows ${EAR_TUNER_SOURCE_DIR}/native/platform/windows/main.cpp
  )
  target_link_libraries(
    ear_tuner_windows PRIVATE ear_tuner_windows_audio ear_tuner_windows_http
                              ole32 uuid avrt
  )
  set_target_properties(
    ear_tuner_windows PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                 "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
  )
  if(MINGW)
    target_link_options(ear_tuner_windows PRIVATE -municode)
  endif()
  add_executable(
    test_windows_http
    ${EAR_TUNER_SOURCE_DIR}/native/tests/test_windows_http.cpp
  )
  target_link_libraries(
    test_windows_http PRIVATE ear_tuner_windows_http Catch2::Catch2WithMain
  )
  catch_discover_tests(test_windows_http)
endif()
