cmake_minimum_required(VERSION 3.20)
project(reverse_windows LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(REVERSE_MUNDO_X_ROOT "" CACHE PATH "Exact pinned mundo-X checkout containing PcMachine")
if(NOT EXISTS "${REVERSE_MUNDO_X_ROOT}/pc-android/native/pc_machine.hpp")
  message(FATAL_ERROR "Missing real Mundo X PcMachine; no fake or fallback CPU is permitted")
endif()

find_package(Threads REQUIRED)
add_executable(reverse_windows_boot_probe src/reverse_windows_boot_probe.cpp)
target_include_directories(reverse_windows_boot_probe PRIVATE
  "${REVERSE_MUNDO_X_ROOT}/pc-android/native"
  "${REVERSE_MUNDO_X_ROOT}/native/include")
target_link_libraries(reverse_windows_boot_probe PRIVATE Threads::Threads)
if(MSVC)
  target_compile_options(reverse_windows_boot_probe PRIVATE /W4)
else()
  target_compile_options(reverse_windows_boot_probe PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
  add_test(NAME reverse_windows_real_x86_demo
    COMMAND reverse_windows_boot_probe --demo-x86 --steps 200000 --ram-mib 2048 --resident-mib 256)
  add_test(NAME reverse_windows_real_x64_demo
    COMMAND reverse_windows_boot_probe --demo-x64 --steps 200000 --ram-mib 2048 --resident-mib 256)
  set_tests_properties(reverse_windows_real_x86_demo reverse_windows_real_x64_demo PROPERTIES TIMEOUT 120)
endif()
