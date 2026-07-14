# Host-side unit tests for DA15 firmware modules.
#
# This is a standalone project compiled with the HOST toolchain (not the ARM
# cross toolchain), so configure it separately from the firmware build:
#
#   cmake -S tests -B build/host-tests
#   cmake --build build/host-tests
#   ctest --test-dir build/host-tests --output-on-failure

cmake_minimum_required(VERSION 3.22)
project(da15_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -O1 -g)

enable_testing()

set(FW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# audio_eq.c is pure C — compiles on the host unmodified
add_executable(test_audio_eq
    test_audio_eq.c
    "${FW_ROOT}/App/Src/audio_eq.c"
)
target_include_directories(test_audio_eq PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${FW_ROOT}/App/Inc"
)
add_test(NAME audio_eq COMMAND test_audio_eq)

# eq_profile.c needs the HAL/RTT stubs in tests/stubs (flash calls are inert)
add_executable(test_eq_profile
    test_eq_profile.c
    "${FW_ROOT}/App/Src/eq_profile.c"
)
target_include_directories(test_eq_profile PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${FW_ROOT}/App/Inc"
)
target_link_libraries(test_eq_profile m)
add_test(NAME eq_profile COMMAND test_eq_profile)
