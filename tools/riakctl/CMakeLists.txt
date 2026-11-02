add_executable(riakctl
  args.c
  cmd_backup.c
  cmd_kv.c
  conn.c
  main.c
  out.c)
target_compile_definitions(riakctl PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(riakctl PRIVATE riak json-c::json-c riak_warnings)
install(TARGETS riakctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT tools)
