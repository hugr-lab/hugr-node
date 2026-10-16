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

# The profile's extensions as LOADABLES (never linked): what a node installs from a repository at start (spec 006).
# Their pins are the ones duckdb-acl tests at its pin - read from its extension_config.cmake, so the node ships the
# combination acl's suite and its door e2e prove (spec 008).
# A macro, not a function: duckdb_extension_load registers in the scope it is called from, and a function's scope ends
# with it (the extension was announced and then never built)
macro(hugr_node_load_from_acl _hn_name)
    file(READ ${CMAKE_CURRENT_LIST_DIR}/duckdb-acl/extension_config.cmake _hn_acl_config)
    string(REGEX MATCH "duckdb_extension_load\\(${_hn_name}[^)]*" _hn_block "${_hn_acl_config}")
    string(REGEX MATCH "GIT_URL ([^ \n]+)" _hn_match "${_hn_block}")
    set(_hn_url ${CMAKE_MATCH_1})
    string(REGEX MATCH "GIT_TAG ([0-9a-f]+)" _hn_match "${_hn_block}")
    set(_hn_tag ${CMAKE_MATCH_1})
    if(NOT _hn_url OR NOT _hn_tag)
        message(FATAL_ERROR "hugr_node: duckdb-acl's extension_config.cmake names no GIT_URL/GIT_TAG for ${_hn_name}")
    endif()
    if(_hn_block MATCHES "SUBMODULES ([^ \n]+)")
        duckdb_extension_load(${_hn_name} GIT_URL ${_hn_url} GIT_TAG ${_hn_tag} SUBMODULES ${CMAKE_MATCH_1})
    else()
        duckdb_extension_load(${_hn_name} GIT_URL ${_hn_url} GIT_TAG ${_hn_tag})
    endif()
endmacro()

# spec 006: the bootstrap e2e (HUGR_NODE_E2E=1) publishes postgres_scanner into its repository and the node installs it
# from there; spec 008: the release (HUGR_NODE_RELEASE=1) builds the three a profile installs - postgres_scanner,
# mssql, ducklake - for the release's assets
if((DEFINED ENV{HUGR_NODE_E2E} OR DEFINED ENV{HUGR_NODE_RELEASE}) AND NOT MINGW AND NOT ${WASM_ENABLED})
    hugr_node_load_from_acl(postgres_scanner)
endif()
if(DEFINED ENV{HUGR_NODE_RELEASE} AND NOT MINGW AND NOT ${WASM_ENABLED})
    hugr_node_load_from_acl(mssql)
    hugr_node_load_from_acl(ducklake)
endif()
