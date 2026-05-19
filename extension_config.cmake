# DuckDB out-of-tree extension config.
# Consumed by extension-ci-tools/makefiles/duckdb_extension.Makefile.

duckdb_extension_load(firebird
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
