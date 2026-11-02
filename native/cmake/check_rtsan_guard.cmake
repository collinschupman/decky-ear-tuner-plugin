execute_process(
  COMMAND "${TEST_EXECUTABLE}" --allocate
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
  TIMEOUT 10
)
if(NOT "${result}" STREQUAL "86"
   OR NOT error MATCHES "RealtimeSanitizer"
   OR NOT error MATCHES "malloc"
)
  message(
    FATAL_ERROR
      "Expected RTSan to reject malloc with exit code 86; got ${result}\n${output}\n${error}"
  )
endif()
message(STATUS "RTSan rejected the deliberate allocation")
