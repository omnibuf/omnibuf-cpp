cmake_minimum_required(VERSION 3.24)

project(
  omnibuf_cpp
  VERSION 0.1.7
  DESCRIPTION "Modern C++20 implementation of the OmniBuf wire format"
  LANGUAGES CXX
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(
  OMNIBUF_BUILD_PLUGIN
  "Build protoc-gen-omnibuf-cpp plugin binary"
  ${PROJECT_IS_TOP_LEVEL}
)
option(
  OMNIBUF_BUILD_TESTS
  "Build OmniBuf conformance adapter, unit tests, and benchmarks"
  ${PROJECT_IS_TOP_LEVEL}
)
option(
  OMNIBUF_INSTALL
  "Generate CMake install rules for the header-only omnibuf library"
  ${PROJECT_IS_TOP_LEVEL}
)
option(OMNIBUF_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(PROJECT_IS_TOP_LEVEL)
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  endif()
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# Header-only C++20 OmniBuf library (consumable via FetchContent, CPM.cmake, add_subdirectory, or find_package)
add_library(omnibuf INTERFACE)
add_library(omnibuf::omnibuf ALIAS omnibuf)
target_include_directories(
  omnibuf
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(omnibuf INTERFACE cxx_std_20)

if(OMNIBUF_INSTALL)
  install(
    TARGETS omnibuf
    EXPORT omnibufTargets
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  )
  install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/omnibuf"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  )
  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/omnibufConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/omnibufConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/omnibuf"
  )
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/omnibufConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
  )
  install(
    EXPORT omnibufTargets
    FILE omnibufTargets.cmake
    NAMESPACE omnibuf::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/omnibuf"
  )
  install(
    FILES
      "${CMAKE_CURRENT_BINARY_DIR}/omnibufConfig.cmake"
      "${CMAKE_CURRENT_BINARY_DIR}/omnibufConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/omnibuf"
  )
endif()

if(OMNIBUF_BUILD_PLUGIN OR OMNIBUF_BUILD_TESTS)
  if(MSVC)
    set(OMNIBUF_WARN_FLAGS /W4)
  else()
    set(OMNIBUF_WARN_FLAGS -Wall -Wextra -Wpedantic -Werror)
  endif()
  set(OMNIBUF_SAN_FLAGS "")
  if(OMNIBUF_ENABLE_SANITIZERS AND NOT MSVC)
    list(APPEND OMNIBUF_SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
  endif()

  function(omnibuf_configure_internal_target target_name)
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    set_target_properties(${target_name} PROPERTIES CXX_EXTENSIONS OFF)
    target_compile_options(${target_name} PRIVATE ${OMNIBUF_WARN_FLAGS} ${OMNIBUF_SAN_FLAGS})
    if(OMNIBUF_ENABLE_SANITIZERS AND NOT MSVC)
      target_link_options(${target_name} PRIVATE ${OMNIBUF_SAN_FLAGS})
    endif()
  endfunction()

  # protoc-gen-omnibuf-cpp plugin binary (invoked by buf generate or protoc)
  add_executable(protoc-gen-omnibuf-cpp tools/protoc_gen_omnibuf_cpp.cpp)
  target_link_libraries(protoc-gen-omnibuf-cpp PRIVATE omnibuf)
  omnibuf_configure_internal_target(protoc-gen-omnibuf-cpp)
endif()

if(OMNIBUF_BUILD_TESTS)
  # Buf schema compilation and C++20 code generation
  find_program(BUF_EXECUTABLE NAMES buf REQUIRED)

  set(OMNIBUF_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
  set(OMNIBUF_GEN_HEADERS
    "${OMNIBUF_GEN_DIR}/omnibuf/bench/peer.omnibuf.pb.hpp"
    "${OMNIBUF_GEN_DIR}/omnibuf/conformance/conformance.omnibuf.pb.hpp"
    "${OMNIBUF_GEN_DIR}/omnibuf/conformance/editions_2023.omnibuf.pb.hpp"
    "${OMNIBUF_GEN_DIR}/omnibuf/conformance/editions_2024.omnibuf.pb.hpp"
    "${OMNIBUF_GEN_DIR}/omnibuf/conformance/editions_2026.omnibuf.pb.hpp"
    "${OMNIBUF_GEN_DIR}/omnibuf/test/edge.omnibuf.pb.hpp"
  )
  set(OMNIBUF_SCHEMAS_FDS "${OMNIBUF_GEN_DIR}/schemas.fds.pb")
  set(OMNIBUF_PROTO_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/proto/omnibuf/bench/peer.proto"
    "${CMAKE_CURRENT_SOURCE_DIR}/proto/omnibuf/conformance/conformance.proto"
    "${CMAKE_CURRENT_SOURCE_DIR}/proto/omnibuf/conformance/editions_2023.proto"
    "${CMAKE_CURRENT_SOURCE_DIR}/proto/omnibuf/conformance/editions_2024.proto"
    "${CMAKE_CURRENT_SOURCE_DIR}/proto/omnibuf/conformance/editions_2026.proto"
    "${CMAKE_CURRENT_SOURCE_DIR}/proto/omnibuf/test/edge.proto"
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
  omnibuf_configure_internal_target(conformance-cpp)
  target_compile_definitions(
    conformance-cpp
    PRIVATE OMNIBUF_SCHEMAS_FDS_PATH="${OMNIBUF_SCHEMAS_FDS}"
  )

  # Unit test suites
  enable_testing()

  function(omnibuf_add_unit_test target_name source_file)
    add_executable(${target_name} "${source_file}")
    add_dependencies(${target_name} omnibuf_codegen)
    target_include_directories(${target_name} PRIVATE "${OMNIBUF_GEN_DIR}")
    target_link_libraries(${target_name} PRIVATE omnibuf)
    omnibuf_configure_internal_target(${target_name})
    target_compile_definitions(
      ${target_name}
      PRIVATE OMNIBUF_SCHEMAS_FDS_PATH="${OMNIBUF_SCHEMAS_FDS}"
    )
    add_test(NAME ${target_name} COMMAND ${target_name})
  endfunction()

  omnibuf_add_unit_test(omnibuf_tests tests/omnibuf_tests.cpp)
  omnibuf_add_unit_test(opaque_test tests/opaque_test.cpp)
  omnibuf_add_unit_test(wire_and_traits_test tests/wire_and_traits_test.cpp)
  omnibuf_add_unit_test(grpc_codec_test tests/grpc_codec_test.cpp)
  target_sources(grpc_codec_test PRIVATE tests/grpc_traits_mock.cpp)
  omnibuf_add_unit_test(codegen_plugin_test tests/codegen_plugin_test.cpp)
  add_dependencies(codegen_plugin_test protoc-gen-omnibuf-cpp)
  target_compile_definitions(
    codegen_plugin_test
    PRIVATE OMNIBUF_PLUGIN_BIN="$<TARGET_FILE:protoc-gen-omnibuf-cpp>"
  )

  # Protobuf Editions (2023, 2024, 2026) end-to-end codegen + runtime tests via protoc
  find_program(OMNIBUF_PROTOC_EXECUTABLE NAMES protoc)
  if(OMNIBUF_PROTOC_EXECUTABLE)
    set(OMNIBUF_EDITIONS_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen_editions")
    set(OMNIBUF_EDITIONS_FDS "${OMNIBUF_EDITIONS_GEN_DIR}/editions.fds.pb")
    set(OMNIBUF_EDITIONS_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/testdata")
    set(OMNIBUF_EDITIONS_PROTOS
      "${OMNIBUF_EDITIONS_PROTO_DIR}/editions_2023.proto"
      "${OMNIBUF_EDITIONS_PROTO_DIR}/editions_2023_implicit.proto"
      "${OMNIBUF_EDITIONS_PROTO_DIR}/editions_2024.proto"
      "${OMNIBUF_EDITIONS_PROTO_DIR}/editions_2026.proto"
    )
    set(OMNIBUF_EDITIONS_HEADERS
      "${OMNIBUF_EDITIONS_GEN_DIR}/editions_2023.omnibuf.pb.hpp"
      "${OMNIBUF_EDITIONS_GEN_DIR}/editions_2023_implicit.omnibuf.pb.hpp"
      "${OMNIBUF_EDITIONS_GEN_DIR}/editions_2024.omnibuf.pb.hpp"
      "${OMNIBUF_EDITIONS_GEN_DIR}/editions_2026.omnibuf.pb.hpp"
    )
    add_custom_command(
      OUTPUT ${OMNIBUF_EDITIONS_HEADERS} "${OMNIBUF_EDITIONS_FDS}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${OMNIBUF_EDITIONS_GEN_DIR}"
      COMMAND "${OMNIBUF_PROTOC_EXECUTABLE}"
        "--plugin=protoc-gen-omnibuf-cpp=$<TARGET_FILE:protoc-gen-omnibuf-cpp>"
        "--omnibuf-cpp_out=${OMNIBUF_EDITIONS_GEN_DIR}"
        "--descriptor_set_out=${OMNIBUF_EDITIONS_FDS}"
        "-I${OMNIBUF_EDITIONS_PROTO_DIR}"
        ${OMNIBUF_EDITIONS_PROTOS}
      DEPENDS
        protoc-gen-omnibuf-cpp
        ${OMNIBUF_EDITIONS_PROTOS}
      COMMENT "Generating C++20 OmniBuf headers for Protobuf Editions 2023/2024/2026 with protoc"
      VERBATIM
    )
    add_custom_target(
      omnibuf_editions_codegen
      DEPENDS ${OMNIBUF_EDITIONS_HEADERS} "${OMNIBUF_EDITIONS_FDS}"
    )

    add_executable(editions_e2e_test tests/editions_e2e_test.cpp)
    add_dependencies(editions_e2e_test omnibuf_editions_codegen protoc-gen-omnibuf-cpp)
    target_include_directories(editions_e2e_test PRIVATE "${OMNIBUF_EDITIONS_GEN_DIR}")
    target_link_libraries(editions_e2e_test PRIVATE omnibuf)
    omnibuf_configure_internal_target(editions_e2e_test)
    target_compile_definitions(
      editions_e2e_test
      PRIVATE
        OMNIBUF_EDITIONS_FDS_PATH="${OMNIBUF_EDITIONS_FDS}"
        OMNIBUF_PLUGIN_BIN="$<TARGET_FILE:protoc-gen-omnibuf-cpp>"
        OMNIBUF_PROTOC_BIN="${OMNIBUF_PROTOC_EXECUTABLE}"
    )
    add_test(NAME editions_e2e_test COMMAND editions_e2e_test)
  endif()

  # Reproducible PeerUpdate benchmark executable (always compiled with -O3 -DNDEBUG)
  add_executable(omnibuf_bench benchmarks/peer_bench.cpp)
  add_dependencies(omnibuf_bench omnibuf_codegen)
  target_include_directories(omnibuf_bench PRIVATE "${OMNIBUF_GEN_DIR}")
  target_link_libraries(omnibuf_bench PRIVATE omnibuf)
  omnibuf_configure_internal_target(omnibuf_bench)
  target_compile_options(omnibuf_bench PRIVATE -O3 -DNDEBUG)
  target_compile_definitions(
    omnibuf_bench
    PRIVATE OMNIBUF_BENCH_JSON_PATH="${CMAKE_CURRENT_SOURCE_DIR}/testdata/benchmarks.json"
  )

  # Optional real libprotobuf (google::protobuf::Message) benchmark peer when installed.
  # Keeps omnibuf::omnibuf strictly zero-dependency; only links into omnibuf_bench.
  find_package(protobuf CONFIG QUIET)
  if(protobuf_FOUND AND OMNIBUF_PROTOC_EXECUTABLE)
    set(OMNIBUF_PB_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/pb_gen")
    set(OMNIBUF_PB_PROTO_IN "${OMNIBUF_PB_GEN_DIR}/libpb_peer.proto")
    set(OMNIBUF_PB_GEN_CC "${OMNIBUF_PB_GEN_DIR}/libpb_peer.pb.cc")
    set(OMNIBUF_PB_GEN_H "${OMNIBUF_PB_GEN_DIR}/libpb_peer.pb.h")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/proto/omnibuf/bench/peer.proto" OMNIBUF_PEER_PROTO_CONTENT)
    string(REPLACE "package omnibuf.bench;" "package libpb.bench;" OMNIBUF_LIBPB_PROTO_CONTENT "${OMNIBUF_PEER_PROTO_CONTENT}")
    file(WRITE "${OMNIBUF_PB_PROTO_IN}" "${OMNIBUF_LIBPB_PROTO_CONTENT}")
    add_custom_command(
      OUTPUT "${OMNIBUF_PB_GEN_CC}" "${OMNIBUF_PB_GEN_H}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${OMNIBUF_PB_GEN_DIR}"
      COMMAND "${OMNIBUF_PROTOC_EXECUTABLE}"
        "--proto_path=${OMNIBUF_PB_GEN_DIR}"
        "--cpp_out=${OMNIBUF_PB_GEN_DIR}"
        "${OMNIBUF_PB_PROTO_IN}"
      DEPENDS "${OMNIBUF_PB_PROTO_IN}"
      COMMENT "Generating official libprotobuf C++ peer benchmark source"
      VERBATIM
    )
    add_library(
      omnibuf_bench_libprotobuf STATIC
      benchmarks/libprotobuf_peer.cpp
      "${OMNIBUF_PB_GEN_CC}"
    )
    target_compile_features(omnibuf_bench_libprotobuf PRIVATE cxx_std_20)
    target_compile_options(omnibuf_bench_libprotobuf PRIVATE -O3 -DNDEBUG)
    target_include_directories(omnibuf_bench_libprotobuf PRIVATE "${OMNIBUF_PB_GEN_DIR}")
    target_link_libraries(omnibuf_bench_libprotobuf PRIVATE protobuf::libprotobuf)
    target_link_libraries(omnibuf_bench PRIVATE omnibuf_bench_libprotobuf)
    target_compile_definitions(omnibuf_bench PRIVATE OMNIBUF_HAS_LIBPROTOBUF=1)
    message(STATUS "omnibuf_bench: enabling official libprotobuf peer comparison")
  endif()
endif()
