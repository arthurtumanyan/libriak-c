# The sources except entry.c form a static library, linked by the riakctl
# binary and by tests/test_riakctl.c.
add_library(riakctl_core STATIC
  args.c
  cmd_aae.c
  cmd_backup.c
  cmd_dt.c
  cmd_kv.c
  cmd_props.c
  cmd_query.c
  cmd_repl.c
  cmd_search.c
  conn.c
  main.c
  out.c)
target_include_directories(riakctl_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(riakctl_core PUBLIC _POSIX_C_SOURCE=200809L)
find_package(Threads REQUIRED)
target_link_libraries(riakctl_core PUBLIC riak json-c::json-c Threads::Threads PRIVATE riak_warnings)
# stricter than the library: implicit conversions are errors with RIAK_WERROR
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(riakctl_core PRIVATE -Wconversion -Wsign-conversion)
endif()

add_executable(riakctl entry.c)
target_link_libraries(riakctl PRIVATE riakctl_core riak_warnings)
install(TARGETS riakctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT tools)
configure_file(riakctl.1.in ${CMAKE_CURRENT_BINARY_DIR}/riakctl.1 @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/riakctl.1
  DESTINATION ${CMAKE_INSTALL_MANDIR}/man1 COMPONENT tools)
# deb transitional package libriak-c-tools (the former name of the tools package)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/deb/libriak-c-tools.txt
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/libriak-c-tools RENAME README
  COMPONENT transitional EXCLUDE_FROM_ALL)
