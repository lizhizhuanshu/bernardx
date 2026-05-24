cmake_minimum_required(VERSION 3.26)

project(bernardx LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ── Dependencies ────────────────────────────────────────────────────────────

# asio (header-only, needed by async_simple and cinatra)
add_definitions(-DASIO_STANDALONE)
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-36-0
    GIT_SUBMODULES ""
    GIT_SUBMODULES_RECURSE OFF
)
FetchContent_MakeAvailable(asio)

# async_simple (coroutine + executor)
set(ASYNC_SIMPLE_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ASYNC_SIMPLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ASYNC_SIMPLE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ASYNC_SIMPLE_ENABLE_BENCHMARKS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    async_simple
    GIT_REPOSITORY https://github.com/lizhizhuanshu/async_simple.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(async_simple)

# Lua (static library from source)
FetchContent_Declare(
    lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG        v5.4.8
)
FetchContent_MakeAvailable(lua)
file(GLOB LUA_SOURCES ${lua_SOURCE_DIR}/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX ".*ltests\\.c|.*onelua\\.c")
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC ${lua_SOURCE_DIR})
set_property(TARGET lua PROPERTY C_STANDARD 99)
if(UNIX)
    target_link_libraries(lua PRIVATE dl m)
endif()

# sol2 (header-only, uses fork with local modifications)
set(SOL2_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    sol2
    GIT_REPOSITORY https://github.com/lizhizhuanshu/sol2.git
    GIT_TAG        v3.5.0
)
FetchContent_MakeAvailable(sol2)

# spdlog (header-only)
set(SPDLOG_HEADER_ONLY ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.17.0
)
FetchContent_MakeAvailable(spdlog)

# OpenSSL (from source)
FetchContent_Declare(
    openssl
    GIT_REPOSITORY https://github.com/openssl/openssl.git
    GIT_TAG        openssl-3.4.0
)
FetchContent_MakeAvailable(openssl)

# cinatra (HTTP client, header-only) -- reuses asio fetched above
set(ENABLE_SSL ON CACHE BOOL "" FORCE)
set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_PRESS_TOOL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    cinatra
    GIT_REPOSITORY https://github.com/lizhizhuanshu/cinatra.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(cinatra)

# nlohmann/json (header-only)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(json)

# ── BernardX library ─────────────────────────────────────────────────────

add_library(bernardx_lib STATIC
    src/lua/lua_runtime.cc
    src/lua/http_library.cc
    src/lua/json_library.cc
    src/lua/blackboard_library.cc
    src/lua/bt_library.cc
    src/bt/behavior_tree_engine.cc
    src/bt/tree_parser.cc
    src/bt/script_node.cc
    src/bt/sensor.cc
    src/bt/node.cc
    src/bt/composite.cc
    src/bt/selector.cc
    src/bt/sequence.cc
    src/bt/parallel.cc
    src/bt/inverter.cc
    src/bt/force_success.cc
    src/bt/force_failure.cc
    src/bt/blackboard_condition.cc
    src/bt/repeat.cc
    src/bt/retry_until_successful.cc
    src/bt/random_selector.cc
    src/bt/random_sequence.cc
    src/bt/wait_node.cc
)
target_include_directories(bernardx_lib PUBLIC src/lua src/bt)
cinatra_use_third_party_headers(bernardx_lib)
target_include_directories(bernardx_lib SYSTEM PRIVATE ${cinatra_SOURCE_DIR}/include)
target_compile_definitions(bernardx_lib PUBLIC CINATRA_ENABLE_SSL)
find_package(Threads REQUIRED)
target_link_libraries(bernardx_lib PUBLIC
    sol2::sol2
    spdlog::spdlog_header_only
    lua
    async_simple
    nlohmann_json::nlohmann_json
)
target_link_libraries(bernardx_lib PRIVATE
    Threads::Threads
    ssl
    crypto
    dl
)

# ── bernardx executable ────────────────────────────────────────────────────

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE INTERNAL "")
FetchContent_Declare(
    gflags
    GIT_REPOSITORY https://github.com/gflags/gflags.git
    GIT_TAG        v2.2.2
)
FetchContent_MakeAvailable(gflags)

add_executable(bernardx src/main.cc)
target_link_libraries(bernardx PRIVATE bernardx_lib gflags)
target_include_directories(bernardx PRIVATE ${CMAKE_SOURCE_DIR}/src/lua ${CMAKE_SOURCE_DIR}/src/bt)
target_include_directories(bernardx SYSTEM PRIVATE ${cinatra_SOURCE_DIR}/include)
cinatra_use_third_party_headers(bernardx)



# ── Tests ──────────────────────────────────────────────────────────────────

enable_testing()
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
FetchContent_MakeAvailable(googletest)

file(GLOB TEST_SOURCES tests/*.cc)
add_executable(bernardx_test ${TEST_SOURCES})
target_link_libraries(bernardx_test PRIVATE bernardx_lib GTest::gtest_main)
target_include_directories(bernardx_test PRIVATE ${CMAKE_SOURCE_DIR}/src/lua ${CMAKE_SOURCE_DIR}/src/bt)
target_include_directories(bernardx_test SYSTEM PRIVATE ${cinatra_SOURCE_DIR}/include)
cinatra_use_third_party_headers(bernardx_test)

include(GoogleTest)
gtest_discover_tests(bernardx_test)

# ── Install ───────────────────────────────────────────────────────────────

include(GNUInstallDirs)

install(TARGETS bernardx
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
