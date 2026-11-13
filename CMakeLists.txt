cmake_minimum_required(VERSION 3.24)

project(
  omnibuf_cpp
  VERSION 0.1.0
  DESCRIPTION "Modern C++20 implementation of the OmniBuf wire format"
  LANGUAGES CXX
)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# Header-only C++20 OmniBuf library
add_library(omnibuf INTERFACE)
target_include_directories(
  omnibuf
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(omnibuf INTERFACE cxx_std_20)

# protoc-gen-omnibuf-cpp plugin binary (invoked by buf generate)
add_executable(protoc-gen-omnibuf-cpp tools/protoc_gen_omnibuf_cpp.cpp)
target_link_libraries(protoc-gen-omnibuf-cpp PRIVATE omnibuf)

# Buf schema compilation and C++20 code generation
find_program(BUF_EXECUTABLE NAMES buf REQUIRED)

set(OMNIBUF_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(OMNIBUF_GEN_HEADERS
  "${OMNIBUF_GEN_DIR}/omnibuf/bench/peer.omnibuf.pb.hpp"
  "${OMNIBUF_GEN_DIR}/omnibuf/conformance/conformance.omnibuf.pb.hpp"
)
set(OMNIBUF_SCHEMAS_FDS "${OMNIBUF_GEN_DIR}/schemas.fds.pb")
set(OMNIBUF_PROTO_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/proto/omnibuf/bench/peer.proto"
  "${CMAKE_CURRENT_SOURCE_DIR}/proto/omnibuf/conformance/conformance.proto"
)
set(OMNIBUF_CMAKE_BUF_GEN "${CMAKE_CURRENT_BINARY_DIR}/buf.gen.cmake.yaml")

file(
  GENERATE
  OUTPUT "${OMNIBUF_CMAKE_BUF_GEN}"
  CONTENT "version: v2\nplugins:\n  - local: $<TARGET_FILE:protoc-gen-omnibuf-cpp>\n    out: ${OMNIBUF_GEN_DIR}\n"
)

add_custom_command(
  OUTPUT ${OMNIBUF_GEN_HEADERS} "${OMNIBUF_SCHEMAS_FDS}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OMNIBUF_GEN_DIR}"
  COMMAND "${BUF_EXECUTABLE}" generate --template "${OMNIBUF_CMAKE_BUF_GEN}"
  COMMAND "${BUF_EXECUTABLE}" build -o "${OMNIBUF_SCHEMAS_FDS}" --exclude-source-info
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  DEPENDS
    protoc-gen-omnibuf-cpp
    "${OMNIBUF_CMAKE_BUF_GEN}"
    "${CMAKE_CURRENT_SOURCE_DIR}/buf.yaml"
    ${OMNIBUF_PROTO_FILES}
  COMMENT "Generating C++20 OmniBuf headers and binary FileDescriptorSet with buf"
  VERBATIM
)

add_custom_target(omnibuf_codegen DEPENDS ${OMNIBUF_GEN_HEADERS} "${OMNIBUF_SCHEMAS_FDS}")

# Conformance adapter executable (all 4 tiers)
add_executable(conformance-cpp tools/conformance_cpp.cpp)
add_dependencies(conformance-cpp omnibuf_codegen)
target_include_directories(conformance-cpp PRIVATE "${OMNIBUF_GEN_DIR}")
target_link_libraries(conformance-cpp PRIVATE omnibuf)
target_compile_definitions(
  conformance-cpp
  PRIVATE OMNIBUF_SCHEMAS_FDS_PATH="${OMNIBUF_SCHEMAS_FDS}"
)

# Unit test suite
enable_testing()
add_executable(omnibuf_tests tests/omnibuf_tests.cpp)
add_dependencies(omnibuf_tests omnibuf_codegen)
target_include_directories(omnibuf_tests PRIVATE "${OMNIBUF_GEN_DIR}")
target_link_libraries(omnibuf_tests PRIVATE omnibuf)
target_compile_definitions(
  omnibuf_tests
  PRIVATE OMNIBUF_SCHEMAS_FDS_PATH="${OMNIBUF_SCHEMAS_FDS}"
)
add_test(NAME omnibuf_tests COMMAND omnibuf_tests)

# Reproducible PeerUpdate benchmark executable (always compiled with -O3 -DNDEBUG)
add_executable(omnibuf_bench benchmarks/peer_bench.cpp)
add_dependencies(omnibuf_bench omnibuf_codegen)
target_include_directories(omnibuf_bench PRIVATE "${OMNIBUF_GEN_DIR}")
target_link_libraries(omnibuf_bench PRIVATE omnibuf)
target_compile_options(omnibuf_bench PRIVATE -O3 -DNDEBUG)
target_compile_definitions(
  omnibuf_bench
  PRIVATE OMNIBUF_BENCH_JSON_PATH="${CMAKE_CURRENT_SOURCE_DIR}/testdata/benchmarks.json"
)
