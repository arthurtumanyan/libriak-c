find_package(Python3 COMPONENTS Interpreter)

foreach(name util parse multipart datatype api)
  add_executable(test_${name} test_${name}.c)
  target_link_libraries(test_${name} PRIVATE riak_objects riak_warnings)
  add_test(NAME ${name} COMMAND test_${name})
endforeach()

add_executable(test_client test_client.c)
target_link_libraries(test_client PRIVATE riak CURL::libcurl json-c::json-c riak_warnings)
if(Python3_Interpreter_FOUND)
  add_test(NAME client
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/stub_riak.py
            $<TARGET_FILE:test_client>)
  set_tests_properties(client PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()

add_executable(test_async test_async.c)
target_link_libraries(test_async PRIVATE riak CURL::libcurl riak_warnings)
if(Python3_Interpreter_FOUND)
  add_test(NAME async
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/stub_riak.py
            $<TARGET_FILE:test_async>)
  set_tests_properties(async PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()

find_program(OPENSSL_EXECUTABLE openssl)
add_executable(test_tls test_tls.c)
target_link_libraries(test_tls PRIVATE riak riak_warnings)
if(Python3_Interpreter_FOUND AND OPENSSL_EXECUTABLE)
  set(RIAK_TEST_CERTS ${CMAKE_CURRENT_BINARY_DIR}/certs)
  add_test(NAME tls_certs COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/gen_certs.sh ${RIAK_TEST_CERTS})
  set_tests_properties(tls_certs PROPERTIES FIXTURES_SETUP certs)
  add_test(NAME tls
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/stub_riak.py --tls ${RIAK_TEST_CERTS}
            $<TARGET_FILE:test_tls>)
  set_tests_properties(tls PROPERTIES FIXTURES_REQUIRED certs SKIP_RETURN_CODE 77 TIMEOUT 60
                       ENVIRONMENT RIAK_TEST_CERTS=${RIAK_TEST_CERTS})
endif()

add_executable(test_live test_live.c)
target_link_libraries(test_live PRIVATE riak riak_warnings)
add_test(NAME live COMMAND test_live)
set_tests_properties(live PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

if(RIAK_FUZZ)
  add_executable(fuzz_parse fuzz_parse.c)
  target_compile_options(fuzz_parse PRIVATE -fsanitize=fuzzer)
  target_link_options(fuzz_parse PRIVATE -fsanitize=fuzzer)
  target_link_libraries(fuzz_parse PRIVATE riak_objects riak_warnings)
endif()
