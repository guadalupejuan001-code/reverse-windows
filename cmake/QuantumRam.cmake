# The vector engine has a deliberately private compatibility surface.  Only its
# implementation translation unit sees the generated headers; PcMachine, qSSD
# and REgraphics retain their own, exactly pinned public ABI.
function(reverse_windows_add_quantum_ram root)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(compat "${CMAKE_CURRENT_BINARY_DIR}/quantum-ram-compat")
  set(prepare "${root}/scripts/prepare_quantum_ram_compat.py")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${prepare}"
      --mundo-x "${REVERSE_MUNDO_X_ROOT}"
      --reverse-pc "${REVERSE_REGRAPHICS_ROOT}"
      --output "${compat}"
    RESULT_VARIABLE prepared OUTPUT_VARIABLE prepare_output ERROR_VARIABLE prepare_error)
  if(NOT prepared EQUAL 0)
    message(FATAL_ERROR "Quantum RAM compatibility verification failed:\n${prepare_output}${prepare_error}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${prepare}" "${root}/config/dependencies.lock.json"
    "${REVERSE_REGRAPHICS_ROOT}/scripts/ether_integration.py"
    "${REVERSE_MUNDO_X_ROOT}/native/include/mundo_x/exact_t_experiment.hpp"
    "${REVERSE_MUNDO_X_ROOT}/native/include/mundo_x/infinite_core.hpp"
    "${REVERSE_MUNDO_X_ROOT}/native/include/mundo_x/quantum_runtime.hpp"
    "${REVERSE_MUNDO_X_ROOT}/native/include/mundo_x/infinite_program.hpp"
    "${REVERSE_MUNDO_X_ROOT}/native/include/mundo_x/inverse_memory.hpp")
  add_library(reverse_windows_quantum_ram STATIC "${root}/src/quantum_ram_authority.cpp")
  set_target_properties(reverse_windows_quantum_ram PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_features(reverse_windows_quantum_ram PUBLIC cxx_std_17)
  target_compile_definitions(reverse_windows_quantum_ram PUBLIC MUNDO_X_PC_STATE_RAM=1)
  target_include_directories(reverse_windows_quantum_ram BEFORE PRIVATE
    "${compat}" "${root}/src" "${REVERSE_REGRAPHICS_ROOT}/src"
    "${REVERSE_MUNDO_X_ROOT}/native/include" "${REVERSE_MUNDO_X_ROOT}/pc-android/native")
  target_link_libraries(reverse_windows_quantum_ram PRIVATE Threads::Threads)
  add_dependencies(reverse_windows_quantum_ram reverse_windows_verify_mundo_x)
  if(MSVC)
    target_compile_options(reverse_windows_quantum_ram PRIVATE /fp:strict)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Preserve inverse-gate cancellation when restoring bytes on ARM64.
    target_compile_options(reverse_windows_quantum_ram PRIVATE
      -frounding-math -ffp-contract=off -fvisibility=hidden)
  endif()
endfunction()
