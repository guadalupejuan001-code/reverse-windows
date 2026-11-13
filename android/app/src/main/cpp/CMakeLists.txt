cmake_minimum_required(VERSION 3.22)
project(reverse_windows_android LANGUAGES CXX)

get_filename_component(REVERSE_WINDOWS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
set(REVERSE_MUNDO_X_ROOT "${REVERSE_WINDOWS_ROOT}/deps/mundo-X" CACHE PATH "Pinned Mundo X source")
set(REVERSE_QSSD_ROOT "${REVERSE_WINDOWS_ROOT}/deps/Virtual-qSSD-" CACHE PATH "Pinned QSSD source")
set(REVERSE_GPU_VIRTUAL_ROOT "${REVERSE_WINDOWS_ROOT}/deps/GPU-VIRTUAL" CACHE PATH "Pinned GPU Virtual source")

foreach(required_header IN ITEMS
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native/pc_machine.hpp"
    "${REVERSE_QSSD_ROOT}/include/qssd/state_bus.hpp"
    "${REVERSE_GPU_VIRTUAL_ROOT}/include/trabajo_fisico.hpp")
    if(NOT EXISTS "${required_header}")
        message(FATAL_ERROR "Missing native component: ${required_header}. Check out config/dependencies.lock.json and run scripts/verify_deps.py.")
    endif()
endforeach()

find_package(Threads REQUIRED)
add_library(reverse_windows_android SHARED
    native_machine.cpp
    "${REVERSE_MUNDO_X_ROOT}/native/src/ether_transform.cpp"
    "${REVERSE_MUNDO_X_ROOT}/native/src/ether_neon.cpp")
target_compile_features(reverse_windows_android PRIVATE cxx_std_17)
target_compile_definitions(reverse_windows_android PRIVATE MUNDO_X_PC_ETHER_NEXT=1)
target_include_directories(reverse_windows_android PRIVATE
    "${REVERSE_WINDOWS_ROOT}/src"
    "${REVERSE_MUNDO_X_ROOT}/pc-android/native"
    "${REVERSE_MUNDO_X_ROOT}/native/include"
    "${REVERSE_QSSD_ROOT}/include"
    "${REVERSE_GPU_VIRTUAL_ROOT}/include")
target_link_libraries(reverse_windows_android PRIVATE Threads::Threads)
target_compile_options(reverse_windows_android PRIVATE -fvisibility=hidden)
