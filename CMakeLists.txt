cmake_minimum_required(VERSION 3.26)

project(bernardx LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ── Third-party dependency paths ──────────────────────────────────────────────
# Defaults point to git submodules under 3rd/.
# Parent projects can override any path via cache variable.

set(BERNARDX_3RD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rd" CACHE PATH "Third-party deps root")

set(ASIO_DIR        "${BERNARDX_3RD_DIR}/asio"           CACHE PATH "asio source dir")
set(ASYNC_SIMPLE_DIR "${BERNARDX_3RD_DIR}/async_simple"  CACHE PATH "async_simple source dir")
set(LUA_DIR         "${BERNARDX_3RD_DIR}/lua"            CACHE PATH "lua source dir")
set(SOL2_DIR        "${BERNARDX_3RD_DIR}/sol2"           CACHE PATH "sol2 source dir")
set(SPDLOG_DIR      "${BERNARDX_3RD_DIR}/spdlog"         CACHE PATH "spdlog source dir")
set(OPENSSL_DIR     "${BERNARDX_3RD_DIR}/openssl"        CACHE PATH "openssl source dir")
set(CINATRA_DIR     "${BERNARDX_3RD_DIR}/cinatra"        CACHE PATH "cinatra source dir")
set(JSON_DIR        "${BERNARDX_3RD_DIR}/json"            CACHE PATH "nlohmann json source dir")
set(GFLAGS_DIR      "${BERNARDX_3RD_DIR}/gflags"         CACHE PATH "gflags source dir")
set(GOOGLETEST_DIR  "${BERNARDX_3RD_DIR}/googletest"     CACHE PATH "googletest source dir")

add_definitions(-DASIO_STANDALONE)

# ── asio (header-only) ────────────────────────────────────────────────────────
# Provide asio_SOURCE_DIR so cinatra's CMake can reuse it.
set(asio_SOURCE_DIR ${ASIO_DIR} CACHE PATH "asio source" FORCE)

# ── async_simple ──────────────────────────────────────────────────────────────
set(ASYNC_SIMPLE_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ASYNC_SIMPLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ASYNC_SIMPLE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ASYNC_SIMPLE_ENABLE_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(ASYNC_SIMPLE_BUILD_MODULES OFF CACHE BOOL "" FORCE)
set(ASYNC_SIMPLE_BUILD_DEMO_EXAMPLE OFF CACHE BOOL "" FORCE)
# Set async_simple_SOURCE_DIR so cinatra reuses this instead of fetching again
set(async_simple_SOURCE_DIR ${ASYNC_SIMPLE_DIR} CACHE PATH "async_simple source" FORCE)
add_subdirectory(${ASYNC_SIMPLE_DIR})

# ── Lua ───────────────────────────────────────────────────────────────────────
file(GLOB LUA_SOURCES ${LUA_DIR}/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX ".*ltests\\.c|.*onelua\\.c")
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC ${LUA_DIR})
set_property(TARGET lua PROPERTY C_STANDARD 99)
if(UNIX)
    target_link_libraries(lua PRIVATE dl m)
endif()

# ── sol2 ──────────────────────────────────────────────────────────────────────
set(SOL2_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${SOL2_DIR})

# ── spdlog ────────────────────────────────────────────────────────────────────
set(SPDLOG_HEADER_ONLY ON CACHE BOOL "" FORCE)
add_subdirectory(${SPDLOG_DIR})

# ── OpenSSL ───────────────────────────────────────────────────────────────────
if(ENABLE_SSL)
    find_package(OpenSSL QUIET)
    if(NOT OpenSSL_FOUND)
        include(${CINATRA_DIR}/cmake/find_openssl.cmake)
    endif()
endif()

# ── cinatra ───────────────────────────────────────────────────────────────────
if(NOT ENABLE_SSL)
    set(ENABLE_SSL OFF CACHE BOOL "" FORCE)
endif()
set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_PRESS_TOOL OFF CACHE BOOL "" FORCE)
# Allow CINATRA_DIR to point outside this source tree (e.g. parent project override)
string(FIND "${CINATRA_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/" _cinatra_pos)
if(_cinatra_pos EQUAL 0)
    add_subdirectory(${CINATRA_DIR})
else()
    add_subdirectory(${CINATRA_DIR} ${CMAKE_BINARY_DIR}/cinatra)
endif()

# ── nlohmann/json ────────────────────────────────────────────────────────────
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
add_subdirectory(${JSON_DIR})

# Export source dirs to parent scope
set(asio_SOURCE_DIR ${ASIO_DIR} PARENT_SCOPE)
set(cinatra_SOURCE_DIR ${CINATRA_DIR} PARENT_SCOPE)

# ── BernardX library ─────────────────────────────────────────────────────────

add_library(bernardx_lib STATIC
    src/lua/lua_runtime.cc
    src/lua/timer_manager.cc
    src/lua/coroutine_pool.cc
    src/lua/http_library.cc
    src/lua/json_library.cc
    src/lua/fs_library.cc
    src/lua/blackboard_library.cc
    src/lua/bt_library.cc
    src/lua/resource_library.cc
    src/lua/async_io_library.cc
    src/lua/file_system_resource_provider.cc
    src/bt/behavior_tree_engine.cc
    src/bt/path_tracer.cc
    src/bt/tree_parser.cc
    src/bt/single_child_node.cc
    src/bt/lua_script_host.cc
    src/bt/script_node.cc
    src/bt/script_condition.cc
    src/bt/condition_composite.cc
    src/bt/node.cc
    src/bt/composite.cc
    src/bt/selector.cc
    src/bt/sequence.cc
    src/bt/pipeline.cc
    src/bt/parallel.cc
    src/bt/inverter.cc
    src/bt/force_success.cc
    src/bt/force_failure.cc
    src/bt/repeat.cc
    src/bt/retry_until_successful.cc
    src/bt/random_selector.cc
    src/bt/random_sequence.cc
    src/bt/wait_node.cc
)
target_include_directories(bernardx_lib PUBLIC src/lua src/bt)
cinatra_use_third_party_headers(bernardx_lib)
target_include_directories(bernardx_lib SYSTEM PRIVATE ${CINATRA_DIR}/include)
if(OpenSSL_FOUND)
    target_compile_definitions(bernardx_lib PUBLIC CINATRA_ENABLE_SSL)
endif()
find_package(Threads REQUIRED)
target_link_libraries(bernardx_lib PUBLIC
    sol2::sol2
    spdlog::spdlog_header_only
    lua
    async_simple_static
    nlohmann_json::nlohmann_json
)
target_link_libraries(bernardx_lib PRIVATE
    Threads::Threads
)
# libdl is POSIX-only. On Windows asio additionally needs winsock.
if(WIN32)
    target_link_libraries(bernardx_lib PRIVATE ws2_32 mswsock)
else()
    target_link_libraries(bernardx_lib PRIVATE dl)
endif()
if(OpenSSL_FOUND)
    target_link_libraries(bernardx_lib PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# ── bernardx executable ──────────────────────────────────────────────────────

if(NOT BERNARDX_BUILD_EXECUTABLE)
    return()
endif()

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
# Suppress gflags' own bundled tests (gflags_unittest etc.) — they are unrelated
# to bernardx and fail for environment reasons, cluttering ctest output.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE INTERNAL "")
add_subdirectory(${GFLAGS_DIR})

add_executable(bernardx src/main.cc)
target_link_libraries(bernardx PRIVATE bernardx_lib gflags)
target_include_directories(bernardx PRIVATE ${CMAKE_SOURCE_DIR}/src/lua ${CMAKE_SOURCE_DIR}/src/bt)
target_include_directories(bernardx SYSTEM PRIVATE ${CINATRA_DIR}/include)
cinatra_use_third_party_headers(bernardx)

# ── Tests ────────────────────────────────────────────────────────────────────

if(BERNARDX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(${GOOGLETEST_DIR})

    file(GLOB TEST_SOURCES tests/*.cc)
    add_executable(bernardx_test ${TEST_SOURCES})
    target_link_libraries(bernardx_test PRIVATE bernardx_lib GTest::gtest_main)
    target_include_directories(bernardx_test PRIVATE ${CMAKE_SOURCE_DIR}/src/lua ${CMAKE_SOURCE_DIR}/src/bt)
    target_include_directories(bernardx_test SYSTEM PRIVATE ${CINATRA_DIR}/include)
    cinatra_use_third_party_headers(bernardx_test)

    include(GoogleTest)
    gtest_discover_tests(bernardx_test)
endif()

# ── Install ──────────────────────────────────────────────────────────────────

include(GNUInstallDirs)

install(TARGETS bernardx
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
