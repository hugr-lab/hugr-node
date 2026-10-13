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
