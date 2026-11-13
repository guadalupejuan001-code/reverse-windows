cmake_minimum_required(VERSION 3.20)
project(reverse_windows LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(REVERSE_MUNDO_X_ROOT "" CACHE PATH "Exact pinned mundo-X checkout containing PcMachine")
set(REVERSE_QSSD_ROOT "" CACHE PATH "Exact pinned Virtual-qSSD- checkout containing StateBus")
set(REVERSE_GPU_VIRTUAL_ROOT "" CACHE PATH "Exact pinned GPU-VIRTUAL checkout containing PhysicalWorkAuthority")
if(NOT EXISTS "${REVERSE_QSSD_ROOT}/include/qssd/state_bus.hpp")
  message(FATAL_ERROR "Pinned qSSD StateBus required for REVERSE Quantum Disk")
endif()
if(NOT EXISTS "${REVERSE_GPU_VIRTUAL_ROOT}/include/trabajo_fisico.hpp")
  message(FATAL_ERROR "Pinned GPU-VIRTUAL work authority required for real guest VGA delta")
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
  "${REVERSE_QSSD_ROOT}/include"
  "${REVERSE_GPU_VIRTUAL_ROOT}/include")
target_link_libraries(reverse_windows_component_headers INTERFACE Threads::Threads)

add_executable(reverse_windows_boot_probe src/reverse_windows_boot_probe.cpp)
target_link_libraries(reverse_windows_boot_probe PRIVATE reverse_windows_component_headers)

# ISO-free hardware readiness audit: one canonical PcMachine, Ether/XI,
# one QSSD BlockDisk, PCI ATA, GPU-VIRTUAL planner, qSSD GraphicsWorkState.
add_executable(reverse_windows_c09r_hardware_preflight src/c09r_hardware_preflight.cpp)
target_link_libraries(reverse_windows_c09r_hardware_preflight PRIVATE reverse_windows_component_headers)
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

  add_executable(reverse_windows_c07_ata_guest_test tests/c07_ata_guest_test.cpp)
  target_link_libraries(reverse_windows_c07_ata_guest_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c07_pci_ata_qssd_pio_irq COMMAND reverse_windows_c07_ata_guest_test)

  # Use the exact pinned Mundo X guest port instructions, not a forked CPU.
  add_executable(reverse_windows_c07_cpu_port_test
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_port_pio_c07_test.cpp")
  target_link_libraries(reverse_windows_c07_cpu_port_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c07_cpu_real_rep_pio COMMAND reverse_windows_c07_cpu_port_test)

  add_executable(reverse_windows_c08_maintenance_test tests/c08_qssd_maintenance_test.cpp)
  target_link_libraries(reverse_windows_c08_maintenance_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c08_verified_checkpoint_compaction
    COMMAND reverse_windows_c08_maintenance_test)

  add_executable(reverse_windows_c08_benchmark tests/c08_raw_vs_qssd_benchmark.cpp)
  target_link_libraries(reverse_windows_c08_benchmark PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c08_raw_vs_qssd_benchmark
    COMMAND reverse_windows_c08_benchmark)

  # Source is taken from the exact pinned Mundo X authority, not a duplicate
  # CPU decoder. C09 traces are opt-in and cannot change x86 semantics.
  add_executable(reverse_windows_c09_cpu_trace
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_trace_c09_test.cpp")
  target_link_libraries(reverse_windows_c09_cpu_trace PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c09_cpu_precise_step_trace
    COMMAND reverse_windows_c09_cpu_trace)

  add_executable(reverse_windows_c09_hardware_trace_test tests/c09_hardware_trace_test.cpp)
  target_link_libraries(reverse_windows_c09_hardware_trace_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c09_real_hardware_trace
    COMMAND reverse_windows_c09_hardware_trace_test)

  add_executable(reverse_windows_c09r_cpu_features
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_windows10_c09r_cpu_readiness_test.cpp")
  target_link_libraries(reverse_windows_c09r_cpu_features PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c09r_real_cpu_cx16_prefetchw
    COMMAND reverse_windows_c09r_cpu_features)

  add_test(NAME reverse_windows_c09r_iso_free_hardware_preflight
    COMMAND reverse_windows_c09r_hardware_preflight --self-test)

  # C09R1 keeps the exact FPU/XMM test inside Mundo X, the sole x86 CPU
  # authority; we compile the original source, not an emulation copy.
  add_executable(reverse_windows_c09r1_cpu_extended_state
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_c09r1_extended_state_test.cpp")
  target_link_libraries(reverse_windows_c09r1_cpu_extended_state PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c09r1_xmm_x87_fxsave_fxrstor
    COMMAND reverse_windows_c09r1_cpu_extended_state)

  set_tests_properties(reverse_windows_real_x86_demo reverse_windows_real_x64_demo
                       reverse_windows_x86_hdd_write_read_persist
                       reverse_windows_qssd_sparse_atomic_reboot_x86
                       reverse_windows_c04_real_guest_idt
                       reverse_windows_c05_real_cpl3_tss
                       reverse_windows_c06_real_motherboard
                       reverse_windows_c07_pci_ata_qssd_pio_irq
                       reverse_windows_c07_cpu_real_rep_pio
                       reverse_windows_c08_verified_checkpoint_compaction
                       reverse_windows_c08_raw_vs_qssd_benchmark
                       reverse_windows_c09_cpu_precise_step_trace
                       reverse_windows_c09_real_hardware_trace
                       reverse_windows_c09r_iso_free_hardware_preflight
                       reverse_windows_c09r_real_cpu_cx16_prefetchw
                       reverse_windows_c09r1_xmm_x87_fxsave_fxrstor PROPERTIES TIMEOUT 120)
endif()
