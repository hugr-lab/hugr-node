cmake_minimum_required(VERSION 3.5)

# hugr_node: the platform node's extension - duckdb-acl's whole acl (a submodule, built in through its
# acl_sources.cmake, duckdb-acl spec 127) plus the platform part this repository owns (the cluster profile, node
# groups' traits, drift, the deployment, the cluster bundle), plugged into acl's registration points (spec 126).
set(TARGET_NAME hugr_node)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ours first: the platform part's headers win over a copy duckdb-acl may still carry
include_directories(${CMAKE_CURRENT_LIST_DIR}/src/include)

# acl is built in, not loaded beside: its entry point is ours (ACL_EMBEDDED)
add_definitions(-DACL_EMBEDDED)
include(${CMAKE_CURRENT_LIST_DIR}/duckdb-acl/acl_sources.cmake)
# the platform part is ours - a duckdb-acl that still carries its copy (before its spec 127 PR B) contributes none
list(REMOVE_ITEM EXTENSION_SOURCES ${ACL_ROOT}/src/acl_cluster.cpp)
list(APPEND EXTENSION_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/src/hugr_node_extension.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/acl_cluster.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})
acl_configure_targets(${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
