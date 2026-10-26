cmake_minimum_required(VERSION 3.20)
project(libriak-c VERSION 2.0.0 LANGUAGES C
        DESCRIPTION "C client for the Riak KV HTTP API")

include(GNUInstallDirs)

option(RIAK_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(RIAK_WERROR "Treat warnings as errors" OFF)
option(RIAK_COVERAGE "Instrument for code coverage" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(CURL 7.62 REQUIRED)
find_package(json-c CONFIG QUIET)
if(NOT TARGET json-c::json-c)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c>=0.15)
  add_library(json-c::json-c ALIAS PkgConfig::JSONC)
endif()

add_library(riak_warnings INTERFACE)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(riak_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wstrict-prototypes
    -Wmissing-prototypes -Wcast-qual -Wnull-dereference -Wvla
    $<$<BOOL:${RIAK_WERROR}>:-Werror>)
  target_compile_definitions(riak_warnings INTERFACE
    $<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=2>)
endif()
if(RIAK_SANITIZE)
  target_compile_options(riak_warnings INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(riak_warnings INTERFACE -fsanitize=address,undefined)
endif()

if(RIAK_COVERAGE)
  target_compile_options(riak_warnings INTERFACE --coverage -O0)
  target_link_options(riak_warnings INTERFACE --coverage)
endif()

install(DIRECTORY include/riak DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
