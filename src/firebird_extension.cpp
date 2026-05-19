#define DUCKDB_EXTENSION_MAIN

#include "firebird_extension.hpp"
#include "firebird_scanner.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
    loader.RegisterFunction(GetFirebirdScanFunction());
}

void FirebirdExtension::Load(ExtensionLoader &loader) {
    LoadInternal(loader);
}

std::string FirebirdExtension::Name() {
    return "firebird";
}

std::string FirebirdExtension::Version() const {
#ifdef EXT_VERSION_FIREBIRD
    return EXT_VERSION_FIREBIRD;
#else
    return "0.1.0";
#endif
}

} // namespace duckdb

// --- C entry points -----------------------------------------------------------
// DuckDB ≥ 1.4 calls the *_duckdb_cpp_init symbol declared by this macro.
extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(firebird, loader) {
    duckdb::LoadInternal(loader);
}

DUCKDB_EXTENSION_API const char *firebird_version() {
    return duckdb::DuckDB::LibraryVersion();
}

} // extern "C"

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
