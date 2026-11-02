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
