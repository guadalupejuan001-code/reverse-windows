cmake_minimum_required(VERSION 3.20)
project(reverse_windows LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(REVERSE_MUNDO_X_ROOT "" CACHE PATH "Exact pinned mundo-X checkout containing PcMachine")
set(REVERSE_QSSD_ROOT "" CACHE PATH "Exact pinned Virtual-qSSD- checkout containing StateBus")
if(NOT EXISTS "${REVERSE_QSSD_ROOT}/include/qssd/state_bus.hpp")
  message(FATAL_ERROR "Pinned qSSD StateBus required for REVERSE Quantum Disk")
endif()
if(NOT EXISTS "${REVERSE_MUNDO_X_ROOT}/pc-android/native/pc_machine.hpp")
  message(FATAL_ERROR "Missing real Mundo X PcMachine; no fake or fallback CPU is permitted")
endif()

find_package(Threads REQUIRED)
add_library(reverse_windows_component_headers INTERFACE)
target_include_directories(reverse_windows_component_headers INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${REVERSE_MUNDO_X_ROOT}/pc-android/native"
  "${REVERSE_MUNDO_X_ROOT}/native/include"
  "${REVERSE_QSSD_ROOT}/include")
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

  add_executable(reverse_windows_qssd_disk_test tests/qssd_disk_guest_test.cpp)
  target_link_libraries(reverse_windows_qssd_disk_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_qssd_sparse_atomic_reboot_x86 COMMAND reverse_windows_qssd_disk_test)
  # Compile the authentic x86 guest IDT test from the pinned Mundo X CPU
  # source, not a duplicated/shadow implementation in REVERSE Windows.
  add_executable(reverse_windows_c04_guest_idt
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_idt_c04_test.cpp")
  target_link_libraries(reverse_windows_c04_guest_idt PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c04_real_guest_idt COMMAND reverse_windows_c04_guest_idt)
  add_executable(reverse_windows_c05_guest_cpl3
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_cpl_c05_test.cpp")
  target_link_libraries(reverse_windows_c05_guest_cpl3 PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c05_real_cpl3_tss COMMAND reverse_windows_c05_guest_cpl3)

  add_executable(reverse_windows_c06_motherboard_guest_test tests/c06_motherboard_guest_test.cpp)
  target_link_libraries(reverse_windows_c06_motherboard_guest_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c06_real_motherboard COMMAND reverse_windows_c06_motherboard_guest_test)

  set_tests_properties(reverse_windows_real_x86_demo reverse_windows_real_x64_demo
                       reverse_windows_x86_hdd_write_read_persist
                       reverse_windows_qssd_sparse_atomic_reboot_x86
                       reverse_windows_c04_real_guest_idt
                       reverse_windows_c05_real_cpl3_tss
                       reverse_windows_c06_real_motherboard PROPERTIES TIMEOUT 120)
endif()
