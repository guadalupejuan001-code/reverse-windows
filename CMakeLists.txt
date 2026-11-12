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
add_library(reverse_windows_component_headers INTERFACE)
target_include_directories(reverse_windows_component_headers INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${REVERSE_MUNDO_X_ROOT}/pc-android/native"
  "${REVERSE_MUNDO_X_ROOT}/native/include")
target_link_libraries(reverse_windows_component_headers INTERFACE Threads::Threads)

add_executable(reverse_windows_boot_probe src/reverse_windows_boot_probe.cpp)
target_link_libraries(reverse_windows_boot_probe PRIVATE reverse_windows_component_headers)
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

  add_executable(reverse_windows_bios_disk_test tests/bios_disk_guest_test.cpp)
  target_link_libraries(reverse_windows_bios_disk_test PRIVATE reverse_windows_component_headers)
  if(MSVC)
    target_compile_options(reverse_windows_bios_disk_test PRIVATE /W4)
  else()
    target_compile_options(reverse_windows_bios_disk_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME reverse_windows_x86_hdd_write_read_persist COMMAND reverse_windows_bios_disk_test)
  add_executable(reverse_windows_riso_native_test tests/riso_native_test.cpp)
  target_link_libraries(reverse_windows_riso_native_test PRIVATE reverse_windows_component_headers)
  if(MSVC)
    target_compile_options(reverse_windows_riso_native_test PRIVATE /W4)
  else()
    target_compile_options(reverse_windows_riso_native_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME reverse_windows_riso_real_iso_boot
    COMMAND reverse_windows_riso_native_test "${CMAKE_CURRENT_BINARY_DIR}/interop.iso"
                                           "${CMAKE_CURRENT_BINARY_DIR}/interop.riso")
  set_tests_properties(reverse_windows_real_x86_demo reverse_windows_real_x64_demo
                       reverse_windows_x86_hdd_write_read_persist reverse_windows_riso_real_iso_boot PROPERTIES TIMEOUT 120)
endif()
