cmake_minimum_required(VERSION 3.20)

function(reverse_windows_check_mundo_x lock_file checkout)
  file(READ "${lock_file}" lock)
  string(JSON count LENGTH "${lock}" components)
  set(pinned_sha "")
  if(count GREATER 0)
    math(EXPR last "${count} - 1")
    foreach(index RANGE 0 ${last})
      string(JSON key GET "${lock}" components ${index} key)
      if(key STREQUAL "mundo_x")
        string(JSON pinned_sha GET "${lock}" components ${index} commit)
        break()
      endif()
    endforeach()
  endif()
  string(LENGTH "${pinned_sha}" sha_length)
  if(NOT sha_length EQUAL 40 OR NOT pinned_sha MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "Config must pin a full Mundo X commit in ${lock_file}")
  endif()
  if(DEFINED REVERSE_CONFIGURED_MUNDO_X_SHA AND
     NOT pinned_sha STREQUAL REVERSE_CONFIGURED_MUNDO_X_SHA)
    message(FATAL_ERROR "Mundo X pin changed since configure. Configure this build again.")
  endif()

  find_package(Git REQUIRED)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${checkout}" rev-parse --verify HEAD
    RESULT_VARIABLE result OUTPUT_VARIABLE actual OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT result STREQUAL "0" OR NOT actual STREQUAL pinned_sha)
    message(FATAL_ERROR
      "Mundo X checkout at ${checkout} must be ${pinned_sha}, found '${actual}'. Run python3 scripts/verify_deps.py --checkout.")
  endif()
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${checkout}" diff --quiet --no-ext-diff HEAD --
    pc-android/native native/include/mundo_x native/src/ether_transform.cpp native/src/ether_neon.cpp
    RESULT_VARIABLE dirty)
  if(NOT dirty STREQUAL "0")
    message(FATAL_ERROR "Mundo X native source differs from pinned commit ${pinned_sha} in ${checkout}.")
  endif()
  set(REVERSE_MUNDO_X_SOURCE_SHA "${pinned_sha}" PARENT_SCOPE)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
  reverse_windows_check_mundo_x("${REVERSE_LOCK_FILE}" "${REVERSE_MUNDO_X_CHECKOUT}")
else()
  function(reverse_windows_pin_mundo_x lock_file checkout)
    reverse_windows_check_mundo_x("${lock_file}" "${checkout}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${lock_file}")
    add_custom_target(reverse_windows_verify_mundo_x
      COMMAND "${CMAKE_COMMAND}"
        "-DREVERSE_LOCK_FILE=${lock_file}" "-DREVERSE_MUNDO_X_CHECKOUT=${checkout}"
        "-DREVERSE_CONFIGURED_MUNDO_X_SHA=${REVERSE_MUNDO_X_SOURCE_SHA}"
        -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
      VERBATIM)
    set(REVERSE_MUNDO_X_SOURCE_SHA "${REVERSE_MUNDO_X_SOURCE_SHA}" PARENT_SCOPE)
  endfunction()
endif()
