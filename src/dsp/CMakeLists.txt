add_library(ear_tuner_dsp INTERFACE)
target_include_directories(ear_tuner_dsp INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(ear_tuner_dsp INTERFACE cxx_std_17)

add_executable(test_dsp tests/test_dsp.cpp)
target_link_libraries(test_dsp PRIVATE ear_tuner_dsp Catch2::Catch2WithMain)
catch_discover_tests(test_dsp)
