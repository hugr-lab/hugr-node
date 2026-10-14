# This file is included by DuckDB's build system. It specifies which extension to load.

# duckdb-acl's own (spec 106): clang-cl with MSVC-built libraries - string pooling off for what clang-cl compiles
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(/GF-)
endif()

duckdb_extension_load(hugr_node
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
duckdb_extension_statically_link(hugr_node)

# icu, as duckdb-acl's test binary carries it (spec 068's client-local rendering settings)
duckdb_extension_load(icu)
duckdb_extension_statically_link(icu)

# spec 004: the dev build (HUGR_NODE_DEV=1) - json, autocomplete and httpfs linked in, what a node for the client tools
# needs (acl-clients' dev/node.sh: an IdP's discovery and keys are read through httpfs; duckdb-acl links the same three
# under ACL_QUACK). Off in CI and in the distribution build: hugr_node's artifact is the one extension.
if(DEFINED ENV{HUGR_NODE_DEV} AND NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(json)
    duckdb_extension_load(autocomplete)
    include(${CMAKE_CURRENT_LIST_DIR}/duckdb/.github/config/extensions/httpfs.cmake)
    duckdb_extension_statically_link(json autocomplete httpfs)
endif()
