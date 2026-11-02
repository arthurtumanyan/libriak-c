add_executable(riakctl
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
target_compile_definitions(riakctl PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(riakctl PRIVATE riak json-c::json-c riak_warnings)
install(TARGETS riakctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT tools)
configure_file(riakctl.1.in ${CMAKE_CURRENT_BINARY_DIR}/riakctl.1 @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/riakctl.1
  DESTINATION ${CMAKE_INSTALL_MANDIR}/man1 COMPONENT tools)
