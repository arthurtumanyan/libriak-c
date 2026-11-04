# Read by cpack for each generator: the transitional libriak-c-tools package
# exists for deb only (rpm replaces the old name through Obsoletes).
# cpack runs this without the project's policies, hence list(FIND) over IN_LIST.
list(FIND CPACK_COMPONENTS_ALL tools riak_tools_index)
if(CPACK_GENERATOR STREQUAL "DEB" AND NOT riak_tools_index EQUAL -1)
  list(APPEND CPACK_COMPONENTS_ALL transitional)
endif()
