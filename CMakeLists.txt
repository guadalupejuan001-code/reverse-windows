cmake_minimum_required(VERSION 3.20)
project(reverse_windows LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(REVERSE_MUNDO_X_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/mundo-X" CACHE PATH "Exact pinned mundo-X checkout containing PcMachine")
set(REVERSE_QSSD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/Virtual-qSSD-" CACHE PATH "Exact pinned Virtual-qSSD- checkout containing StateBus")
set(REVERSE_GPU_VIRTUAL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/GPU-VIRTUAL" CACHE PATH "Exact pinned GPU-VIRTUAL checkout containing PhysicalWorkAuthority")
set(REVERSE_REGRAPHICS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/REVERSE-PC" CACHE PATH "Exact pinned REgraphics checkout")
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
find_package(ZLIB REQUIRED)
include(cmake/VerifyMundoX.cmake)
reverse_windows_pin_mundo_x("${CMAKE_CURRENT_SOURCE_DIR}/config/dependencies.lock.json" "${REVERSE_MUNDO_X_ROOT}")
include(cmake/QuantumRam.cmake)
reverse_windows_add_quantum_ram("${CMAKE_CURRENT_SOURCE_DIR}")
add_library(reverse_windows_component_headers INTERFACE)
add_dependencies(reverse_windows_component_headers reverse_windows_verify_mundo_x)
target_compile_definitions(reverse_windows_component_headers INTERFACE
  MUNDO_X_PC_STATE_RAM=1
  REVERSE_MUNDO_X_SOURCE_SHA="${REVERSE_MUNDO_X_SOURCE_SHA}")
target_include_directories(reverse_windows_component_headers INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${REVERSE_MUNDO_X_ROOT}/pc-android/native"
  "${REVERSE_MUNDO_X_ROOT}/native/include"
  "${REVERSE_QSSD_ROOT}/include"
  "${REVERSE_GPU_VIRTUAL_ROOT}/include")
target_link_libraries(reverse_windows_component_headers INTERFACE
  Threads::Threads ZLIB::ZLIB reverse_windows_quantum_ram)
option(REVERSE_WINDOWS_GLES_GUEST "Compile the REgraphics EGL/GLES2 guest renderer" ON)
if(REVERSE_WINDOWS_GLES_GUEST)
  find_path(REVERSE_EGL_INCLUDE EGL/egl.h)
  find_path(REVERSE_GLES_INCLUDE GLES2/gl2.h)
  find_library(REVERSE_EGL_LIBRARY EGL)
  find_library(REVERSE_GLES_LIBRARY GLESv2)
  if(REVERSE_EGL_INCLUDE AND REVERSE_GLES_INCLUDE AND REVERSE_EGL_LIBRARY AND REVERSE_GLES_LIBRARY)
    if(NOT EXISTS "${REVERSE_REGRAPHICS_ROOT}/src/regraphics/guest_renderer.hpp")
      message(FATAL_ERROR "Pin REVERSE-PC's REgraphics guest draw backend or disable the GLES guest path explicitly")
    endif()
    target_include_directories(reverse_windows_component_headers INTERFACE
      "${REVERSE_REGRAPHICS_ROOT}/src" "${REVERSE_EGL_INCLUDE}" "${REVERSE_GLES_INCLUDE}")
    target_compile_definitions(reverse_windows_component_headers INTERFACE REVERSE_WINDOWS_GLES_GUEST=1)
    target_link_libraries(reverse_windows_component_headers INTERFACE "${REVERSE_EGL_LIBRARY}" "${REVERSE_GLES_LIBRARY}")
  else()
    message(STATUS "EGL/GLES2 development libraries missing; native VBE remains available")
  endif()
endif()
if(MSVC)
  target_compile_options(reverse_windows_component_headers INTERFACE /fp:strict)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(reverse_windows_component_headers INTERFACE -frounding-math)
endif()

add_executable(reverse_windows_boot_probe src/reverse_windows_boot_probe.cpp
  "${REVERSE_MUNDO_X_ROOT}/native/src/ether_transform.cpp"
  "${REVERSE_MUNDO_X_ROOT}/native/src/ether_neon.cpp")
target_compile_definitions(reverse_windows_boot_probe PRIVATE MUNDO_X_PC_ETHER_NEXT=1)
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
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  foreach(ram_test IN ITEMS quantum_ram_authority ether_state_ram_integration)
    add_executable(reverse_windows_${ram_test}_test tests/${ram_test}_test.cpp)
    target_link_libraries(reverse_windows_${ram_test}_test PRIVATE reverse_windows_component_headers)
    add_test(NAME reverse_windows_${ram_test} COMMAND reverse_windows_${ram_test}_test)
    set_tests_properties(reverse_windows_${ram_test} PROPERTIES TIMEOUT 120)
  endforeach()
  # Current-version comparison binaries use the same memory selection and
  # isolated vector provider as the APK. Historical baselines stay separate.
  foreach(bench IN ITEMS prime_execution boot_segment vram_partial)
    add_executable(reverse_windows_${bench}_bench tests/${bench}_bench.cpp)
    target_link_libraries(reverse_windows_${bench}_bench PRIVATE reverse_windows_component_headers)
  endforeach()
  add_executable(reverse_windows_pc_group6_iso_test
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/pc_group6_iso_test.cpp")
  target_link_libraries(reverse_windows_pc_group6_iso_test PRIVATE reverse_windows_component_headers)
  find_package(Java 11 QUIET COMPONENTS Development)
  if(Java_FOUND)
    set(riso_classes "${CMAKE_CURRENT_BINARY_DIR}/riso-host")
    set(riso_iso "${riso_classes}/interop.iso")
    set(riso_archive "${riso_classes}/interop.riso")
    set(riso_codec "${CMAKE_CURRENT_SOURCE_DIR}/iso-lab-android/app/src/main/java/com/reversewindows/isolab/IsoArchive.java")
    set(riso_host_test "${CMAKE_CURRENT_SOURCE_DIR}/iso-lab-android/host-tests/com/reversewindows/isolab/IsoArchiveTest.java")
    set(riso_fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/RisoInteropFixture.java")
    add_custom_command(OUTPUT "${riso_iso}" "${riso_archive}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${riso_classes}"
      COMMAND "${Java_JAVAC_EXECUTABLE}" -encoding UTF-8 -d "${riso_classes}"
        "${riso_codec}" "${riso_host_test}" "${riso_fixture}"
      COMMAND "${Java_JAVA_EXECUTABLE}" -cp "${riso_classes}"
        com.reversewindows.isolab.RisoInteropFixture "${riso_iso}" "${riso_archive}"
      DEPENDS "${riso_codec}" "${riso_host_test}" "${riso_fixture}"
      VERBATIM)
    add_custom_target(reverse_windows_riso_fixture DEPENDS "${riso_iso}" "${riso_archive}")
    add_executable(reverse_windows_riso_native_test tests/riso_native_test.cpp)
    add_dependencies(reverse_windows_riso_native_test reverse_windows_riso_fixture)
    target_link_libraries(reverse_windows_riso_native_test PRIVATE reverse_windows_component_headers)
    add_test(NAME reverse_windows_riso_codec COMMAND "${Java_JAVA_EXECUTABLE}" -Xmx256m
      -cp "${riso_classes}" com.reversewindows.isolab.IsoArchiveTest)
    add_test(NAME reverse_windows_riso_real_iso_boot
      COMMAND reverse_windows_riso_native_test "${riso_iso}" "${riso_archive}")
    add_test(NAME reverse_windows_riso_probe
      COMMAND reverse_windows_boot_probe --riso "${riso_archive}" --verify-riso --steps 40)
    set_tests_properties(reverse_windows_riso_codec reverse_windows_riso_real_iso_boot
      reverse_windows_riso_probe PROPERTIES TIMEOUT 120)
    set_tests_properties(reverse_windows_riso_probe PROPERTIES
      PASS_REGULAR_EXPRESSION "\"compressed_media_mounted\":true.*\"machine\":\\{\"status\":\"halted\",\"instructions\":3,.*\"cpuid_vendor\":\"GenuineIntel\"")
  endif()
  add_executable(reverse_windows_android_boot_session_test tests/android_boot_session_test.cpp)
  target_link_libraries(reverse_windows_android_boot_session_test PRIVATE reverse_windows_component_headers)
  if(TARGET reverse_windows_riso_fixture)
    add_dependencies(reverse_windows_android_boot_session_test reverse_windows_riso_fixture)
    add_test(NAME reverse_windows_android_riso_boot
      COMMAND reverse_windows_android_boot_session_test --riso "${riso_iso}" "${riso_archive}")
    set_tests_properties(reverse_windows_android_riso_boot PROPERTIES TIMEOUT 60)
  endif()
  set(android_boot_fixture "${CMAKE_CURRENT_BINARY_DIR}/android-c03-boot.iso")
  add_test(NAME reverse_windows_android_synthetic_iso
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/c10_android_boot_iso.py"
      "${android_boot_fixture}")
  set_tests_properties(reverse_windows_android_synthetic_iso PROPERTIES FIXTURES_SETUP android_boot_iso)
  add_test(NAME reverse_windows_android_bios_iso_qssd
    COMMAND reverse_windows_android_boot_session_test "${android_boot_fixture}"
      "${CMAKE_CURRENT_BINARY_DIR}/c03-boot-evidence.json")
  set_tests_properties(reverse_windows_android_bios_iso_qssd PROPERTIES
    FIXTURES_REQUIRED android_boot_iso FIXTURES_SETUP ram_pressure_report TIMEOUT 60)
  add_test(NAME reverse_windows_android_ram_pressure_diagnostic
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ram_pressure_diagnostic.py"
      "${CMAKE_CURRENT_BINARY_DIR}/c03-boot-evidence.json")
  set_tests_properties(reverse_windows_android_ram_pressure_diagnostic PROPERTIES
    FIXTURES_REQUIRED ram_pressure_report)
  add_test(NAME reverse_windows_prime_boot_profile
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/prime_boot_profile.py"
      "$<TARGET_FILE:reverse_windows_boot_probe>"
      --evidence "${CMAKE_CURRENT_BINARY_DIR}/prime-c02-boot-profile.json")
  set_tests_properties(reverse_windows_prime_boot_profile PROPERTIES TIMEOUT 90)
  add_executable(reverse_windows_atapi_cdrom_guest_test tests/atapi_cdrom_guest_test.cpp)
  target_link_libraries(reverse_windows_atapi_cdrom_guest_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_android_atapi_guest COMMAND reverse_windows_atapi_cdrom_guest_test)
  set_tests_properties(reverse_windows_android_atapi_guest PROPERTIES TIMEOUT 60)
  add_executable(reverse_windows_protected_entry_test
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_group6_0f00_test.cpp")
  target_link_libraries(reverse_windows_protected_entry_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_protected_entry_stack COMMAND reverse_windows_protected_entry_test)
  set_tests_properties(reverse_windows_protected_entry_stack PROPERTIES TIMEOUT 60)
  add_executable(reverse_windows_protected_real_transition_test
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_protected_real_transition_test.cpp")
  target_link_libraries(reverse_windows_protected_real_transition_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_protected_real_transition COMMAND reverse_windows_protected_real_transition_test)
  set_tests_properties(reverse_windows_protected_real_transition PROPERTIES TIMEOUT 60)
  add_executable(reverse_windows_cpu_profile_test
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_cpu_profile_test.cpp")
  target_link_libraries(reverse_windows_cpu_profile_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_cpu_profile COMMAND reverse_windows_cpu_profile_test)
  add_executable(reverse_windows_lock_rmw_test
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_lock_rmw_test.cpp")
  target_link_libraries(reverse_windows_lock_rmw_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_lock_rmw COMMAND reverse_windows_lock_rmw_test)
  foreach(cpu_test IN ITEMS x86_long_mode x86_long_null_ss x86_far_transfer64 x86_system64
                            x86_ether_rep_batch x86_ether_shift_loop x86_ether_counter_loop
                            x86_ram_backing pc_video
                            pc_bios_keyboard boot_hotspots sparse_ram sparse_ram_pressure ether_state_ram pc_memory pc_memory_fabric)
    add_executable(reverse_windows_${cpu_test}_test
      "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/${cpu_test}_test.cpp")
    target_link_libraries(reverse_windows_${cpu_test}_test PRIVATE reverse_windows_component_headers)
    add_test(NAME reverse_windows_${cpu_test} COMMAND reverse_windows_${cpu_test}_test)
  endforeach()
  add_executable(reverse_windows_x86_ether_next_guest_test
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_ether_next_guest_test.cpp"
    "${REVERSE_MUNDO_X_ROOT}/native/src/ether_transform.cpp"
    "${REVERSE_MUNDO_X_ROOT}/native/src/ether_neon.cpp")
  target_compile_definitions(reverse_windows_x86_ether_next_guest_test PRIVATE MUNDO_X_PC_ETHER_NEXT=1)
  target_link_libraries(reverse_windows_x86_ether_next_guest_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_x86_ether_next_guest COMMAND reverse_windows_x86_ether_next_guest_test)
  add_executable(reverse_windows_prime_ether_arm64_test
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_ether_arm64_test.cpp")
  add_executable(reverse_windows_ether_core_rules_test
    "${REVERSE_MUNDO_X_ROOT}/native/tests/ether_rules.cpp")
  foreach(ether_test IN ITEMS reverse_windows_prime_ether_arm64_test reverse_windows_ether_core_rules_test)
    target_link_libraries(${ether_test} PRIVATE reverse_windows_component_headers)
    if(MSVC)
      target_compile_options(${ether_test} PRIVATE /O2)
    else()
      target_compile_options(${ether_test} PRIVATE -O2)
    endif()
  endforeach()
  add_test(NAME reverse_windows_prime_ether_arm64 COMMAND reverse_windows_prime_ether_arm64_test)
  set_tests_properties(reverse_windows_prime_ether_arm64 PROPERTIES TIMEOUT 60)
  add_test(NAME reverse_windows_ether_core_rules COMMAND reverse_windows_ether_core_rules_test)
  add_executable(reverse_windows_x64_optical_qssd_test tests/x64_optical_qssd_test.cpp)
  target_link_libraries(reverse_windows_x64_optical_qssd_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_x64_optical_qssd COMMAND reverse_windows_x64_optical_qssd_test)

  foreach(fpu_test IN ITEMS x86_sse_fp32 x86_sse2_packed_oracle x86_c09r1_stage5_cvt_fxsr
            x86_sse_legacy_forms x86_mmx x86_x87_arithmetic_oracle x86_x87_state)
    add_executable(reverse_windows_${fpu_test}_test
      "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/${fpu_test}_test.cpp")
    target_link_libraries(reverse_windows_${fpu_test}_test PRIVATE reverse_windows_component_headers)
    add_test(NAME reverse_windows_${fpu_test} COMMAND reverse_windows_${fpu_test}_test)
    set_tests_properties(reverse_windows_${fpu_test} PROPERTIES TIMEOUT 120)
  endforeach()

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
  add_executable(reverse_windows_prime_qssd_guest_test tests/prime_qssd_guest_test.cpp)
  target_link_libraries(reverse_windows_prime_qssd_guest_test PRIVATE reverse_windows_component_headers)
  target_compile_options(reverse_windows_prime_qssd_guest_test PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/O2,-O2>)
  add_test(NAME reverse_windows_prime_qssd_guest COMMAND reverse_windows_prime_qssd_guest_test)
  set_tests_properties(reverse_windows_prime_qssd_guest PROPERTIES TIMEOUT 90)
  add_executable(reverse_windows_prime_vbe_guest_test tests/prime_vbe_guest_test.cpp)
  target_link_libraries(reverse_windows_prime_vbe_guest_test PRIVATE reverse_windows_component_headers)
  target_compile_options(reverse_windows_prime_vbe_guest_test PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/O2,-O2>)
  add_test(NAME reverse_windows_prime_vbe_guest COMMAND reverse_windows_prime_vbe_guest_test)
  set_tests_properties(reverse_windows_prime_vbe_guest PROPERTIES TIMEOUT 90)
  add_executable(reverse_windows_prime_gpu_guest_test tests/prime_gpu_guest_test.cpp)
  target_link_libraries(reverse_windows_prime_gpu_guest_test PRIVATE reverse_windows_component_headers)
  target_compile_options(reverse_windows_prime_gpu_guest_test PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/O2,-O2>)
  if(ANDROID AND CMAKE_ANDROID_ARCH_ABI MATCHES "^(arm64-v8a|x86_64)$")
    target_link_options(reverse_windows_prime_gpu_guest_test PRIVATE
      "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
  endif()
  add_test(NAME reverse_windows_prime_gpu_guest COMMAND reverse_windows_prime_gpu_guest_test)
  set_tests_properties(reverse_windows_prime_gpu_guest PROPERTIES TIMEOUT 60
    ENVIRONMENT "EGL_PLATFORM=surfaceless")
  add_executable(reverse_windows_primevgc_transfer_test tests/gpu_vram_transfer_test.cpp)
  target_link_libraries(reverse_windows_primevgc_transfer_test PRIVATE reverse_windows_component_headers)
  target_compile_options(reverse_windows_primevgc_transfer_test PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/O2,-O3>)
  add_test(NAME reverse_windows_primevgc_transfer COMMAND reverse_windows_primevgc_transfer_test)
  add_test(NAME reverse_windows_primevgc_parallel_transfer
    COMMAND reverse_windows_primevgc_transfer_test --verify-parallel)
  set_tests_properties(reverse_windows_primevgc_transfer reverse_windows_primevgc_parallel_transfer PROPERTIES TIMEOUT 60)
  add_executable(reverse_windows_primevgc_compute_test tests/gpu_calculo_benchmark.cpp)
  target_link_libraries(reverse_windows_primevgc_compute_test PRIVATE reverse_windows_component_headers)
  target_compile_options(reverse_windows_primevgc_compute_test PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/O2,-O3>)
  add_test(NAME reverse_windows_primevgc_compute COMMAND reverse_windows_primevgc_compute_test)
  set_tests_properties(reverse_windows_primevgc_compute PROPERTIES TIMEOUT 60)
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
  add_executable(reverse_windows_smp_guest_test tests/smp_guest_test.cpp)
  target_link_libraries(reverse_windows_smp_guest_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_smp_guest COMMAND reverse_windows_smp_guest_test)
  add_executable(reverse_windows_smp_pause_wait_test tests/smp_pause_wait_test.cpp)
  target_link_libraries(reverse_windows_smp_pause_wait_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_smp_pause_wait COMMAND reverse_windows_smp_pause_wait_test)
  set_tests_properties(reverse_windows_smp_pause_wait PROPERTIES TIMEOUT 30)
  set_tests_properties(reverse_windows_smp_guest PROPERTIES TIMEOUT 30)
  add_executable(reverse_windows_ether_x86_sessions_test tests/ether_x86_sessions_test.cpp)
  target_link_libraries(reverse_windows_ether_x86_sessions_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_ether_x86_sessions COMMAND reverse_windows_ether_x86_sessions_test)
  set_tests_properties(reverse_windows_ether_x86_sessions PROPERTIES TIMEOUT 30)

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

  # Compile the FPU, FXSAVE and SSE2 tests from the pinned Mundo X checkout.
  add_executable(reverse_windows_c09r1_daz_ftz_test
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_c09r1_stage4_daz_ftz_test.cpp")
  target_link_libraries(reverse_windows_c09r1_daz_ftz_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c09r1_daz_ftz_mxcsr
    COMMAND reverse_windows_c09r1_daz_ftz_test)

  add_executable(reverse_windows_c09r1_stage4_mxcsr_test
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_c09r1_stage4_mxcsr_test.cpp")
  target_link_libraries(reverse_windows_c09r1_stage4_mxcsr_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c09r1_stage4_daz_ftz_oracles COMMAND reverse_windows_c09r1_stage4_mxcsr_test)

  add_executable(reverse_windows_c09r1_stage4_x87_test
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_c09r1_stage4_x87_test.cpp")
  target_link_libraries(reverse_windows_c09r1_stage4_x87_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c09r1_stage4_x87_pc_rc_stack COMMAND reverse_windows_c09r1_stage4_x87_test)

  add_executable(reverse_windows_c09r1_stage4_packed_cvt_test
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_c09r1_stage4_packed_cvt_test.cpp")
  target_link_libraries(reverse_windows_c09r1_stage4_packed_cvt_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c09r1_stage4_packed_cvt_mxcsr COMMAND reverse_windows_c09r1_stage4_packed_cvt_test)

  add_executable(reverse_windows_c09r1_stage4_sse2_misc_test
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_c09r1_stage4_sse2_misc_test.cpp")
  target_link_libraries(reverse_windows_c09r1_stage4_sse2_misc_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c09r1_stage4_sse2_cmp_shuffle COMMAND reverse_windows_c09r1_stage4_sse2_misc_test)

  add_executable(reverse_windows_c09r1_sse2_fp_test
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_c09r1_stage3_fp_test.cpp")
  target_link_libraries(reverse_windows_c09r1_sse2_fp_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c09r1_binary64_mxcsr_xm_ud
    COMMAND reverse_windows_c09r1_sse2_fp_test)

  add_executable(reverse_windows_c09r1_ring3_fpu_test
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_c09r1_stage3_ring3_test.cpp")
  target_link_libraries(reverse_windows_c09r1_ring3_fpu_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c09r1_cpl3_tss_fpu_fxsave
    COMMAND reverse_windows_c09r1_ring3_fpu_test)

  add_executable(reverse_windows_c09r1_exact_x87_test
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_c09r1_stage3_x87_precision_test.cpp")
  target_link_libraries(reverse_windows_c09r1_exact_x87_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c09r1_x87_ieee_to_ext80_oracle
    COMMAND reverse_windows_c09r1_exact_x87_test)

  add_executable(reverse_windows_c09r1_x87_mf_test
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_c09r1_stage3_x87_mf_test.cpp")
  target_link_libraries(reverse_windows_c09r1_x87_mf_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c09r1_x87_deferred_mf
    COMMAND reverse_windows_c09r1_x87_mf_test)

  add_executable(reverse_windows_c09r1_fxstate_test
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_c09r1_fxstate_test.cpp")
  target_link_libraries(reverse_windows_c09r1_fxstate_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c09r1_x87_fxsave_context
    COMMAND reverse_windows_c09r1_fxstate_test)

  add_executable(reverse_windows_c09r1_sse2_packed_test
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_c09r1_sse2_packed_test.cpp")
  target_link_libraries(reverse_windows_c09r1_sse2_packed_test PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c09r1_sse2_integer_packed
    COMMAND reverse_windows_c09r1_sse2_packed_test)

  add_executable(reverse_windows_c09r1_simd_subset
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_c09r1_xmm_subset_test.cpp")
  target_link_libraries(reverse_windows_c09r1_simd_subset PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c09r1_xmm_mxcsr_subset
    COMMAND reverse_windows_c09r1_simd_subset)

  add_executable(reverse_windows_c09r_cpu_features
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/tests/x86_windows10_c09r_cpu_readiness_test.cpp")
  target_link_libraries(reverse_windows_c09r_cpu_features PRIVATE reverse_windows_component_headers)
  add_test(NAME reverse_windows_c09r_real_cpu_cx16_prefetchw
    COMMAND reverse_windows_c09r_cpu_features)

  add_test(NAME reverse_windows_c09r_iso_free_hardware_preflight
    COMMAND reverse_windows_c09r_hardware_preflight --self-test)

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
                       reverse_windows_c09r1_xmm_mxcsr_subset
                       reverse_windows_c09r1_x87_fxsave_context
                       reverse_windows_c09r1_sse2_integer_packed
                       reverse_windows_c09r1_binary64_mxcsr_xm_ud
                       reverse_windows_c09r1_cpl3_tss_fpu_fxsave
                       reverse_windows_c09r1_x87_ieee_to_ext80_oracle
                       reverse_windows_c09r1_x87_deferred_mf
                       reverse_windows_c09r1_stage4_daz_ftz_oracles
                       reverse_windows_c09r1_stage4_x87_pc_rc_stack
                       reverse_windows_c09r1_stage4_packed_cvt_mxcsr
                       reverse_windows_c09r1_stage4_sse2_cmp_shuffle
                       reverse_windows_c09r1_daz_ftz_mxcsr PROPERTIES TIMEOUT 120)
endif()
