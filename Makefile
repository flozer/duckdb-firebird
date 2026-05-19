# DuckDB out-of-tree extension makefile.
#
# Delegates to extension-ci-tools (the standard DuckDB extension build harness),
# which pulls in a pinned DuckDB checkout and builds both the static and
# loadable variants of the extension.
#
# First-time setup:
#     git submodule add https://github.com/duckdb/duckdb.git
#     git submodule add https://github.com/duckdb/extension-ci-tools.git
#     git submodule update --init --recursive
#
# Common targets:
#     make debug         build with debug symbols
#     make release       optimized build
#     make test_debug    run SQL tests against the debug build
#     make clean

PROJ_DIR  := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
EXT_NAME  := firebird
EXT_CONFIG := $(PROJ_DIR)extension_config.cmake

include extension-ci-tools/makefiles/duckdb_extension.Makefile
