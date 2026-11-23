cmake_minimum_required(VERSION 3.20)
project(reverse_windows_boot_ab LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(REVERSE_WINDOWS_SOURCE "" CACHE PATH "Reverse Windows source checkout")
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(observer_include "${CMAKE_CURRENT_BINARY_DIR}/observer-include")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare-observer.py"
  "${REVERSE_WINDOWS_SOURCE}" "${observer_include}/android_boot_session.hpp"
  COMMAND_ERROR_IS_FATAL ANY)
add_subdirectory("${REVERSE_WINDOWS_SOURCE}" reverse-windows EXCLUDE_FROM_ALL)
add_executable(session_probe session_probe.cpp
  "${REVERSE_MUNDO_X_ROOT}/native/src/ether_transform.cpp"
  "${REVERSE_MUNDO_X_ROOT}/native/src/ether_neon.cpp")
target_compile_definitions(session_probe PRIVATE MUNDO_X_PC_ETHER_NEXT=1)
target_include_directories(session_probe BEFORE PRIVATE "${observer_include}")
target_link_libraries(session_probe PRIVATE reverse_windows_component_headers OpenSSL::Crypto)
