cmake_minimum_required(VERSION 3.20)
project(libriak-c VERSION 2.0.0 LANGUAGES C
        DESCRIPTION "C client for the Riak KV HTTP API")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(RIAK_BUILD_TESTS "Build the test suite" ${PROJECT_IS_TOP_LEVEL})
option(RIAK_BUILD_EXAMPLES "Build the examples" ${PROJECT_IS_TOP_LEVEL})
option(RIAK_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(RIAK_WERROR "Treat warnings as errors" OFF)
option(RIAK_FUZZ "Build libFuzzer targets (clang only)" OFF)
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

if(RIAK_FUZZ)
  # instrument the library too, so libFuzzer sees coverage inside it
  target_compile_options(riak_warnings INTERFACE -fsanitize=fuzzer-no-link)
endif()
if(RIAK_COVERAGE)
  target_compile_options(riak_warnings INTERFACE --coverage -O0)
  target_link_options(riak_warnings INTERFACE --coverage)
endif()

set(RIAK_SOURCES
  src/bucket.c
  src/client.c
  src/http.c
  src/kv.c
  src/multipart.c
  src/parse.c
  src/util.c)

# Object library shared by the shared/static targets and the unit tests,
# which need the internal symbols that the shared library hides.
add_library(riak_objects OBJECT ${RIAK_SOURCES})
target_include_directories(riak_objects PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>)
target_compile_definitions(riak_objects PRIVATE RIAK_BUILDING _POSIX_C_SOURCE=200809L)
target_link_libraries(riak_objects PUBLIC CURL::libcurl json-c::json-c
                      PRIVATE $<BUILD_INTERFACE:riak_warnings>)

add_library(riak SHARED $<TARGET_OBJECTS:riak_objects>)
add_library(riak_static STATIC $<TARGET_OBJECTS:riak_objects>)
foreach(t riak riak_static)
  target_include_directories(${t} PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(${t} PRIVATE CURL::libcurl json-c::json-c
                        $<BUILD_INTERFACE:riak_warnings>)
  set_target_properties(${t} PROPERTIES OUTPUT_NAME riak-c)
endforeach()
target_compile_definitions(riak INTERFACE RIAK_SHARED)
set_target_properties(riak PROPERTIES
  VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
add_library(riak::riak ALIAS riak)
add_library(riak::riak_static ALIAS riak_static)

install(TARGETS riak EXPORT riakTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT runtime
          NAMELINK_COMPONENT dev
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime)
install(TARGETS riak_static EXPORT riakTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev)
install(DIRECTORY include/riak DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev)
install(EXPORT riakTargets NAMESPACE riak::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libriak-c COMPONENT dev)
configure_package_config_file(cmake/libriak-c-config.cmake.in
  ${PROJECT_BINARY_DIR}/libriak-c-config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libriak-c)
write_basic_package_version_file(${PROJECT_BINARY_DIR}/libriak-c-config-version.cmake
  COMPATIBILITY SameMajorVersion)
install(FILES ${PROJECT_BINARY_DIR}/libriak-c-config.cmake
              ${PROJECT_BINARY_DIR}/libriak-c-config-version.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libriak-c COMPONENT dev)
configure_file(cmake/libriak-c.pc.in ${PROJECT_BINARY_DIR}/libriak-c.pc @ONLY)
install(FILES ${PROJECT_BINARY_DIR}/libriak-c.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig COMPONENT dev)
install(FILES README.md CHANGELOG.md
  DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT runtime)

if(RIAK_BUILD_EXAMPLES)
  add_executable(riak_example examples/basic.c)
  target_link_libraries(riak_example PRIVATE riak riak_warnings)
endif()

if(RIAK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

include(cmake/Packaging.cmake)
