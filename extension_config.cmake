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

# spec 006: httpfs and azure are linked in (owner, 2026-10-15: until duckdb 2.0) - httpfs because the bootstrap reads
# the extension repository's discovery and the control plane over https, azure because the `azure` secret type mssql
# reads comes from it. From the submodule's own pins. The loadable hugr_node artifact stays the one extension: linking
# here is what the node's CLI (the image) and the test binary carry.
if(NOT MINGW AND NOT ${WASM_ENABLED})
    include(${CMAKE_CURRENT_LIST_DIR}/duckdb/.github/config/extensions/httpfs.cmake)
    include(${CMAKE_CURRENT_LIST_DIR}/duckdb/.github/config/extensions/azure.cmake)
    duckdb_extension_statically_link(httpfs azure)
endif()

# spec 004: the dev build (HUGR_NODE_DEV=1) - json and autocomplete linked in as well, what a node for the client tools
# needs (acl-clients' dev/node.sh; duckdb-acl links the same under ACL_QUACK). Off in CI and in the distribution build.
if(DEFINED ENV{HUGR_NODE_DEV} AND NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(json)
    duckdb_extension_load(autocomplete)
    duckdb_extension_statically_link(json autocomplete)
endif()

# spec 006: the bootstrap e2e (HUGR_NODE_E2E=1) - postgres_scanner built as a LOADABLE (never linked) from the submodule's
# pin, published by the e2e into its repository and installed by the node from there, as a profile extension would be
if(DEFINED ENV{HUGR_NODE_E2E} AND NOT MINGW AND NOT ${WASM_ENABLED})
    include(${CMAKE_CURRENT_LIST_DIR}/duckdb/.github/config/extensions/postgres_scanner.cmake)
endif()
