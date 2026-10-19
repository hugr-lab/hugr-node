cmake_minimum_required(VERSION 3.5)

# hugr_node: the platform node's extension - duckdb-acl's whole acl (a submodule, built in through its
# acl_sources.cmake, duckdb-acl spec 127), tresor's whole client of the secrets service (a submodule, built in through
# its tresor_sources.cmake, tresor spec 024 - spec 002: a platform node's secrets live in the service), acl_otel's
# whole OpenTelemetry export (a submodule, built in through its acl_otel_sources.cmake, acl-otel spec 024 - spec 005)
# and the platform part this repository owns (the cluster profile, node groups' traits, drift, the cluster bundle),
# plugged into acl's registration points (duckdb-acl spec 126).
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
# tresor too (TRESOR_EMBEDDED: its entry point and TresorExtension left out, tresor_version() says tresor's version).
# After acl's file: tresor's names its own copy of the OIDC core (oidc.cmake again, its path) with tresor's namespace
# on those sources, and checks that both submodules carry the same duckdb-ext-common headers (ACL_ROOT)
set(TRESOR_EMBEDDED ON)
include(${CMAKE_CURRENT_LIST_DIR}/tresor/tresor_sources.cmake)
# acl_otel too (spec 005, acl-otel spec 024: ACL_OTEL_EMBEDDED - its entry point and AclOtelExtension left out,
# acl_otel_version() says acl-otel's version). The OpenTelemetry SDK comes with it (vcpkg.json); with no endpoint
# configured it exports nothing. Its contracts and hooks must be acl's and tresor's files (checked at configure)
set(ACL_OTEL_EMBEDDED ON)
include(${CMAKE_CURRENT_LIST_DIR}/acl-otel/acl_otel_sources.cmake)
# spec 011: the `hugr` catalog type - quack's client transport (the copies src/hugr_type/sync.py makes) and the type's
# own catalog, pool and scan. The protocol it speaks is compiled once, by acl's embedded quack server (its message,
# uri and log TUs), so the type needs the embed. Hidden symbols, as the embed's: a stock quack loaded beside must not
# bind its calls to our copies
if(NOT ACL_QUACK_EMBED_ENABLED)
    message(FATAL_ERROR "hugr_node: the hugr type needs acl's embedded quack server (unset ACL_NO_QUACK_EMBED)")
endif()
set(HUGR_TYPE_DIR ${CMAKE_CURRENT_LIST_DIR}/src/hugr_type)
include_directories(${HUGR_TYPE_DIR}/include)
set(HUGR_TYPE_SOURCES
    ${HUGR_TYPE_DIR}/hugr_client.cpp
    ${HUGR_TYPE_DIR}/hugr_client_affinity.cpp
    ${HUGR_TYPE_DIR}/hugr_client_fetch_ahead.cpp
    ${HUGR_TYPE_DIR}/hugr_pool.cpp
    ${HUGR_TYPE_DIR}/hugr_remote.cpp
    ${HUGR_TYPE_DIR}/hugr_catalog.cpp
    ${HUGR_TYPE_DIR}/hugr_scan.cpp
    ${HUGR_TYPE_DIR}/hugr_type.cpp)
if(NOT MSVC)
    set_source_files_properties(${HUGR_TYPE_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
endif()

list(APPEND EXTENSION_SOURCES
    ${HUGR_TYPE_SOURCES}
    ${TRESOR_SOURCES}
    ${ACL_OTEL_SOURCES}
    ${CMAKE_CURRENT_LIST_DIR}/src/hugr_node_extension.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/acl_cluster.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/hugr_node_agent.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})
acl_configure_targets(${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
tresor_configure_targets(${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
acl_otel_configure_targets(${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
